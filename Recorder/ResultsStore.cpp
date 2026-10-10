#include "ResultsStore.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>

#include "Catalog.hpp"
#include "Digest.hpp"
#include "Limits.hpp"

namespace recorder {

namespace {

constexpr int SCHEMA_VERSION = 2;

static_assert(limits::MAX_STEPS == 4, "the orders tables have a column pair for each of four steps");

constexpr const char* SCHEMA = R"sql(
CREATE TABLE accelerators (
    id             INTEGER PRIMARY KEY,
    name           TEXT NOT NULL UNIQUE,
    vendor         TEXT NOT NULL,
    chain_position INTEGER NOT NULL
);
CREATE TABLE interconnects (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
CREATE TABLE regions       (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);

CREATE TABLE batches (
    batch_id       INTEGER NOT NULL PRIMARY KEY,
    status         INTEGER NOT NULL CHECK (status IN (0, 1)),
    void_reason    INTEGER NOT NULL,
    first_hour     INTEGER NOT NULL,
    event_seq      INTEGER NOT NULL,
    bids           INTEGER NOT NULL,
    asks           INTEGER NOT NULL,
    arcs           INTEGER NOT NULL,
    pivots         INTEGER NOT NULL,
    units          INTEGER NOT NULL,
    lifted_units   INTEGER NOT NULL,
    welfare        INTEGER NOT NULL,
    rank_sum       INTEGER NOT NULL,
    fills          INTEGER NOT NULL,
    certified      INTEGER NOT NULL,
    solver_version INTEGER NOT NULL,
    digest         BLOB NOT NULL CHECK (length(digest) = 32)
) WITHOUT ROWID;

CREATE TABLE orders (
    batch_id          INTEGER NOT NULL,
    side              INTEGER NOT NULL CHECK (side IN (0, 1)),
    rank              INTEGER NOT NULL,
    participant_id    INTEGER NOT NULL,
    order_token       INTEGER NOT NULL,
    accelerator_mask  INTEGER NOT NULL,
    interconnect_mask INTEGER NOT NULL,
    region_mask       INTEGER NOT NULL,
    slot_mask         INTEGER NOT NULL,
    per_slot_cap      INTEGER NOT NULL,
    total_units       INTEGER NOT NULL,
    step_count        INTEGER NOT NULL CHECK (step_count BETWEEN 1 AND 4),
    q1 INTEGER NOT NULL, p1 INTEGER NOT NULL,
    q2 INTEGER, p2 INTEGER,
    q3 INTEGER, p3 INTEGER,
    q4 INTEGER, p4 INTEGER,
    filled_units      INTEGER NOT NULL,
    lifted_units      INTEGER NOT NULL,
    amount            INTEGER NOT NULL,
    PRIMARY KEY (batch_id, side, rank)
) WITHOUT ROWID;

CREATE INDEX orders_by_participant ON orders (participant_id, batch_id, side, rank);

CREATE TABLE fills (
    batch_id    INTEGER NOT NULL,
    side        INTEGER NOT NULL CHECK (side IN (0, 1)),
    rank        INTEGER NOT NULL,
    hour        INTEGER NOT NULL,
    accelerator INTEGER NOT NULL,
    interconnect INTEGER NOT NULL,
    region      INTEGER NOT NULL,
    price       INTEGER NOT NULL,
    units       INTEGER NOT NULL,
    lifted      INTEGER NOT NULL,
    PRIMARY KEY (batch_id, side, rank, hour, accelerator, interconnect, region)
) WITHOUT ROWID;

CREATE TABLE accelerator_prices (
    batch_id    INTEGER NOT NULL,
    hour        INTEGER NOT NULL,
    accelerator INTEGER NOT NULL,
    units       INTEGER NOT NULL,
    amount      INTEGER NOT NULL,
    price_min   INTEGER NOT NULL,
    price_max   INTEGER NOT NULL,
    PRIMARY KEY (batch_id, hour, accelerator)
) WITHOUT ROWID;

CREATE INDEX accelerator_prices_by_hour ON accelerator_prices (accelerator, hour, batch_id);

CREATE TABLE live (
    id         INTEGER PRIMARY KEY CHECK (id = 1),
    batch_id   INTEGER NOT NULL,
    first_hour INTEGER NOT NULL,
    event_seq  INTEGER NOT NULL
);

CREATE TABLE open_orders (
    side              INTEGER NOT NULL CHECK (side IN (0, 1)),
    rank              INTEGER NOT NULL,
    participant_id    INTEGER NOT NULL,
    order_token       INTEGER NOT NULL,
    accelerator_mask  INTEGER NOT NULL,
    interconnect_mask INTEGER NOT NULL,
    region_mask       INTEGER NOT NULL,
    slot_mask         INTEGER NOT NULL,
    per_slot_cap      INTEGER NOT NULL,
    total_units       INTEGER NOT NULL,
    step_count        INTEGER NOT NULL CHECK (step_count BETWEEN 1 AND 4),
    q1 INTEGER NOT NULL, p1 INTEGER NOT NULL,
    q2 INTEGER, p2 INTEGER,
    q3 INTEGER, p3 INTEGER,
    q4 INTEGER, p4 INTEGER,
    PRIMARY KEY (side, rank)
) WITHOUT ROWID;

CREATE INDEX open_orders_by_participant ON open_orders (participant_id, side, rank);
)sql";

std::int64_t toDb(std::uint64_t v) { return static_cast<std::int64_t>(v); }
std::uint64_t fromDb(std::int64_t v) { return static_cast<std::uint64_t>(v); }

[[noreturn]] void fail(sqlite3* db, const std::string& what, int code) {
    throw StoreError(what + ": " + (db ? sqlite3_errmsg(db) : "no connection"), code);
}

void exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const std::string text = message ? message : "unknown error";
        sqlite3_free(message);
        throw StoreError(std::string(sql).substr(0, 60) + ": " + text, rc);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        const int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &stmt_, nullptr);
        if (rc != SQLITE_OK) fail(db, "cannot prepare a statement", rc);
    }
    Statement(sqlite3* db, const std::string& sql) : Statement(db, sql.c_str()) {}
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, std::int64_t value) { check(sqlite3_bind_int64(stmt_, index, value), "bind"); }
    void bind(int index, std::uint64_t value) { bind(index, toDb(value)); }
    void bind(int index, std::uint32_t value) { bind(index, std::int64_t{ value }); }
    void bind(int index, std::uint8_t value) { bind(index, std::int64_t{ value }); }
    void bindNull(int index) { check(sqlite3_bind_null(stmt_, index), "bind"); }
    void bindText(int index, const std::string& text) {
        check(sqlite3_bind_text(stmt_, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT), "bind");
    }
    void bindBlob(int index, const void* data, int size) {
        check(sqlite3_bind_blob(stmt_, index, data, size, SQLITE_STATIC), "bind");
    }

    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        fail(db_, "cannot run a statement", rc);
    }

    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }

    bool isNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }
    std::int64_t i64(int column) const { return sqlite3_column_int64(stmt_, column); }
    std::uint32_t u32(int column) const { return static_cast<std::uint32_t>(sqlite3_column_int64(stmt_, column)); }
    std::uint8_t u8(int column) const { return static_cast<std::uint8_t>(sqlite3_column_int64(stmt_, column)); }
    std::uint64_t u64(int column) const { return fromDb(sqlite3_column_int64(stmt_, column)); }
    std::string text(int column) const {
        const unsigned char* t = sqlite3_column_text(stmt_, column);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    Digest blob(int column) const {
        Digest out{};
        const void* data = sqlite3_column_blob(stmt_, column);
        if (sqlite3_column_bytes(stmt_, column) != static_cast<int>(out.size()) || data == nullptr) {
            throw StoreError("a digest is not 32 bytes", SQLITE_CORRUPT);
        }
        std::memcpy(out.data(), data, out.size());
        return out;
    }

private:
    void check(int rc, const char* what) {
        if (rc != SQLITE_OK) fail(db_, what, rc);
    }

    sqlite3* db_;
    sqlite3_stmt* stmt_{ nullptr };
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {
        exec(db_, "BEGIN IMMEDIATE");
        open_ = true;
    }
    ~Transaction() {
        if (open_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        exec(db_, "COMMIT");
        open_ = false;
    }

private:
    sqlite3* db_;
    bool open_{ false };
};

std::string firstText(sqlite3* db, const std::string& sql) {
    Statement s(db, sql.c_str());
    return s.step() ? s.text(0) : "";
}

constexpr const char* BATCH_COLUMNS =
    "batch_id, status, void_reason, first_hour, event_seq, bids, asks, arcs, pivots, units, lifted_units, "
    "welfare, rank_sum, fills, certified, solver_version, digest";

BatchHeader readHeader(const Statement& s) {
    BatchHeader h;
    h.batchId = s.u64(0);
    h.status = static_cast<BatchStatus>(s.u8(1));
    h.voidReason = s.u8(2);
    h.firstHour = s.u32(3);
    h.eventSeq = s.u64(4);
    h.bids = s.u32(5);
    h.asks = s.u32(6);
    h.arcs = s.u32(7);
    h.pivots = s.u32(8);
    h.units = s.u32(9);
    h.liftedUnits = s.u32(10);
    h.welfare = s.i64(11);
    h.rankSum = s.i64(12);
    h.fills = s.u32(13);
    h.certified = s.u8(14);
    h.solverVersion = s.u32(15);
    h.digest = s.blob(16);
    return h;
}

constexpr int TERM_COLUMN_COUNT = 15;
constexpr int OPEN_ORDER_COLUMN_COUNT = 4 + TERM_COLUMN_COUNT;
constexpr int ORDER_COLUMN_COUNT = OPEN_ORDER_COLUMN_COUNT + 3;

// The columns that say what an order asked for, each with `prefix` before it.
std::string termColumns(const std::string& p) {
    return p + "accelerator_mask, " + p + "interconnect_mask, " + p + "region_mask, " + p + "slot_mask, " + p +
           "per_slot_cap, " + p + "total_units, " + p + "step_count, " + p + "q1, " + p + "p1, " + p + "q2, " + p +
           "p2, " + p + "q3, " + p + "p3, " + p + "q4, " + p + "p4";
}

std::string openOrderColumns(const std::string& p) {
    return p + "side, " + p + "rank, " + p + "participant_id, " + p + "order_token, " + termColumns(p);
}

std::string orderColumns(const std::string& p) {
    return openOrderColumns(p) + ", " + p + "filled_units, " + p + "lifted_units, " + p + "amount";
}

std::string placeholders(int count) {
    std::string out;
    for (int i = 1; i <= count; ++i) out += (i > 1 ? ", ?" : "?") + std::to_string(i);
    return out;
}

void bindTerms(Statement& s, int first, const OrderTerms& t) {
    s.bind(first, t.acceleratorMask);
    s.bind(first + 1, t.interconnectMask);
    s.bind(first + 2, t.regionMask);
    s.bind(first + 3, t.slotMask);
    s.bind(first + 4, t.perSlotCap);
    s.bind(first + 5, t.totalUnits);
    s.bind(first + 6, t.stepCount);
    for (std::size_t k = 0; k < t.stepQuantity.size(); ++k) {
        const int at = first + 7 + 2 * static_cast<int>(k);
        if (k < t.stepCount) {
            s.bind(at, t.stepQuantity[k]);
            s.bind(at + 1, t.stepPrice[k]);
        } else {
            s.bindNull(at);
            s.bindNull(at + 1);
        }
    }
}

// An absent step is NULL in the table and zero here.
OrderTerms readTerms(const Statement& s, int first) {
    OrderTerms t;
    t.acceleratorMask = s.u32(first);
    t.interconnectMask = s.u8(first + 1);
    t.regionMask = s.u32(first + 2);
    t.slotMask = s.u64(first + 3);
    t.perSlotCap = s.u32(first + 4);
    t.totalUnits = s.u32(first + 5);
    t.stepCount = s.u8(first + 6);
    for (std::size_t k = 0; k < t.stepQuantity.size(); ++k) {
        const int at = first + 7 + 2 * static_cast<int>(k);
        t.stepQuantity[k] = s.u32(at);
        t.stepPrice[k] = s.u32(at + 1);
    }
    return t;
}

// The columns of orderColumns(), starting at `first`.
OrderRow readOrder(const Statement& s, int first) {
    OrderRow r;
    r.side = static_cast<RowSide>(s.u8(first));
    r.rank = s.u32(first + 1);
    r.participant = s.u32(first + 2);
    r.token = s.u64(first + 3);
    r.terms = readTerms(s, first + 4);
    r.filled = s.u32(first + OPEN_ORDER_COLUMN_COUNT);
    r.lifted = s.u32(first + OPEN_ORDER_COLUMN_COUNT + 1);
    r.amount = s.i64(first + OPEN_ORDER_COLUMN_COUNT + 2);
    return r;
}

// The columns of openOrderColumns(), starting at `first`.
OpenOrderRow readOpenOrder(const Statement& s, int first) {
    OpenOrderRow r;
    r.side = static_cast<RowSide>(s.u8(first));
    r.rank = s.u32(first + 1);
    r.participant = s.u32(first + 2);
    r.token = s.u64(first + 3);
    r.terms = readTerms(s, first + 4);
    return r;
}

constexpr const char* FILL_COLUMNS = "side, rank, hour, accelerator, interconnect, region, price, units, lifted";

FillRow readFill(const Statement& s) {
    return FillRow{ static_cast<RowSide>(s.u8(0)), s.u32(1), s.u32(2), s.u8(3), s.u8(4), s.u8(5), s.u32(6), s.u32(7), s.u8(8) };
}

constexpr const char* PRICE_COLUMNS = "hour, accelerator, units, amount, price_min, price_max";

AcceleratorPriceRow readPrice(const Statement& s, int first) {
    return AcceleratorPriceRow{ s.u32(first), s.u8(first + 1), s.u32(first + 2), s.i64(first + 3), s.u32(first + 4), s.u32(first + 5) };
}

const char* vendorName(catalog::Vendor vendor) {
    switch (vendor) {
    case catalog::Vendor::NVIDIA: return "NVIDIA";
    case catalog::Vendor::AMD: return "AMD";
    case catalog::Vendor::GOOGLE: return "GOOGLE";
    }
    return "?";
}

void writeCatalog(sqlite3* db) {
    {
        Statement s(db, "INSERT INTO accelerators (id, name, vendor, chain_position) VALUES (?1, ?2, ?3, ?4)");
        for (std::size_t i = 0; i < catalog::ACCELERATOR_COUNT; ++i) {
            const catalog::Accelerator& a = catalog::ACCELERATORS[i];
            s.bind(1, static_cast<std::int64_t>(i));
            s.bindText(2, a.name);
            s.bindText(3, vendorName(a.vendor));
            s.bind(4, std::int64_t{ a.chainPosition });
            s.step();
            s.reset();
        }
    }
    const auto names = [&](const char* table, const char* const* list, std::size_t count) {
        Statement s(db, std::string("INSERT INTO ") + table + " (id, name) VALUES (?1, ?2)");
        for (std::size_t i = 0; i < count; ++i) {
            s.bind(1, static_cast<std::int64_t>(i));
            s.bindText(2, list[i]);
            s.step();
            s.reset();
        }
    };
    names("interconnects", catalog::INTERCONNECTS, catalog::INTERCONNECT_COUNT);
    names("regions", catalog::REGIONS, catalog::REGION_COUNT);
}

// Whether the three small tables say what this build's catalog says: the numbers in the other tables mean
// nothing under another one.
bool catalogMatches(sqlite3* db) {
    {
        Statement s(db, "SELECT id, name, vendor, chain_position FROM accelerators ORDER BY id");
        for (std::size_t i = 0; i < catalog::ACCELERATOR_COUNT; ++i) {
            const catalog::Accelerator& a = catalog::ACCELERATORS[i];
            if (!s.step() || s.i64(0) != static_cast<std::int64_t>(i) || s.text(1) != a.name || s.text(2) != vendorName(a.vendor) ||
                s.i64(3) != std::int64_t{ a.chainPosition }) {
                return false;
            }
        }
        if (s.step()) return false;
    }
    const auto sameNames = [&](const char* table, const char* const* list, std::size_t count) {
        Statement s(db, std::string("SELECT id, name FROM ") + table + " ORDER BY id");
        for (std::size_t i = 0; i < count; ++i) {
            if (!s.step() || s.i64(0) != static_cast<std::int64_t>(i) || s.text(1) != list[i]) return false;
        }
        return !s.step();
    };
    return sameNames("interconnects", catalog::INTERCONNECTS, catalog::INTERCONNECT_COUNT) &&
           sameNames("regions", catalog::REGIONS, catalog::REGION_COUNT);
}

// SQLite orders the stored 64 bits as signed numbers, so batch ids from 2^63 up are negative there. Largest
// first as unsigned numbers is the negative range, then the rest, each from its top.
struct Region {
    std::int64_t lo;
    std::int64_t hi;
};
constexpr Region REGIONS_LARGEST_FIRST[] = { { std::numeric_limits<std::int64_t>::min(), -1 },
                                             { 0, std::numeric_limits<std::int64_t>::max() } };

constexpr std::size_t SQL_MAX = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());

// A negative LIMIT means no limit, and a negative OFFSET means none, so an offset is clamped instead.
std::int64_t limitOf(std::size_t limit) { return limit >= SQL_MAX ? -1 : static_cast<std::int64_t>(limit); }
std::int64_t offsetOf(std::size_t offset) { return static_cast<std::int64_t>(std::min(offset, SQL_MAX)); }

} // namespace

struct ResultsStore::Impl {
    sqlite3* db{ nullptr };
    std::unique_ptr<Statement> insertBatch;
    std::unique_ptr<Statement> insertOrder;
    std::unique_ptr<Statement> insertFill;
    std::unique_ptr<Statement> insertPrice;

    void resetStatements() {
        for (Statement* s : { insertBatch.get(), insertOrder.get(), insertFill.get(), insertPrice.get() }) {
            if (s) s->reset();
        }
    }

    ~Impl() {
        insertBatch.reset();
        insertOrder.reset();
        insertFill.reset();
        insertPrice.reset();
        if (db) sqlite3_close(db);
    }
};

namespace {

// False, with nothing changed, when the batch id is already recorded. A template only so that it can use
// the private Impl without naming it.
template <typename ImplT>
bool insertRecord(sqlite3* db, ImplT& impl, const BatchRecord& record, const Digest& digest) {
    const BatchHeader& h = record.header;
    Statement& b = *impl.insertBatch;
    b.bind(1, h.batchId);
    b.bind(2, static_cast<std::uint8_t>(h.status));
    b.bind(3, h.voidReason);
    b.bind(4, h.firstHour);
    b.bind(5, h.eventSeq);
    b.bind(6, h.bids);
    b.bind(7, h.asks);
    b.bind(8, h.arcs);
    b.bind(9, h.pivots);
    b.bind(10, h.units);
    b.bind(11, h.liftedUnits);
    b.bind(12, h.welfare);
    b.bind(13, h.rankSum);
    b.bind(14, h.fills);
    b.bind(15, h.certified);
    b.bind(16, h.solverVersion);
    b.bindBlob(17, digest.data(), static_cast<int>(digest.size()));
    b.step();
    b.reset();
    if (sqlite3_changes(db) == 0) return false;

    for (const OrderRow& r : record.orders) {
        Statement& s = *impl.insertOrder;
        s.bind(1, h.batchId);
        s.bind(2, static_cast<std::uint8_t>(r.side));
        s.bind(3, r.rank);
        s.bind(4, r.participant);
        s.bind(5, r.token);
        bindTerms(s, 6, r.terms);
        s.bind(6 + TERM_COLUMN_COUNT, r.filled);
        s.bind(7 + TERM_COLUMN_COUNT, r.lifted);
        s.bind(8 + TERM_COLUMN_COUNT, r.amount);
        s.step();
        s.reset();
    }
    for (const FillRow& r : record.fills) {
        Statement& s = *impl.insertFill;
        s.bind(1, h.batchId);
        s.bind(2, static_cast<std::uint8_t>(r.side));
        s.bind(3, r.rank);
        s.bind(4, r.hour);
        s.bind(5, r.accelerator);
        s.bind(6, r.interconnect);
        s.bind(7, r.region);
        s.bind(8, r.price);
        s.bind(9, r.units);
        s.bind(10, r.lifted);
        s.step();
        s.reset();
    }
    for (const AcceleratorPriceRow& r : record.prices) {
        Statement& s = *impl.insertPrice;
        s.bind(1, h.batchId);
        s.bind(2, r.hour);
        s.bind(3, r.accelerator);
        s.bind(4, r.units);
        s.bind(5, r.amount);
        s.bind(6, r.priceMin);
        s.bind(7, r.priceMax);
        s.step();
        s.reset();
    }
    return true;
}

void replaceLiveState(sqlite3* db, const LiveState& state) {
    Statement s(db, "INSERT OR REPLACE INTO live (id, batch_id, first_hour, event_seq) VALUES (1, ?1, ?2, ?3)");
    s.bind(1, state.batchId);
    s.bind(2, state.firstHour);
    s.bind(3, state.eventSeq);
    s.step();
}

} // namespace

ResultsStore::ResultsStore(const std::string& path, Mode mode) : impl_(std::make_unique<Impl>()) {
    const int flags = mode == Mode::READ_ONLY ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
    impl_->db = db;
    if (rc != SQLITE_OK) fail(db, "cannot open " + path, rc);
    sqlite3_busy_timeout(db, 5000);

    if (mode == Mode::READ_WRITE) {
        const std::string journal = firstText(db, "PRAGMA journal_mode=WAL");
        if (journal != "wal" && journal != "memory") throw StoreError("cannot use WAL, journal mode is " + journal, SQLITE_ERROR);
        exec(db, "PRAGMA synchronous=FULL");
        if (std::stoi(firstText(db, "PRAGMA user_version")) == 0) {
            if (std::stoi(firstText(db, "SELECT count(*) FROM sqlite_master")) != 0) {
                throw StoreError(path + " is a database of something else", SQLITE_NOTADB);
            }
            Transaction tx(db);
            exec(db, SCHEMA);
            writeCatalog(db);
            exec(db, ("PRAGMA user_version=" + std::to_string(SCHEMA_VERSION)).c_str());
            tx.commit();
        }
    }
    const int version = std::stoi(firstText(db, "PRAGMA user_version"));
    if (version != SCHEMA_VERSION) {
        throw StoreError(path + " has schema version " + std::to_string(version) + ", expected " +
                             std::to_string(SCHEMA_VERSION),
                         SQLITE_SCHEMA);
    }
    if (!catalogMatches(db)) {
        throw StoreError(path + " was written under another catalog of accelerators, interconnects and regions", SQLITE_SCHEMA);
    }
}

ResultsStore::~ResultsStore() = default;

ResultsStore::Outcome ResultsStore::write(const BatchRecord& record, const LiveState* live) {
    if (!isCanonical(record)) throw StoreError("the rows of a batch are not in canonical order", SQLITE_MISUSE);
    sqlite3* db = impl_->db;
    const Digest digest = batchDigest(record);

    if (!impl_->insertBatch) {
        impl_->insertBatch = std::make_unique<Statement>(
            db, std::string("INSERT OR IGNORE INTO batches (") + BATCH_COLUMNS + ") VALUES (" + placeholders(17) + ")");
        impl_->insertOrder = std::make_unique<Statement>(
            db, "INSERT INTO orders (batch_id, " + orderColumns("") + ") VALUES (" + placeholders(1 + ORDER_COLUMN_COUNT) + ")");
        impl_->insertFill = std::make_unique<Statement>(
            db, std::string("INSERT INTO fills (batch_id, ") + FILL_COLUMNS + ") VALUES (" + placeholders(10) + ")");
        impl_->insertPrice = std::make_unique<Statement>(
            db, std::string("INSERT INTO accelerator_prices (batch_id, ") + PRICE_COLUMNS + ") VALUES (" + placeholders(7) + ")");
    }

    Transaction tx(db);
    bool inserted = false;
    try {
        inserted = insertRecord(db, *impl_, record, digest);
        if (live) {
            exec(db, "DELETE FROM open_orders");
            replaceLiveState(db, *live);
        }
        tx.commit();
    } catch (...) {
        impl_->resetStatements();
        throw;
    }
    if (inserted) return Outcome::WRITTEN;
    const std::optional<BatchHeader> stored = batch(record.header.batchId);
    return stored && stored->digest == digest ? Outcome::ALREADY_RECORDED : Outcome::ALREADY_RECORDED_DIFFERENTLY;
}

void ResultsStore::applyLive(const LiveChanges& changes) {
    sqlite3* db = impl_->db;
    Transaction tx(db);
    {
        Statement remove(db, "DELETE FROM open_orders WHERE side = ?1 AND rank = ?2");
        for (const OrderKey& key : changes.removals) {
            remove.bind(1, static_cast<std::uint8_t>(key.first));
            remove.bind(2, key.second);
            remove.step();
            remove.reset();
        }
        Statement put(db, "INSERT OR REPLACE INTO open_orders (" + openOrderColumns("") + ") VALUES (" +
                              placeholders(OPEN_ORDER_COLUMN_COUNT) + ")");
        for (const OpenOrderRow& r : changes.upserts) {
            put.bind(1, static_cast<std::uint8_t>(r.side));
            put.bind(2, r.rank);
            put.bind(3, r.participant);
            put.bind(4, r.token);
            bindTerms(put, 5, r.terms);
            put.step();
            put.reset();
        }
    }
    replaceLiveState(db, changes.state);
    tx.commit();
}

void ResultsStore::clearLive() {
    Transaction tx(impl_->db);
    exec(impl_->db, "DELETE FROM open_orders");
    exec(impl_->db, "DELETE FROM live");
    tx.commit();
}

std::optional<BatchHeader> ResultsStore::batch(BatchId id) const {
    Statement s(impl_->db, std::string("SELECT ") + BATCH_COLUMNS + " FROM batches WHERE batch_id = ?1");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return readHeader(s);
}

std::vector<OrderRow> ResultsStore::orders(BatchId id) const {
    return orders(id, 0, std::numeric_limits<std::size_t>::max());
}

std::vector<OrderRow> ResultsStore::orders(BatchId id, std::size_t offset, std::size_t limit) const {
    Statement s(impl_->db, "SELECT " + orderColumns("") + " FROM orders WHERE batch_id = ?1 ORDER BY side, rank LIMIT ?2 OFFSET ?3");
    s.bind(1, id);
    s.bind(2, limitOf(limit));
    s.bind(3, offsetOf(offset));
    std::vector<OrderRow> out;
    while (s.step()) out.push_back(readOrder(s, 0));
    return out;
}

std::optional<OrderRow> ResultsStore::order(BatchId id, RowSide side, std::uint32_t rank) const {
    Statement s(impl_->db, "SELECT " + orderColumns("") + " FROM orders WHERE batch_id = ?1 AND side = ?2 AND rank = ?3");
    s.bind(1, id);
    s.bind(2, static_cast<std::uint8_t>(side));
    s.bind(3, rank);
    if (!s.step()) return std::nullopt;
    return readOrder(s, 0);
}

std::vector<FillRow> ResultsStore::fills(BatchId id) const {
    return fills(id, 0, std::numeric_limits<std::size_t>::max());
}

std::vector<FillRow> ResultsStore::fills(BatchId id, std::size_t offset, std::size_t limit) const {
    Statement s(impl_->db, std::string("SELECT ") + FILL_COLUMNS +
                               " FROM fills WHERE batch_id = ?1 ORDER BY side, rank, hour, accelerator, interconnect, region "
                               "LIMIT ?2 OFFSET ?3");
    s.bind(1, id);
    s.bind(2, limitOf(limit));
    s.bind(3, offsetOf(offset));
    std::vector<FillRow> out;
    while (s.step()) out.push_back(readFill(s));
    return out;
}

std::vector<FillRow> ResultsStore::fillsOf(BatchId id, RowSide side, std::uint32_t rank) const {
    Statement s(impl_->db, std::string("SELECT ") + FILL_COLUMNS +
                               " FROM fills WHERE batch_id = ?1 AND side = ?2 AND rank = ?3 "
                               "ORDER BY hour, accelerator, interconnect, region");
    s.bind(1, id);
    s.bind(2, static_cast<std::uint8_t>(side));
    s.bind(3, rank);
    std::vector<FillRow> out;
    while (s.step()) out.push_back(readFill(s));
    return out;
}

std::vector<BatchHeader> ResultsStore::batches(std::optional<BatchId> before, std::size_t limit) const {
    std::vector<BatchHeader> out;
    const std::size_t cursorRegion = before && toDb(*before) >= 0 ? 1 : 0;
    for (std::size_t r = 0; r < std::size(REGIONS_LARGEST_FIRST) && out.size() < limit; ++r) {
        if (before && r < cursorRegion) continue; // everything there is larger than the cursor
        const bool bounded = before && r == cursorRegion;
        std::string sql = std::string("SELECT ") + BATCH_COLUMNS + " FROM batches WHERE batch_id BETWEEN ?1 AND ?2";
        if (bounded) sql += " AND batch_id < ?3";
        sql += " ORDER BY batch_id DESC LIMIT ?4";
        Statement s(impl_->db, sql);
        s.bind(1, REGIONS_LARGEST_FIRST[r].lo);
        s.bind(2, REGIONS_LARGEST_FIRST[r].hi);
        if (bounded) s.bind(3, *before);
        s.bind(4, limitOf(limit - out.size()));
        while (s.step()) out.push_back(readHeader(s));
    }
    return out;
}

std::vector<ResultsStore::ParticipantOrder> ResultsStore::ordersOf(ParticipantId participant,
                                                                   std::optional<Position> before,
                                                                   std::optional<OrderToken> token,
                                                                   std::size_t limit) const {
    std::vector<ParticipantOrder> out;
    const std::size_t cursorRegion = before && toDb(before->batchId) >= 0 ? 1 : 0;
    for (std::size_t r = 0; r < std::size(REGIONS_LARGEST_FIRST) && out.size() < limit; ++r) {
        if (before && r < cursorRegion) continue;
        const bool bounded = before && r == cursorRegion;
        // CROSS JOIN: walk the participant's orders and look each batch up, and not the other way round.
        std::string sql = "SELECT o.batch_id, b.first_hour, " + orderColumns("o.") +
                          " FROM orders o CROSS JOIN batches b ON b.batch_id = o.batch_id"
                          " WHERE o.participant_id = ?1 AND o.batch_id BETWEEN ?2 AND ?3";
        if (bounded) sql += " AND (o.batch_id, o.side, o.rank) < (?4, ?5, ?6)";
        if (token) sql += " AND o.order_token = ?7";
        sql += " ORDER BY o.batch_id DESC, o.side DESC, o.rank DESC LIMIT ?8";
        Statement s(impl_->db, sql);
        s.bind(1, participant);
        s.bind(2, REGIONS_LARGEST_FIRST[r].lo);
        // The range ends at the cursor's batch, so that the scan starts there and not at the newest batch.
        s.bind(3, bounded ? toDb(before->batchId) : REGIONS_LARGEST_FIRST[r].hi);
        if (bounded) {
            s.bind(4, before->batchId);
            s.bind(5, static_cast<std::uint8_t>(before->side));
            s.bind(6, before->rank);
        }
        if (token) s.bind(7, *token);
        s.bind(8, limitOf(limit - out.size()));
        while (s.step()) {
            out.push_back(ParticipantOrder{ Position{ s.u64(0), static_cast<RowSide>(s.u8(2)), s.u32(3) }, s.u32(1), readOrder(s, 2) });
        }
    }
    return out;
}

std::vector<AcceleratorPriceRow> ResultsStore::prices(BatchId id) const {
    Statement s(impl_->db, std::string("SELECT ") + PRICE_COLUMNS + " FROM accelerator_prices WHERE batch_id = ?1 ORDER BY hour, accelerator");
    s.bind(1, id);
    std::vector<AcceleratorPriceRow> out;
    while (s.step()) out.push_back(readPrice(s, 0));
    return out;
}

std::vector<ResultsStore::PricePoint> ResultsStore::priceSeries(std::uint8_t accelerator, HourIndex hour,
                                                                std::optional<BatchId> before, std::size_t limit) const {
    std::vector<PricePoint> out;
    const std::size_t cursorRegion = before && toDb(*before) >= 0 ? 1 : 0;
    for (std::size_t r = 0; r < std::size(REGIONS_LARGEST_FIRST) && out.size() < limit; ++r) {
        if (before && r < cursorRegion) continue;
        const bool bounded = before && r == cursorRegion;
        std::string sql = std::string("SELECT batch_id, ") + PRICE_COLUMNS +
                          " FROM accelerator_prices WHERE accelerator = ?1 AND hour = ?2 AND batch_id BETWEEN ?3 AND ?4";
        if (bounded) sql += " AND batch_id < ?5";
        sql += " ORDER BY batch_id DESC LIMIT ?6";
        Statement s(impl_->db, sql);
        s.bind(1, accelerator);
        s.bind(2, hour);
        s.bind(3, REGIONS_LARGEST_FIRST[r].lo);
        s.bind(4, bounded ? toDb(*before) : REGIONS_LARGEST_FIRST[r].hi);
        if (bounded) s.bind(5, *before);
        s.bind(6, limitOf(limit - out.size()));
        while (s.step()) out.push_back(PricePoint{ s.u64(0), readPrice(s, 1) });
    }
    return out;
}

std::vector<ResultsStore::PricePoint> ResultsStore::priceSeriesAtLead(std::uint8_t accelerator, std::uint32_t lead,
                                                                      std::optional<BatchId> before, std::size_t limit) const {
    std::vector<PricePoint> out;
    const std::size_t cursorRegion = before && toDb(*before) >= 0 ? 1 : 0;
    for (std::size_t r = 0; r < std::size(REGIONS_LARGEST_FIRST) && out.size() < limit; ++r) {
        if (before && r < cursorRegion) continue;
        const bool bounded = before && r == cursorRegion;
        // CROSS JOIN: walk the batches newest first and look the price up by its key, and not the other way
        // round, which reads all of the accelerator's prices.
        std::string sql = "SELECT b.batch_id, p.hour, p.accelerator, p.units, p.amount, p.price_min, p.price_max"
                          " FROM batches b CROSS JOIN accelerator_prices p"
                          " ON p.batch_id = b.batch_id AND p.hour = b.first_hour + ?2 AND p.accelerator = ?1"
                          " WHERE b.batch_id BETWEEN ?3 AND ?4";
        if (bounded) sql += " AND b.batch_id < ?5";
        sql += " ORDER BY b.batch_id DESC LIMIT ?6";
        Statement s(impl_->db, sql);
        s.bind(1, accelerator);
        s.bind(2, lead);
        s.bind(3, REGIONS_LARGEST_FIRST[r].lo);
        s.bind(4, bounded ? toDb(*before) : REGIONS_LARGEST_FIRST[r].hi);
        if (bounded) s.bind(5, *before);
        s.bind(6, limitOf(limit - out.size()));
        while (s.step()) out.push_back(PricePoint{ s.u64(0), readPrice(s, 1) });
    }
    return out;
}

std::optional<ResultsStore::OpenOrders> ResultsStore::openOrdersOf(ParticipantId participant) const {
    Statement s(impl_->db, "SELECT l.batch_id, l.first_hour, l.event_seq, " + openOrderColumns("o.") +
                               " FROM live l LEFT JOIN open_orders o ON o.participant_id = ?1"
                               " WHERE l.id = 1 ORDER BY o.side, o.rank");
    s.bind(1, participant);
    std::optional<OpenOrders> out;
    while (s.step()) {
        if (!out) out = OpenOrders{ LiveState{ s.u64(0), s.u32(1), s.u64(2) }, {} };
        if (!s.isNull(3)) out->orders.push_back(readOpenOrder(s, 3));
    }
    return out;
}

std::optional<LiveState> ResultsStore::live() const {
    Statement s(impl_->db, "SELECT batch_id, first_hour, event_seq FROM live WHERE id = 1");
    if (!s.step()) return std::nullopt;
    return LiveState{ s.u64(0), s.u32(1), s.u64(2) };
}

std::vector<OpenOrderRow> ResultsStore::openOrders() const {
    Statement s(impl_->db, "SELECT " + openOrderColumns("") + " FROM open_orders ORDER BY side, rank");
    std::vector<OpenOrderRow> out;
    while (s.step()) out.push_back(readOpenOrder(s, 0));
    return out;
}

ResultsStore::Watermark ResultsStore::watermark() const {
    Watermark w;
    {
        Statement s(impl_->db, "SELECT count(*), coalesce(max(event_seq), 0) FROM batches");
        if (s.step()) {
            w.batches = s.u64(0);
            w.lastEventSeq = s.u64(1);
        }
    }
    Statement s(impl_->db, "SELECT batch_id FROM batches ORDER BY event_seq DESC LIMIT 1");
    if (s.step()) w.lastBatchId = s.u64(0);
    return w;
}

bool ResultsStore::intact(BatchId id) const {
    const std::optional<BatchHeader> header = batch(id);
    if (!header) return false;
    BatchRecord record;
    record.header = *header;
    record.orders = orders(id);
    record.fills = fills(id);
    record.prices = prices(id);
    return batchDigest(record) == header->digest;
}

std::string ResultsStore::pragma(const std::string& name) const {
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') throw StoreError("bad pragma name", SQLITE_MISUSE);
    }
    return firstText(impl_->db, "PRAGMA " + name);
}

} // namespace recorder
