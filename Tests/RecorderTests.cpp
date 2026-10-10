#include <gtest/gtest.h>

#include <sqlite3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "BatchEngine.hpp"
#include "BatchRecorder.hpp"
#include "Catalog.hpp"
#include "Digest.hpp"
#include "EngineBook.hpp"
#include "EngineFixtures.hpp"
#include "ExchangeFixtures.hpp"
#include "RecorderPolicy.hpp"
#include "ResultsStore.hpp"
#include "SequencedInboundMessage.hpp"
#include "Sha256.hpp"

using namespace recorder;
using namespace enginefix;
using namespace exfix;

namespace {

void rawExec(const std::string& path, const std::string& sql) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), SQLITE_OK);
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    EXPECT_EQ(rc, SQLITE_OK) << sql << ": " << (message ? message : "");
    sqlite3_free(message);
    sqlite3_close(db);
}

std::int64_t scalar(const std::string& path, const std::string& sql) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK) << sql;
    std::int64_t value = -1;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

// Two bids and two asks at three items: H100 at 1800 on NVLink in two hours and at 1700 on PCIe in the first.
// A buys 3 (2 + 1), B buys 3 (1 + 2), X sells 5 (4 + 1) and Y sells 1. Every number agrees with the others, as a
// batch the engine emitted would.
BatchRecord sampleRecord(BatchId id = 7) {
    BatchRecord r;
    BatchHeader& h = r.header;
    h.batchId = id;
    h.firstHour = 497'449;
    h.eventSeq = 100 + id;
    h.bids = 2;
    h.asks = 2;
    h.arcs = 20;
    h.pivots = 9;
    h.units = 6;
    h.liftedUnits = 5;
    h.welfare = 1200;
    h.rankSum = 4;
    h.fills = 7;
    h.certified = 1;
    h.solverVersion = SOLVER_VERSION;
    r.orders = {
        OrderRow{ RowSide::BID, 0, 101, 1001, makeTerms(A100 | H100, NVLINK, US_WEST, 0b11, 2, { { 2, 2400 }, { 1, 2200 } }), 3, 3, 5500 },
        OrderRow{ RowSide::BID, 1, 102, 1002, makeTerms(H100, PCIE | NVLINK, US_WEST, 0b1, 3, { { 3, 3000 } }), 3, 2, 5300 },
        OrderRow{ RowSide::ASK, 0, 201, 2001, makeTerms(H100, NVLINK, US_WEST, 0b11, 4, { { 8, 1800 }, { 8, 2000 } }), 5, 0, 9100 },
        OrderRow{ RowSide::ASK, 1, 202, 2002, makeTerms(H100, PCIE, US_WEST, 0b1, 1, { { 1, 1700 } }), 1, 0, 1700 },
    };
    r.fills = {
        FillRow{ RowSide::BID, 0, 497'449, 1, 1, 0, 1800, 2, 1 },
        FillRow{ RowSide::BID, 0, 497'450, 1, 1, 0, 1900, 1, 1 },
        FillRow{ RowSide::BID, 1, 497'449, 1, 0, 0, 1700, 1, 0 },
        FillRow{ RowSide::BID, 1, 497'449, 1, 1, 0, 1800, 2, 1 },
        FillRow{ RowSide::ASK, 0, 497'449, 1, 1, 0, 1800, 4, 0 },
        FillRow{ RowSide::ASK, 0, 497'450, 1, 1, 0, 1900, 1, 0 },
        FillRow{ RowSide::ASK, 1, 497'449, 1, 0, 0, 1700, 1, 0 },
    };
    r.prices = {
        AcceleratorPriceRow{ 497'449, 1, 5, 8900, 1700, 1800 },
        AcceleratorPriceRow{ 497'450, 1, 1, 1900, 1900, 1900 },
    };
    h.digest = batchDigest(r);
    return r;
}

BatchRecord voidedRecord(BatchId id) {
    BatchRecord r;
    r.header.batchId = id;
    r.header.status = BatchStatus::VOIDED;
    r.header.voidReason = 1;
    r.header.firstHour = 497'450;
    r.header.eventSeq = 200 + id;
    r.header.bids = 3;
    r.header.asks = 2;
    r.header.arcs = 61;
    r.header.pivots = 5;
    r.header.solverVersion = SOLVER_VERSION;
    r.header.digest = batchDigest(r);
    return r;
}

OpenOrderRow openOrder(RowSide side, std::uint32_t rank, ParticipantId participant, OrderToken token, const OrderTerms& terms) {
    return OpenOrderRow{ side, rank, participant, token, terms };
}

} // namespace

TEST(Digest, Sha256MatchesTheStandardVectorsAndTheBatchDigestCoversTheResultsAndNothingElse) {
    auto sha = [](const std::string& text, std::size_t chunk) {
        Sha256 hash;
        for (std::size_t pos = 0; pos < text.size(); pos += chunk) {
            hash.update(text.data() + pos, std::min(chunk, text.size() - pos));
        }
        return hexOf(hash.finish());
    };
    EXPECT_EQ(sha("", 1), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha("abc", 1), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 7),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha(std::string(1'000'000, 'a'), 997), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    const std::pair<std::size_t, const char*> padding[] = {
        { 55, "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072" },
        { 56, "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e" },
        { 63, "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2" },
        { 64, "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c" },
        { 65, "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9" },
    };
    for (const auto& [length, want] : padding) {
        EXPECT_EQ(sha(std::string(length, 'x'), 1), want) << length << " bytes";
        EXPECT_EQ(sha(std::string(length, 'x'), 64), want) << length << " bytes in one piece";
    }

    const BatchRecord base = sampleRecord();
    const Digest original = batchDigest(base);
    const std::pair<const char*, std::function<void(BatchRecord&)>> changes[] = {
        { "batch id", [](BatchRecord& r) { r.header.batchId += 1; } },
        { "status", [](BatchRecord& r) { r.header.status = BatchStatus::VOIDED; } },
        { "void reason", [](BatchRecord& r) { r.header.voidReason = 2; } },
        { "first hour", [](BatchRecord& r) { r.header.firstHour += 1; } },
        { "bids", [](BatchRecord& r) { r.header.bids += 1; } },
        { "asks", [](BatchRecord& r) { r.header.asks += 1; } },
        { "arcs", [](BatchRecord& r) { r.header.arcs += 1; } },
        { "pivots", [](BatchRecord& r) { r.header.pivots += 1; } },
        { "units", [](BatchRecord& r) { r.header.units += 1; } },
        { "lifted units", [](BatchRecord& r) { r.header.liftedUnits += 1; } },
        { "welfare", [](BatchRecord& r) { r.header.welfare += 1; } },
        { "rank sum", [](BatchRecord& r) { r.header.rankSum += 1; } },
        { "fills", [](BatchRecord& r) { r.header.fills += 1; } },
        { "certified", [](BatchRecord& r) { r.header.certified = 0; } },
        { "an order's side", [](BatchRecord& r) { r.orders[0].side = RowSide::ASK; } },
        { "an order's rank", [](BatchRecord& r) { r.orders[0].rank += 1; } },
        { "an order's participant", [](BatchRecord& r) { r.orders[0].participant += 1; } },
        { "an order's token", [](BatchRecord& r) { r.orders[0].token += 1; } },
        { "an order's accelerators", [](BatchRecord& r) { r.orders[0].terms.acceleratorMask ^= 0b100; } },
        { "an order's interconnects", [](BatchRecord& r) { r.orders[0].terms.interconnectMask ^= 1; } },
        { "an order's regions", [](BatchRecord& r) { r.orders[0].terms.regionMask ^= 0b10; } },
        { "an order's hours", [](BatchRecord& r) { r.orders[0].terms.slotMask <<= 1; } },
        { "an order's cap", [](BatchRecord& r) { r.orders[0].terms.perSlotCap += 1; } },
        { "an order's quantity", [](BatchRecord& r) { r.orders[0].terms.totalUnits += 1; } },
        { "an order's step count", [](BatchRecord& r) { r.orders[0].terms.stepCount = 1; } },
        { "an order's first step quantity", [](BatchRecord& r) { r.orders[0].terms.stepQuantity[0] += 1; } },
        { "an order's first step price", [](BatchRecord& r) { r.orders[0].terms.stepPrice[0] += 1; } },
        { "an order's last step price", [](BatchRecord& r) { r.orders[0].terms.stepPrice[1] += 1; } },
        { "a step the order does not have", [](BatchRecord& r) { r.orders[0].terms.stepQuantity[3] = 1; } },
        { "the price of a step the order does not have", [](BatchRecord& r) { r.orders[0].terms.stepPrice[3] = 1; } },
        { "an order's filled units", [](BatchRecord& r) { r.orders[0].filled += 1; } },
        { "an order's lifted units", [](BatchRecord& r) { r.orders[0].lifted += 1; } },
        { "an order's amount", [](BatchRecord& r) { r.orders[0].amount += 1; } },
        { "a fill's side", [](BatchRecord& r) { r.fills[0].side = RowSide::ASK; } },
        { "a fill's rank", [](BatchRecord& r) { r.fills[0].rank += 1; } },
        { "a fill's hour", [](BatchRecord& r) { r.fills[0].hour += 1; } },
        { "a fill's accelerator", [](BatchRecord& r) { r.fills[0].accelerator += 1; } },
        { "a fill's interconnect", [](BatchRecord& r) { r.fills[0].interconnect += 1; } },
        { "a fill's region", [](BatchRecord& r) { r.fills[0].region += 1; } },
        { "a fill's price", [](BatchRecord& r) { r.fills[0].price += 1; } },
        { "a fill's units", [](BatchRecord& r) { r.fills[0].units += 1; } },
        { "a fill's lifted flag", [](BatchRecord& r) { r.fills[0].lifted ^= 1; } },
        { "a price's hour", [](BatchRecord& r) { r.prices[0].hour += 1; } },
        { "a price's accelerator", [](BatchRecord& r) { r.prices[0].accelerator += 1; } },
        { "a price's units", [](BatchRecord& r) { r.prices[0].units += 1; } },
        { "a price's amount", [](BatchRecord& r) { r.prices[0].amount += 1; } },
        { "a price's lowest item", [](BatchRecord& r) { r.prices[0].priceMin += 1; } },
        { "a price's highest item", [](BatchRecord& r) { r.prices[0].priceMax += 1; } },
        { "an order added", [](BatchRecord& r) { r.orders.push_back(r.orders.back()); } },
        { "an order dropped", [](BatchRecord& r) { r.orders.pop_back(); } },
        { "a fill added", [](BatchRecord& r) { r.fills.push_back(r.fills.back()); } },
        { "a fill dropped", [](BatchRecord& r) { r.fills.pop_back(); } },
        { "a price added", [](BatchRecord& r) { r.prices.push_back(r.prices.back()); } },
        { "a price dropped", [](BatchRecord& r) { r.prices.pop_back(); } },
    };
    for (const auto& [what, change] : changes) {
        BatchRecord changed = base;
        change(changed);
        EXPECT_NE(batchDigest(changed), original) << "the digest does not cover " << what;
    }

    BatchRecord elsewhere = base;
    elsewhere.header.eventSeq += 1000;
    elsewhere.header.solverVersion += 1;
    elsewhere.header.digest = Digest{};
    EXPECT_EQ(batchDigest(elsewhere), original) << "when and by which solver a batch was recorded is not a result";

    ASSERT_TRUE(isCanonical(base));
    BatchRecord shuffled = base;
    std::reverse(shuffled.orders.begin(), shuffled.orders.end());
    std::reverse(shuffled.fills.begin(), shuffled.fills.end());
    std::reverse(shuffled.prices.begin(), shuffled.prices.end());
    EXPECT_FALSE(isCanonical(shuffled));
    sortCanonical(shuffled);
    EXPECT_TRUE(isCanonical(shuffled));
    EXPECT_EQ(batchDigest(shuffled), original);
}

TEST(ResultsStore, WritesABatchAndReadsEveryRowBackAndNoticesWhenARowIsChanged) {
    TempFile db;
    ResultsStore store(db.path());
    EXPECT_EQ(store.pragma("journal_mode"), "wal");
    EXPECT_EQ(store.pragma("synchronous"), "2") << "full";
    EXPECT_EQ(store.pragma("user_version"), "2");
    EXPECT_FALSE(store.batch(7));
    EXPECT_EQ(store.watermark().batches, 0u);

    const BatchRecord record = sampleRecord();
    ASSERT_EQ(store.write(record), ResultsStore::Outcome::WRITTEN);
    EXPECT_EQ(store.batch(7), record.header) << "the stored digest is the one computed from the rows";
    EXPECT_EQ(store.orders(7), record.orders);
    EXPECT_EQ(store.fills(7), record.fills);
    EXPECT_EQ(store.prices(7), record.prices);
    EXPECT_TRUE(store.intact(7));
    EXPECT_EQ(store.watermark().batches, 1u);
    EXPECT_EQ(store.watermark().lastEventSeq, record.header.eventSeq);
    EXPECT_EQ(store.watermark().lastBatchId, 7u);

    {
        SCOPED_TRACE("a reader in another connection, while the writer is open, sees the same and cannot write");
        ResultsStore reader(db.path(), ResultsStore::Mode::READ_ONLY);
        EXPECT_EQ(reader.batch(7), record.header);
        EXPECT_EQ(reader.fills(7), record.fills);
        EXPECT_THROW(reader.write(sampleRecord(8)), StoreError);
        EXPECT_FALSE(reader.batch(8));
    }

    {
        SCOPED_TRACE("ids and tokens above 2^63 come back as they went in");
        BatchRecord huge = sampleRecord(std::numeric_limits<BatchId>::max());
        huge.orders[0].token = std::numeric_limits<OrderToken>::max();
        huge.header.digest = batchDigest(huge);
        ASSERT_EQ(store.write(huge), ResultsStore::Outcome::WRITTEN);
        BatchRecord big = sampleRecord(BatchId{ 1 } << 63);
        ASSERT_EQ(store.write(big), ResultsStore::Outcome::WRITTEN);
        EXPECT_EQ(store.batch(huge.header.batchId), huge.header);
        EXPECT_EQ(store.orders(huge.header.batchId), huge.orders);
        EXPECT_EQ(store.batch(big.header.batchId), big.header);
        EXPECT_EQ(store.batch(big.header.batchId + 1), std::nullopt);
    }

    {
        SCOPED_TRACE("a voided batch is a row and nothing else");
        const BatchRecord voided = voidedRecord(9);
        ASSERT_EQ(store.write(voided), ResultsStore::Outcome::WRITTEN);
        EXPECT_EQ(store.batch(9), voided.header);
        EXPECT_TRUE(store.orders(9).empty());
        EXPECT_TRUE(store.fills(9).empty());
        EXPECT_TRUE(store.prices(9).empty());
        EXPECT_TRUE(store.intact(9));
    }

    {
        SCOPED_TRACE("reads in pages, batches largest id first, and one participant's orders newest first");
        const auto slice = [](const auto& all, std::size_t offset, std::size_t limit) {
            const std::size_t from = std::min(offset, all.size());
            const std::size_t to = std::min(offset + limit, all.size());
            return std::decay_t<decltype(all)>(all.begin() + static_cast<std::ptrdiff_t>(from), all.begin() + static_cast<std::ptrdiff_t>(to));
        };
        for (std::size_t offset = 0; offset <= 8; ++offset) {
            for (std::size_t limit = 1; limit <= 8; ++limit) {
                EXPECT_EQ(store.orders(7, offset, limit), slice(record.orders, offset, limit)) << offset << "+" << limit;
                EXPECT_EQ(store.fills(7, offset, limit), slice(record.fills, offset, limit)) << offset << "+" << limit;
            }
        }
        EXPECT_EQ(store.orders(7, 1, std::numeric_limits<std::size_t>::max()).size(), 3u) << "no limit";
        EXPECT_TRUE(store.orders(7, std::numeric_limits<std::size_t>::max(), 5).empty()) << "an offset past everything";
        EXPECT_EQ(store.order(7, RowSide::ASK, 0), record.orders[2]);
        EXPECT_EQ(store.order(7, RowSide::BID, 1), record.orders[1]);
        EXPECT_FALSE(store.order(7, RowSide::ASK, 2));
        EXPECT_FALSE(store.order(8, RowSide::BID, 0));
        EXPECT_EQ(store.fillsOf(7, RowSide::BID, 0), slice(record.fills, 0, 2));
        EXPECT_EQ(store.fillsOf(7, RowSide::BID, 1), slice(record.fills, 2, 2));
        EXPECT_EQ(store.fillsOf(7, RowSide::ASK, 0), slice(record.fills, 4, 2));
        EXPECT_TRUE(store.fillsOf(7, RowSide::BID, 5).empty());

        BatchRecord both = sampleRecord(20);
        both.orders[2].participant = 101; // the same participant on both sides of one batch
        both.header.digest = batchDigest(both);
        ASSERT_EQ(store.write(both), ResultsStore::Outcome::WRITTEN);

        const auto idsOf = [](const std::vector<BatchHeader>& headers) {
            std::vector<BatchId> ids;
            for (const BatchHeader& h : headers) ids.push_back(h.batchId);
            return ids;
        };
        constexpr BatchId LARGEST = std::numeric_limits<BatchId>::max();
        constexpr BatchId HALF = BatchId{ 1 } << 63;
        using Ids = std::vector<BatchId>;
        EXPECT_EQ(idsOf(store.batches(std::nullopt, 10)), (Ids{ LARGEST, HALF, 20, 9, 7 })) << "as unsigned numbers";
        EXPECT_EQ(idsOf(store.batches(std::nullopt, 2)), (Ids{ LARGEST, HALF }));
        EXPECT_EQ(idsOf(store.batches(LARGEST, 10)), (Ids{ HALF, 20, 9, 7 }));
        EXPECT_EQ(idsOf(store.batches(HALF, 2)), (Ids{ 20, 9 }));
        EXPECT_EQ(idsOf(store.batches(HALF + 1, 1)), (Ids{ HALF }));
        EXPECT_EQ(idsOf(store.batches(21, 10)), (Ids{ 20, 9, 7 }));
        EXPECT_EQ(idsOf(store.batches(7, 10)), (Ids{}));
        EXPECT_EQ(idsOf(store.batches(0, 10)), (Ids{}));
        EXPECT_EQ(store.batches(9, 1).front(), *store.batch(7));

        using Position = ResultsStore::Position;
        const auto positionsOf = [](const std::vector<ResultsStore::ParticipantOrder>& rows) {
            std::vector<Position> out;
            for (const auto& r : rows) out.push_back(r.position);
            return out;
        };
        using Positions = std::vector<Position>;
        const Positions all = { { LARGEST, RowSide::BID, 0 }, { HALF, RowSide::BID, 0 }, { 20, RowSide::ASK, 0 }, { 20, RowSide::BID, 0 }, { 7, RowSide::BID, 0 } };
        EXPECT_EQ(positionsOf(store.ordersOf(101, std::nullopt, std::nullopt, 10)), all);
        for (std::size_t from = 0; from < all.size(); ++from) {
            EXPECT_EQ(positionsOf(store.ordersOf(101, all[from], std::nullopt, 10)), Positions(all.begin() + static_cast<std::ptrdiff_t>(from) + 1, all.end()))
                << "after " << from;
        }
        EXPECT_EQ(positionsOf(store.ordersOf(101, std::nullopt, std::nullopt, 3)), Positions(all.begin(), all.begin() + 3));
        EXPECT_EQ(positionsOf(store.ordersOf(101, Position{ 8, RowSide::BID, 0 }, std::nullopt, 10)), (Positions{ { 7, RowSide::BID, 0 } }));
        EXPECT_EQ(positionsOf(store.ordersOf(101, Position{ 7, RowSide::BID, 0 }, std::nullopt, 10)), Positions{});
        EXPECT_EQ(positionsOf(store.ordersOf(101, std::nullopt, std::numeric_limits<OrderToken>::max(), 10)), (Positions{ { LARGEST, RowSide::BID, 0 } }))
            << "only the order with that token";
        EXPECT_EQ(positionsOf(store.ordersOf(201, std::nullopt, std::nullopt, 10)),
                  (Positions{ { LARGEST, RowSide::ASK, 0 }, { HALF, RowSide::ASK, 0 }, { 7, RowSide::ASK, 0 } }));
        EXPECT_TRUE(store.ordersOf(555, std::nullopt, std::nullopt, 10).empty());
        const auto rows = store.ordersOf(101, Position{ HALF, RowSide::BID, 0 }, std::nullopt, 1);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].row, both.orders[2]) << "the row is the whole order, terms included";
        EXPECT_EQ(rows[0].firstHour, both.header.firstHour) << "and the first hour of its batch comes with it";
        EXPECT_EQ(store.ordersOf(101, std::nullopt, std::nullopt, 1)[0].firstHour, 497'449u);

        SCOPED_TRACE("the price of a model in an hour over the batches, and at a fixed lead");
        const auto pointsOf = [](const std::vector<ResultsStore::PricePoint>& points) {
            Ids out;
            for (const auto& p : points) out.push_back(p.batchId);
            return out;
        };
        const std::vector<ResultsStore::PricePoint> firstHour = store.priceSeries(1, 497'449, std::nullopt, 10);
        EXPECT_EQ(pointsOf(firstHour), (Ids{ LARGEST, HALF, 20, 7 })) << "the batches that traded it, largest id first as unsigned numbers";
        EXPECT_EQ(firstHour.front().row, record.prices[0]);
        EXPECT_EQ(pointsOf(store.priceSeries(1, 497'449, std::nullopt, 2)), (Ids{ LARGEST, HALF }));
        EXPECT_EQ(pointsOf(store.priceSeries(1, 497'449, LARGEST, 10)), (Ids{ HALF, 20, 7 }));
        EXPECT_EQ(pointsOf(store.priceSeries(1, 497'449, HALF, 1)), (Ids{ 20 }));
        EXPECT_EQ(pointsOf(store.priceSeries(1, 497'449, 7, 10)), (Ids{}));
        EXPECT_EQ(store.priceSeries(1, 497'450, 21, 10).front().row, record.prices[1]);
        EXPECT_TRUE(store.priceSeries(0, 497'449, std::nullopt, 10).empty()) << "another model";
        EXPECT_TRUE(store.priceSeries(1, 497'451, std::nullopt, 10).empty()) << "another hour";
        EXPECT_EQ(pointsOf(store.priceSeriesAtLead(1, 0, std::nullopt, 10)), (Ids{ LARGEST, HALF, 20, 7 })) << "lead 0 is the first hour of the batch";
        EXPECT_EQ(store.priceSeriesAtLead(1, 1, std::nullopt, 10).front().row, record.prices[1]) << "and lead 1 the next";
        EXPECT_EQ(pointsOf(store.priceSeriesAtLead(1, 1, HALF, 10)), (Ids{ 20, 7 }));
        EXPECT_TRUE(store.priceSeriesAtLead(1, 2, std::nullopt, 10).empty());
        EXPECT_TRUE(store.priceSeriesAtLead(3, 0, std::nullopt, 10).empty());
    }

    {
        SCOPED_TRACE("the catalog is in the file, and a file written under another one is refused");
        EXPECT_EQ(scalar(db.path(), "SELECT count(*) FROM accelerators"), static_cast<std::int64_t>(catalog::ACCELERATOR_COUNT));
        EXPECT_EQ(scalar(db.path(), "SELECT id FROM accelerators WHERE name = 'H100-80GB'"), 1);
        EXPECT_EQ(scalar(db.path(), "SELECT chain_position FROM accelerators WHERE name = 'H200-141GB'"), 2);
        EXPECT_EQ(scalar(db.path(), "SELECT count(*) FROM regions"), static_cast<std::int64_t>(catalog::REGION_COUNT));
        EXPECT_EQ(scalar(db.path(),
                         "SELECT count(*) FROM orders o JOIN accelerators a ON o.accelerator_mask & (1 << a.id) "
                         "WHERE o.batch_id = 7 AND o.side = 0 AND o.rank = 0"),
                  2)
            << "a mask joins to the names it holds";
        const std::pair<const char*, const char*> changes[] = {
            { "UPDATE accelerators SET name = 'H100' WHERE id = 1", "UPDATE accelerators SET name = 'H100-80GB' WHERE id = 1" },
            { "UPDATE accelerators SET chain_position = 0 WHERE id = 2", "UPDATE accelerators SET chain_position = 2 WHERE id = 2" },
            { "UPDATE accelerators SET vendor = 'AMD' WHERE id = 0", "UPDATE accelerators SET vendor = 'NVIDIA' WHERE id = 0" },
            { "UPDATE interconnects SET name = 'PCIe5' WHERE id = 0", "UPDATE interconnects SET name = 'PCIe' WHERE id = 0" },
            { "DELETE FROM regions WHERE id = 2", "INSERT INTO regions VALUES (2, 'EU_WEST')" },
            { "INSERT INTO regions VALUES (3, 'AP_EAST')", "DELETE FROM regions WHERE id = 3" },
        };
        for (const auto& [change, undo] : changes) {
            rawExec(db.path(), change);
            EXPECT_THROW(ResultsStore(db.path()), StoreError) << change;
            EXPECT_THROW(ResultsStore(db.path(), ResultsStore::Mode::READ_ONLY), StoreError) << change;
            rawExec(db.path(), undo);
            EXPECT_NO_THROW(ResultsStore(db.path(), ResultsStore::Mode::READ_ONLY)) << undo;
        }
    }

    {
        SCOPED_TRACE("the open batch: changes are applied, a batch's write moves it on, and a reader sees one moment");
        EXPECT_FALSE(store.live());
        EXPECT_FALSE(store.openOrdersOf(101));
        EXPECT_TRUE(store.openOrders().empty());

        const OrderTerms bidTerms = makeTerms(H100 | H200, NVLINK, US_WEST | US_EAST, 0b110, 2, { { 3, 900 }, { 2, 800 }, { 1, 700 } });
        const OrderTerms askTerms = makeTerms(H200, NVLINK, EU_WEST, 0b1, 5, { { 5, 600 } });
        LiveChanges first;
        first.state = LiveState{ 8, 497'450, 120 };
        first.upserts = { openOrder(RowSide::BID, 0, 101, 1001, bidTerms), openOrder(RowSide::BID, 1, 102, 1002, bidTerms),
                          openOrder(RowSide::ASK, 0, 201, 2001, askTerms) };
        store.applyLive(first);
        EXPECT_EQ(store.live(), first.state);
        EXPECT_EQ(store.openOrders(), first.upserts);
        const std::optional<ResultsStore::OpenOrders> mine = store.openOrdersOf(101);
        ASSERT_TRUE(mine);
        EXPECT_EQ(mine->state, first.state);
        EXPECT_EQ(mine->orders, (std::vector<OpenOrderRow>{ first.upserts[0] }));
        EXPECT_EQ(store.openOrdersOf(201)->orders, (std::vector<OpenOrderRow>{ first.upserts[2] }));
        const std::optional<ResultsStore::OpenOrders> nobody = store.openOrdersOf(555);
        ASSERT_TRUE(nobody) << "the open batch is known even when the participant has nothing in it";
        EXPECT_EQ(nobody->state, first.state);
        EXPECT_TRUE(nobody->orders.empty());

        LiveChanges second;
        second.state = LiveState{ 8, 497'450, 130 };
        OrderTerms changed = bidTerms;
        changed.perSlotCap = 9;
        second.upserts = { openOrder(RowSide::BID, 1, 102, 1002, changed) };
        second.removals = { { RowSide::BID, 0 }, { RowSide::ASK, 9 } };
        store.applyLive(second);
        EXPECT_EQ(store.live(), second.state);
        EXPECT_EQ(store.openOrders(), (std::vector<OpenOrderRow>{ second.upserts[0], first.upserts[2] }))
            << "an order that is replaced, one that is removed, and a removal of nothing";
        EXPECT_TRUE(store.openOrdersOf(101)->orders.empty());

        const LiveState next{ 9, 497'451, 140 };
        ASSERT_EQ(store.write(sampleRecord(30), &next), ResultsStore::Outcome::WRITTEN);
        EXPECT_TRUE(store.openOrders().empty()) << "the batch's own write takes its open orders away";
        EXPECT_EQ(store.live(), next);

        store.applyLive(first);
        const LiveState later{ 10, 497'452, 150 };
        EXPECT_EQ(store.write(sampleRecord(30), &later), ResultsStore::Outcome::ALREADY_RECORDED)
            << "a batch that is recorded already still ends the open batch";
        EXPECT_TRUE(store.openOrders().empty());
        EXPECT_EQ(store.live(), later);

        store.applyLive(first);
        BatchRecord torn = sampleRecord(31);
        torn.fills.back().side = static_cast<RowSide>(7);
        EXPECT_THROW(store.write(torn, &next), StoreError);
        EXPECT_EQ(store.live(), first.state) << "a write that fails changes nothing, the open batch included";
        EXPECT_EQ(store.openOrders(), first.upserts);

        store.clearLive();
        EXPECT_FALSE(store.live());
        EXPECT_TRUE(store.openOrders().empty());
    }

    {
        SCOPED_TRACE("a change to any stored result is noticed, and undoing it is noticed too");
        const std::pair<const char*, const char*> changes[] = {
            { "UPDATE fills SET price = price + 1 WHERE batch_id = 7 AND side = 0 AND rank = 0 AND hour = 497449",
              "UPDATE fills SET price = price - 1 WHERE batch_id = 7 AND side = 0 AND rank = 0 AND hour = 497449" },
            { "UPDATE orders SET amount = amount + 1 WHERE batch_id = 7 AND side = 1",
              "UPDATE orders SET amount = amount - 1 WHERE batch_id = 7 AND side = 1" },
            { "UPDATE orders SET slot_mask = slot_mask + 4 WHERE batch_id = 7 AND side = 0 AND rank = 1",
              "UPDATE orders SET slot_mask = slot_mask - 4 WHERE batch_id = 7 AND side = 0 AND rank = 1" },
            { "UPDATE orders SET q3 = 5 WHERE batch_id = 7 AND side = 0 AND rank = 1",
              "UPDATE orders SET q3 = NULL WHERE batch_id = 7 AND side = 0 AND rank = 1" },
            { "UPDATE accelerator_prices SET price_max = price_max + 1 WHERE batch_id = 7 AND hour = 497450",
              "UPDATE accelerator_prices SET price_max = price_max - 1 WHERE batch_id = 7 AND hour = 497450" },
            { "UPDATE batches SET welfare = welfare + 1 WHERE batch_id = 7",
              "UPDATE batches SET welfare = welfare - 1 WHERE batch_id = 7" },
            { "DELETE FROM fills WHERE batch_id = 7 AND side = 1",
              "INSERT INTO fills VALUES (7, 1, 0, 497449, 1, 1, 0, 1800, 4, 0), (7, 1, 0, 497450, 1, 1, 0, 1900, 1, 0), "
              "(7, 1, 1, 497449, 1, 0, 0, 1700, 1, 0)" },
        };
        for (const auto& [tamper, restore] : changes) {
            rawExec(db.path(), tamper);
            EXPECT_FALSE(store.intact(7)) << tamper;
            rawExec(db.path(), restore);
            EXPECT_TRUE(store.intact(7)) << restore;
        }
    }

    {
        SCOPED_TRACE("a file that is not a results database is refused, whoever asks");
        rawExec(db.path(), "PRAGMA user_version = 9");
        EXPECT_THROW(ResultsStore(db.path()), StoreError);
        EXPECT_THROW(ResultsStore(db.path(), ResultsStore::Mode::READ_ONLY), StoreError);
        rawExec(db.path(), "PRAGMA user_version = 1");
        EXPECT_THROW(ResultsStore(db.path()), StoreError) << "the first version is not read either";
        rawExec(db.path(), "PRAGMA user_version = 2");
        EXPECT_NO_THROW(ResultsStore(db.path(), ResultsStore::Mode::READ_ONLY));

        TempFile other;
        rawExec(other.path(), "CREATE TABLE somebody_elses (a INTEGER)");
        EXPECT_THROW(ResultsStore(other.path()), StoreError);
        TempFile text;
        std::ofstream(text.path()) << "this is not a database, it is a sentence of some length, long enough to look like a file";
        EXPECT_THROW(ResultsStore(text.path()), StoreError);
        TempFile missing;
        EXPECT_THROW(ResultsStore(missing.path(), ResultsStore::Mode::READ_ONLY), StoreError);
    }
}

TEST(ResultsStore, KeepsTheFirstRecordOfABatchIdAndAWriteThatFailsHalfwayLeavesNothingBehind) {
    TempFile db;
    ResultsStore store(db.path());

    const BatchRecord first = sampleRecord(3);
    ASSERT_EQ(store.write(first), ResultsStore::Outcome::WRITTEN);
    EXPECT_EQ(store.write(first), ResultsStore::Outcome::ALREADY_RECORDED);

    BatchRecord other = sampleRecord(3);
    other.header.welfare += 1;
    other.header.eventSeq = 99;
    other.header.digest = batchDigest(other);
    EXPECT_EQ(store.write(other), ResultsStore::Outcome::ALREADY_RECORDED_DIFFERENTLY);
    EXPECT_EQ(store.batch(3), first.header) << "the first record is the one that stays";
    EXPECT_EQ(store.orders(3), first.orders);

    BatchRecord later = sampleRecord(3);
    later.header.eventSeq = 500;
    later.header.solverVersion = 2;
    EXPECT_EQ(store.write(later), ResultsStore::Outcome::ALREADY_RECORDED)
        << "the same results recorded at another time are the same record";

    BatchRecord torn = sampleRecord(4);
    torn.fills.back().side = static_cast<RowSide>(7);
    ASSERT_TRUE(isCanonical(torn));
    EXPECT_THROW(store.write(torn), StoreError) << "the last fill has a side that the schema refuses";
    EXPECT_FALSE(store.batch(4));
    EXPECT_TRUE(store.orders(4).empty()) << "the orders were inserted before the fill that failed";
    EXPECT_TRUE(store.fills(4).empty());
    EXPECT_TRUE(store.prices(4).empty());
    EXPECT_EQ(scalar(db.path(), "SELECT count(*) FROM orders WHERE batch_id = 4"), 0);
    EXPECT_EQ(store.write(sampleRecord(4)), ResultsStore::Outcome::WRITTEN) << "and the store carries on";
    EXPECT_TRUE(store.intact(4));

    BatchRecord noSteps = sampleRecord(6);
    noSteps.orders[0].terms.stepCount = 0;
    EXPECT_THROW(store.write(noSteps), StoreError) << "an order has at least one step";
    EXPECT_FALSE(store.batch(6));

    BatchRecord unsorted = sampleRecord(5);
    std::reverse(unsorted.fills.begin(), unsorted.fills.end());
    EXPECT_THROW(store.write(unsorted), StoreError);
    EXPECT_FALSE(store.batch(5));

    EXPECT_EQ(store.watermark().batches, 2u);
    EXPECT_EQ(store.watermark().lastBatchId, 4u);
    EXPECT_EQ(store.watermark().lastEventSeq, sampleRecord(4).header.eventSeq);
}

namespace {

BatchCommand cancelCommand(ParticipantId participant, OrderToken token) {
    BatchCommand c{};
    c.type = CommandType::CANCEL;
    c.participantId = participant;
    c.orderToken = token;
    return c;
}

BatchCommand clearCommand(BatchId id) {
    BatchCommand c{};
    c.type = CommandType::BATCH_CLEAR;
    c.batchId = id;
    return c;
}

// Batch 0 is the design's example plus a few more orders, a cancel and a bid that gets nothing; batch 1 is a
// random crowd; batch 2 is empty; the batch after that is cleared as id 10, and id 10 comes once more.
std::vector<BatchCommand> aDayOfCommands() {
    std::vector<BatchCommand> out;
    TraceId trace = 1;
    auto add = [&](BatchCommand c, SessionId session) {
        c.trace_id = trace++;
        c.sessionId = session;
        out.push_back(c);
    };
    auto order = [&](const Order& o, SessionId session) { add(o.command(), session); };

    order(bid(101, 1, 0b1111111111, 1, { { 2, 2400 }, { 3, 2200 }, { 5, 2000 } }, A100 | H100), 31);
    order(ask(201, 1, 0b1111, 4, { { 8, 1800 }, { 8, 2000 } }, H100), 32);
    order(bid(102, 1, 0b11, 3, { { 6, 3000 } }, H200), 33);
    order(ask(202, 1, 0b11, 3, { { 4, 700 }, { 4, 900 } }, H200), 34);
    order(bid(103, 1, 0b1, 1, { { 1, 5 } }, A100), 35);
    order(bid(104, 9, 0b1, 1, { { 1, 5000 } }, A100), 36);
    add(cancelCommand(104, 9), 36);
    Order bad = bid(105, 1, 0b0, 1, { { 1, 100 } }, A100);
    order(bad, 37);
    add(clearCommand(0), 99);

    Lcg rng{ 20261008 };
    std::vector<std::pair<ParticipantId, OrderToken>> placed;
    for (OrderToken t = 1; t <= 30; ++t) {
        const ParticipantId who = 1 + rng.below(8);
        order(randomBid(rng, who, t, H0 + 1), 40 + who);
        placed.emplace_back(who, t);
    }
    for (OrderToken t = 1; t <= 20; ++t) {
        const ParticipantId who = 11 + rng.below(4);
        order(randomAsk(rng, who, t, H0 + 1), 50 + who);
    }
    for (int k = 0; k < 5; ++k) {
        const auto [who, token] = placed[rng.below(static_cast<std::uint32_t>(placed.size()))];
        add(cancelCommand(who, token), 40 + who);
    }
    add(clearCommand(1), 99);

    add(clearCommand(2), 99);

    {
        Order lateBid = bid(106, 1, 0b1, 1, { { 1, 900 } }, A100);
        lateBid.baseHour = H0 + 3;
        order(lateBid, 38);
        Order lateAsk = ask(206, 1, 0b1, 1, { { 1, 400 } }, A100);
        lateAsk.baseHour = H0 + 3;
        order(lateAsk, 39);
    }
    add(clearCommand(10), 99);
    add(clearCommand(10), 99);
    return out;
}

struct TeePolicy {
    RecorderPolicy& recorder;
    std::vector<BatchEvent>& events;

    void on_order_event(const BatchEvent& e) {
        events.push_back(e);
        recorder.on_order_event(e);
    }
    void on_trace_complete(const OrderTrace&) {}
};

// An engine whose policy records into `store`, and keeps every event it emits.
class Recording {
public:
    Recording(ResultsStore& store, const EngineConfig& config)
        : policy(store, log, config.schedule), tee{ policy, events }, engine(tee, config) {
        policy.attach(bookLookupOf(engine.book()));
    }

    // Returns the events that the command caused.
    std::vector<BatchEvent> send(BatchCommand c) {
        const std::size_t before = events.size();
        engine.submit_order(c);
        for (std::size_t k = before; k < events.size(); ++k) {
            if (events[k].type == EventType::ACCEPTED) acceptedFrom[k] = c;
        }
        return std::vector<BatchEvent>(events.begin() + static_cast<std::ptrdiff_t>(before), events.end());
    }

    void feed(const std::vector<BatchCommand>& commands) {
        for (const BatchCommand& c : commands) send(c);
    }

    std::vector<BatchRecord> records() const { return recordsOf(events, acceptedFrom); }

    std::ostringstream log;
    std::vector<BatchEvent> events;
    AcceptedFrom acceptedFrom;
    RecorderPolicy policy;
    TeePolicy tee;
    BatchEngine<TeePolicy> engine;
};

// The open batch as the commands and the events say it must be, worked out apart from the recorder: the
// orders that were accepted and not cancelled, with the terms of the commands that carried them.
class OpenBatchModel {
public:
    void apply(const BatchCommand& command, const std::vector<BatchEvent>& caused) {
        for (const BatchEvent& e : caused) {
            const RowSide side = e.side == Side::BID ? RowSide::BID : RowSide::ASK;
            if (e.type == EventType::ACCEPTED) {
                orders_[{ side, e.rank }] = command;
                touch(e);
            } else if (e.type == EventType::CANCELED) {
                orders_.erase({ side, e.rank });
                touch(e);
            } else if (e.type == EventType::BATCH_DONE || e.type == EventType::BATCH_VOIDED) {
                orders_.clear();
                expected_ = e.batchId + 1;
                state_ = LiveState{ expected_, H0 + static_cast<HourIndex>(expected_), e.sequence_number };
            }
        }
    }

    BatchId expected() const { return expected_; }
    const std::optional<LiveState>& state() const { return state_; }

    std::vector<OpenOrderRow> orders() const {
        std::vector<OpenOrderRow> out;
        for (const auto& [key, command] : orders_) {
            out.push_back(OpenOrderRow{ key.first, key.second, command.participantId, command.orderToken,
                                        termsOfCommand(command, H0 + static_cast<HourIndex>(expected_)) });
        }
        return out;
    }

private:
    void touch(const BatchEvent& e) { state_ = LiveState{ expected_, H0 + static_cast<HourIndex>(expected_), e.sequence_number }; }

    BatchId expected_{ 0 };
    std::map<std::pair<RowSide, std::uint32_t>, BatchCommand> orders_;
    std::optional<LiveState> state_;
};

// A copy of the book as it is now, to answer lookups with after the engine has moved on to another batch.
BookLookup snapshotOf(const BatchBook& book) {
    auto bids = std::make_shared<std::vector<StoredOrder>>(book.orders(Side::BID));
    auto asks = std::make_shared<std::vector<StoredOrder>>(book.orders(Side::ASK));
    return [bids, asks](RowSide side, std::uint32_t rank) -> std::optional<BookedOrder> {
        const std::vector<StoredOrder>& orders = side == RowSide::BID ? *bids : *asks;
        if (rank >= orders.size()) return std::nullopt;
        return BookedOrder{ orders[rank].participant, orders[rank].token, orders[rank].live, termsOf(orders[rank]) };
    };
}

} // namespace

TEST(Recorder, WritesWhatTheEngineEmitsAndTwoRecordersAgreeAndAReplayChangesNothing) {
    const std::vector<BatchCommand> commands = aDayOfCommands();
    TempFile dbA;
    TempFile dbB;
    ResultsStore storeA(dbA.path());
    ResultsStore storeB(dbB.path());

    Recording a(storeA, testConfig());
    a.feed(commands);
    ASSERT_FALSE(a.policy.failed()) << a.policy.error();

    const std::vector<BatchRecord> ended = a.records();
    ASSERT_EQ(ended.size(), 5u) << "the clears of batches 0, 1 and 2, and of id 10 twice";
    std::map<BatchId, BatchRecord> first;
    std::uint64_t repeats = 0;
    std::uint64_t different = 0;
    for (const BatchRecord& record : ended) {
        const auto [it, fresh] = first.emplace(record.header.batchId, record);
        if (!fresh) {
            ++repeats;
            if (it->second.header.digest != record.header.digest) ++different;
        }
    }

    {
        SCOPED_TRACE("what the day was like");
        ASSERT_GE(first.at(0).fills.size(), 8u);
        ASSERT_GE(first.at(1).fills.size(), 40u) << "the random crowd trades a lot";
        EXPECT_EQ(first.at(0).header.firstHour, H0);
        EXPECT_EQ(first.at(1).header.firstHour, H0 + 1);
        EXPECT_EQ(first.at(10).header.firstHour, H0 + 3) << "the hours are those of the batch that was open, whatever id cleared it";
        EXPECT_EQ(first.at(2).header.bids + first.at(2).header.asks, 0u);
        EXPECT_TRUE(first.at(2).orders.empty());
        EXPECT_GT(first.at(0).header.liftedUnits, 0u);
        EXPECT_GE(first.at(1).orders.size(), 40u);
        EXPECT_EQ(repeats, 1u);
        EXPECT_EQ(different, 1u) << "the second clear of id 10 cleared another batch";

        const auto holds = [&](const BatchRecord& r, ParticipantId who, OrderToken token) {
            return std::any_of(r.orders.begin(), r.orders.end(), [&](const OrderRow& o) { return o.participant == who && o.token == token; });
        };
        EXPECT_TRUE(holds(first.at(0), 103, 1));
        EXPECT_FALSE(holds(first.at(0), 104, 9)) << "a cancelled order is not recorded";
        EXPECT_EQ(first.at(0).header.bids, 3u) << "and it is not a bid of the batch";
        std::vector<std::uint32_t> bidRanks;
        for (const OrderRow& o : first.at(1).orders) {
            if (o.side == RowSide::BID) bidRanks.push_back(o.rank);
        }
        ASSERT_FALSE(bidRanks.empty());
        EXPECT_GT(bidRanks.back() + 1, bidRanks.size()) << "cancelled orders leave gaps in the ranks, and the orders after them keep theirs";

        const OrderRow& example = first.at(0).orders.front();
        EXPECT_EQ(example.terms.acceleratorMask, A100 | H100);
        EXPECT_EQ(example.terms.stepCount, 3u);
        EXPECT_EQ(example.terms.totalUnits, 10u);
        EXPECT_EQ(example.terms.slotMask, 0b1111111111u) << "hours counted from the first hour of the batch";
        EXPECT_EQ(first.at(10).orders.front().terms.slotMask, 0b1u) << "an order of hour H0 + 3, in the batch that sells from it";
    }

    std::uint64_t orderRows = 0;
    std::uint64_t fillRows = 0;
    std::uint64_t priceRows = 0;
    for (const auto& [id, record] : first) {
        expectStoreHolds(storeA, record);
        orderRows += record.orders.size();
        fillRows += record.fills.size();
        priceRows += record.prices.size();
    }
    EXPECT_EQ(scalar(dbA.path(), "SELECT count(*) FROM batches"), static_cast<std::int64_t>(first.size()));
    EXPECT_EQ(scalar(dbA.path(), "SELECT count(*) FROM orders"), static_cast<std::int64_t>(orderRows));
    EXPECT_EQ(scalar(dbA.path(), "SELECT count(*) FROM fills"), static_cast<std::int64_t>(fillRows));
    EXPECT_EQ(scalar(dbA.path(), "SELECT count(*) FROM accelerator_prices"), static_cast<std::int64_t>(priceRows));
    EXPECT_EQ(a.policy.written(), first.size());
    EXPECT_EQ(a.policy.duplicates(), repeats);
    EXPECT_EQ(a.policy.mismatches(), different);
    EXPECT_NE(a.log.str().find("batch 10 was recorded before with different results"), std::string::npos) << a.log.str();

    {
        SCOPED_TRACE("a model's price is the money over the units, with the cheapest and dearest item that traded");
        bool sawRange = false;
        for (const auto& [id, record] : first) {
            for (const AcceleratorPriceRow& p : record.prices) {
                std::uint64_t units = 0;
                std::int64_t amount = 0;
                PriceTicks lowest = std::numeric_limits<PriceTicks>::max();
                PriceTicks highest = 0;
                for (const FillRow& f : record.fills) {
                    if (f.side != RowSide::BID || f.hour != p.hour || f.accelerator != p.accelerator) continue;
                    units += f.units;
                    amount += static_cast<std::int64_t>(f.units) * f.price;
                    lowest = std::min(lowest, f.price);
                    highest = std::max(highest, f.price);
                }
                EXPECT_EQ(p.units, units);
                EXPECT_EQ(p.amount, amount);
                EXPECT_EQ(p.priceMin, lowest);
                EXPECT_EQ(p.priceMax, highest);
                sawRange = sawRange || p.priceMin != p.priceMax;
            }
        }
        EXPECT_TRUE(sawRange) << "the crowd trades some model at more than one price in an hour";
    }

    {
        SCOPED_TRACE("a second recorder fed the same log holds the same batches, digests included");
        Recording b(storeB, testConfig());
        b.feed(commands);
        ASSERT_FALSE(b.policy.failed()) << b.policy.error();
        EXPECT_TRUE(sameEvents(a.events, b.events));
        for (const auto& [id, record] : first) {
            EXPECT_EQ(storeB.batch(id), storeA.batch(id));
            EXPECT_EQ(storeB.batch(id)->digest, record.header.digest);
            expectStoreHolds(storeB, record);
        }
        EXPECT_EQ(storeB.watermark().batches, storeA.watermark().batches);
        EXPECT_EQ(storeB.watermark().lastEventSeq, storeA.watermark().lastEventSeq);
    }

    {
        SCOPED_TRACE("a recorder that starts over on the same file, as a restarted one does, changes nothing");
        const ResultsStore::Watermark before = storeA.watermark();
        Recording again(storeA, testConfig());
        again.feed(commands);
        ASSERT_FALSE(again.policy.failed()) << again.policy.error();
        EXPECT_EQ(again.policy.written(), 0u);
        EXPECT_EQ(again.policy.duplicates(), ended.size());
        EXPECT_EQ(again.policy.mismatches(), different);
        EXPECT_EQ(storeA.watermark().batches, before.batches);
        EXPECT_EQ(storeA.watermark().lastEventSeq, before.lastEventSeq);
        EXPECT_EQ(scalar(dbA.path(), "SELECT count(*) FROM fills"), static_cast<std::int64_t>(fillRows));
        for (const auto& [id, record] : first) expectStoreHolds(storeA, record);
    }

    {
        SCOPED_TRACE("a batch that runs out of pivots is a voided row with no rows under it");
        TempFile dbC;
        ResultsStore storeC(dbC.path());
        EngineConfig tight = testConfig();
        tight.clearing.maxPivots = 1;
        Recording c(storeC, tight);
        std::vector<BatchCommand> market;
        TraceId trace = 1;
        auto add = [&](const Order& o) {
            BatchCommand cmd = o.command();
            cmd.trace_id = trace++;
            cmd.sessionId = 5;
            market.push_back(cmd);
        };
        for (ParticipantId p = 1; p <= 4; ++p) {
            add(bid(p, 1, 0b111, 2, { { 3, 900 - 10 * p }, { 3, 500 } }, A100 | H100));
            add(ask(10 + p, 1, 0b111, 2, { { 3, 100 + 10 * p }, { 3, 600 } }, p % 2 ? A100 : H100));
        }
        BatchCommand clear = clearCommand(0);
        clear.trace_id = trace++;
        clear.sessionId = 99;
        market.push_back(clear);
        c.feed(market);
        ASSERT_FALSE(c.policy.failed()) << c.policy.error();
        ASSERT_EQ(c.events.back().type, EventType::BATCH_VOIDED);
        const std::optional<BatchHeader> voided = storeC.batch(0);
        ASSERT_TRUE(voided);
        EXPECT_EQ(voided->status, BatchStatus::VOIDED);
        EXPECT_EQ(voided->voidReason, static_cast<std::uint8_t>(VoidReason::PIVOT_BUDGET));
        EXPECT_EQ(voided->firstHour, H0);
        EXPECT_EQ(voided->bids, 4u);
        EXPECT_EQ(voided->asks, 4u);
        EXPECT_TRUE(storeC.fills(0).empty());
        EXPECT_TRUE(storeC.orders(0).empty());
        EXPECT_TRUE(storeC.intact(0));
        expectStoreHolds(storeC, c.records().front());
    }
}

TEST(Recorder, KeepsTheOpenBatchAsOrdersComeAndGoAndHandsItOnWhenTheBatchEnds) {
    std::vector<BatchCommand> commands = aDayOfCommands();
    {
        Order stillOpen = bid(107, 1, 0b11, 2, { { 2, 900 }, { 1, 800 } }, H100 | H200, NVLINK | PCIE, US_WEST | EU_WEST);
        stillOpen.baseHour = H0 + 11;
        BatchCommand c = stillOpen.command();
        c.trace_id = 5000;
        c.sessionId = 77;
        commands.push_back(c);
    }
    const auto expectLive = [&](const ResultsStore& store, const OpenBatchModel& model, const std::string& when) {
        SCOPED_TRACE(when);
        EXPECT_EQ(store.openOrders(), model.orders());
        EXPECT_EQ(store.live(), model.state());
    };

    {
        SCOPED_TRACE("every command in turn, the open orders written by flushing and the batches by their own write");
        TempFile db;
        ResultsStore store(db.path());
        Recording rec(store, testConfig());
        OpenBatchModel model;
        bool deferred = false;
        for (std::size_t step = 0; step < commands.size(); ++step) {
            const std::vector<BatchEvent> caused = rec.send(commands[step]);
            model.apply(commands[step], caused);
            if (!deferred && !model.orders().empty()) {
                EXPECT_TRUE(store.openOrders().empty()) << "nothing is written for an order until the next flush";
                deferred = true;
            }
            const bool clearIsNext = step + 1 < commands.size() && commands[step + 1].type == CommandType::BATCH_CLEAR;
            if (clearIsNext) continue; // the changes wait for the flush that the clear takes the place of
            rec.policy.flushLive();
            ASSERT_FALSE(rec.policy.failed()) << rec.policy.error();
            expectLive(store, model, "after command " + std::to_string(step));
        }
        EXPECT_TRUE(deferred);
        ASSERT_EQ(model.orders().size(), 1u);
        EXPECT_EQ(model.expected(), 11u);
        EXPECT_EQ(model.state()->batchId, 11u);
        EXPECT_EQ(store.openOrders().front().terms.slotMask, 0b11u);
        EXPECT_EQ(store.openOrders().front().terms.acceleratorMask, H100 | H200);
        EXPECT_EQ(store.openOrdersOf(107)->orders, store.openOrders());
        EXPECT_TRUE(store.openOrdersOf(101)->orders.empty());
        EXPECT_EQ(store.openOrdersOf(107)->state.firstHour, H0 + 11);

        SCOPED_TRACE("a recorder that starts over replays batches that are recorded already, and their orders are not open");
        store.clearLive();
        Recording again(store, testConfig());
        OpenBatchModel replayed;
        const std::set<BatchId> recorded = { 0, 1, 2, 10 };
        for (const BatchCommand& command : commands) {
            const std::vector<BatchEvent> caused = again.send(command);
            replayed.apply(command, caused);
            again.policy.flushLive();
            if (recorded.count(replayed.expected()) != 0) {
                EXPECT_TRUE(store.openOrders().empty()) << "the batch that is open there has been recorded";
            }
        }
        ASSERT_FALSE(again.policy.failed()) << again.policy.error();
        expectLive(store, replayed, "after the replay, which reaches the batch that is still open");
        EXPECT_EQ(store.openOrders().size(), 1u);
    }

    {
        SCOPED_TRACE("a thread that flushes all the while, as the program does, loses and invents nothing");
        TempFile db;
        ResultsStore store(db.path());
        Recording rec(store, testConfig());
        OpenBatchModel model;
        std::atomic<bool> done{ false };
        std::atomic<std::uint64_t> flushes{ 0 };
        std::thread flusher([&] {
            while (!done.load()) {
                rec.policy.flushLive();
                ++flushes;
                std::this_thread::yield();
            }
        });
        for (const BatchCommand& command : commands) model.apply(command, rec.send(command));
        done = true;
        flusher.join();
        EXPECT_GT(flushes.load(), 0u);
        ASSERT_FALSE(rec.policy.failed()) << rec.policy.error();
        rec.policy.flushLive();
        expectLive(store, model, "at the end");
        EXPECT_EQ(rec.policy.written(), 4u);
        std::map<BatchId, BatchRecord> firstOfEach;
        for (const BatchRecord& record : rec.records()) firstOfEach.emplace(record.header.batchId, record);
        for (const auto& [id, record] : firstOfEach) expectStoreHolds(store, record);
    }
}

TEST(Recorder, StopsInsteadOfRecordingEventsThatDisagreeOrThatCannotBeWritten) {
    Harness h;
    h.order(bid(101, 1, 0b1111111111, 1, { { 2, 2400 }, { 3, 2200 }, { 5, 2000 } }, A100 | H100));
    h.order(ask(201, 1, 0b1111, 4, { { 8, 1800 }, { 8, 2000 } }, H100));
    h.order(bid(102, 1, 0b11, 3, { { 6, 3000 } }, H200));
    h.order(ask(202, 1, 0b11, 3, { { 4, 700 }, { 4, 900 } }, H200));
    h.order(bid(103, 1, 0b1, 1, { { 1, 5 } }, A100));
    const BookLookup book = snapshotOf(h.engine.book());
    const std::vector<BatchEvent> batch = h.clearBatch(0);
    ASSERT_EQ(batch.back().type, EventType::BATCH_DONE);
    ASSERT_GE(batch.size(), 10u);

    auto feed = [](const std::vector<BatchEvent>& events, const BookLookup& lookup) -> std::optional<BatchRecord> {
        BatchRecorder recorder(lookup);
        std::optional<BatchRecord> last;
        for (const BatchEvent& e : events) last = recorder.onEvent(e);
        return last;
    };
    ASSERT_TRUE(feed(batch, book)) << "the events as the engine emitted them make a record";
    for (const BatchEvent& e : h.events) {
        if (e.type == EventType::ACCEPTED || e.type == EventType::REJECTED || e.type == EventType::CANCELED) {
            BatchRecorder recorder;
            EXPECT_FALSE(recorder.onEvent(e)) << "events about orders going in are not recorded";
        }
    }

    auto firstOf = [&](EventType type) {
        return static_cast<std::size_t>(std::find_if(batch.begin(), batch.end(), [&](const BatchEvent& e) { return e.type == type; }) - batch.begin());
    };
    const std::size_t fill = firstOf(EventType::FILL);
    const std::size_t done = firstOf(EventType::ORDER_DONE);
    ASSERT_LT(fill, batch.size());
    ASSERT_LT(done, batch.size());
    const std::size_t lastDone = static_cast<std::size_t>(std::find_if(batch.rbegin(), batch.rend(), [](const BatchEvent& e) { return e.type == EventType::ORDER_DONE; }).base() - batch.begin()) - 1;

    const std::pair<const char*, std::function<void(std::vector<BatchEvent>&)>> breakages[] = {
        { "a fill is missing", [&](std::vector<BatchEvent>& v) { v.erase(v.begin() + static_cast<std::ptrdiff_t>(fill)); } },
        { "a fill has more units than its order got", [&](std::vector<BatchEvent>& v) { v[fill].units += 1; } },
        { "a fill belongs to another order", [&](std::vector<BatchEvent>& v) { v[fill].orderToken += 1; } },
        { "an item is filled at two prices", [&](std::vector<BatchEvent>& v) { v[lastDone - 1].price += 1; } },
        { "the orders are not in rank order", [&](std::vector<BatchEvent>& v) { std::swap(v[done], v[lastDone]); } },
        { "the batch counts one bid too many", [&](std::vector<BatchEvent>& v) { v.back().bids += 1; } },
        { "the batch counts one fill too many", [&](std::vector<BatchEvent>& v) { v.back().fills += 1; } },
        { "the batch counts one unit too many", [&](std::vector<BatchEvent>& v) { v.back().units += 1; } },
        { "a fill has no order done after it", [&](std::vector<BatchEvent>& v) { v.erase(v.begin() + static_cast<std::ptrdiff_t>(lastDone)); } },
        { "a voided batch with fills", [&](std::vector<BatchEvent>& v) { v.back().type = EventType::BATCH_VOIDED; } },
        { "an order is done with more than it asked for", [&](std::vector<BatchEvent>& v) { v[done].unfilledUnits += 1; } },
        { "an order is done that the book has under another token", [&](std::vector<BatchEvent>& v) {
              v[done].orderToken += 1;
              for (std::size_t k = fill; k < done; ++k) v[k].orderToken += 1;
          } },
    };
    for (const auto& [what, breakIt] : breakages) {
        std::vector<BatchEvent> broken = batch;
        breakIt(broken);
        EXPECT_THROW(feed(broken, book), std::logic_error) << what;
    }

    const std::pair<const char*, BookLookup> books[] = {
        { "no book at all", BookLookup() },
        { "a book that holds nothing", [](RowSide, std::uint32_t) { return std::optional<BookedOrder>(); } },
        { "a book where the order is not live", [&](RowSide side, std::uint32_t rank) {
              std::optional<BookedOrder> order = book(side, rank);
              if (order) order->live = false;
              return order;
          } },
        { "a book that holds another participant's order there", [&](RowSide side, std::uint32_t rank) {
              std::optional<BookedOrder> order = book(side, rank);
              if (order) order->participant += 1;
              return order;
          } },
        { "a book that holds an order of another size there", [&](RowSide side, std::uint32_t rank) {
              std::optional<BookedOrder> order = book(side, rank);
              if (order) order->terms.totalUnits += 1;
              return order;
          } },
    };
    for (const auto& [what, lookup] : books) EXPECT_THROW(feed(batch, lookup), std::logic_error) << what;

    {
        SCOPED_TRACE("a recorder that cannot write says so, stops, and does not hold up the engine");
        TempFile db;
        ResultsStore creator(db.path());
        ResultsStore readOnly(db.path(), ResultsStore::Mode::READ_ONLY);
        std::ostringstream log;
        RecorderPolicy policy(readOnly, log, testSchedule());
        policy.attach(book);
        for (const BatchEvent& e : batch) policy.on_order_event(e);
        EXPECT_TRUE(policy.failed());
        EXPECT_FALSE(policy.error().empty());
        EXPECT_EQ(policy.written(), 0u);
        EXPECT_NE(log.str().find("stopped recording"), std::string::npos);
        EXPECT_NO_THROW(policy.on_order_event(batch.back())) << "once stopped, events are ignored";
        EXPECT_NO_THROW(policy.flushLive());
    }
    {
        SCOPED_TRACE("nor can it write the open orders");
        TempFile db;
        ResultsStore creator(db.path());
        ResultsStore readOnly(db.path(), ResultsStore::Mode::READ_ONLY);
        std::ostringstream log;
        RecorderPolicy policy(readOnly, log, testSchedule());
        policy.attach(book);
        const auto accepted = std::find_if(h.events.begin(), h.events.end(), [](const BatchEvent& e) { return e.type == EventType::ACCEPTED; });
        ASSERT_NE(accepted, h.events.end());
        policy.on_order_event(*accepted);
        EXPECT_FALSE(policy.failed()) << "an order is kept in memory until the flush";
        policy.flushLive();
        EXPECT_TRUE(policy.failed());
    }
    {
        SCOPED_TRACE("so does one that is given events that disagree");
        TempFile db;
        ResultsStore store(db.path());
        std::ostringstream log;
        RecorderPolicy policy(store, log, testSchedule());
        policy.attach(book);
        std::vector<BatchEvent> broken = batch;
        broken.back().bids += 1;
        for (const BatchEvent& e : broken) policy.on_order_event(e);
        EXPECT_TRUE(policy.failed());
        EXPECT_FALSE(store.batch(0)) << "nothing of a batch that does not add up is kept";
    }
    {
        SCOPED_TRACE("or an order that the book does not hold");
        TempFile db;
        ResultsStore store(db.path());
        std::ostringstream log;
        RecorderPolicy policy(store, log, testSchedule());
        policy.attach([](RowSide, std::uint32_t) { return std::optional<BookedOrder>(); });
        const auto accepted = std::find_if(h.events.begin(), h.events.end(), [](const BatchEvent& e) { return e.type == EventType::ACCEPTED; });
        ASSERT_NE(accepted, h.events.end());
        policy.on_order_event(*accepted);
        EXPECT_TRUE(policy.failed());
    }
}

TEST(Recorder, RunsAsAProgramThatFollowsTheLogAndStopsOnSigterm) {
    const schedule::Schedule schedule{};
    const HourIndex first = static_cast<HourIndex>(schedule::firstSlotHour(schedule, 0));
    std::vector<BatchCommand> commands;
    auto add = [&](BatchCommand c, SessionId session) {
        c.trace_id = static_cast<TraceId>(commands.size());
        c.sessionId = session;
        commands.push_back(c);
    };
    Order buy = bid(101, 1, 0b1111111111, 1, { { 2, 2400 }, { 3, 2200 }, { 5, 2000 } }, A100 | H100);
    Order sell = ask(201, 1, 0b1111, 4, { { 8, 1800 }, { 8, 2000 } }, H100);
    buy.baseHour = first;
    sell.baseHour = first;
    add(buy.command(), 1);
    add(sell.command(), 2);
    const std::size_t ordersCount = commands.size();
    add(clearCommand(0), 3);

    Harness inProcess{ EngineConfig{} };
    for (const BatchCommand& c : commands) inProcess.send(c, c.sessionId);
    const std::vector<BatchRecord> expected = recordsOf(inProcess);
    ASSERT_EQ(expected.size(), 1u);
    ASSERT_EQ(expected.front().header.units, 4u);

    TempFile db;
    TempFile log;
    const std::uint16_t port = freeUdpPort();
    Child recorder;
    ASSERT_TRUE(recorder.spawn({ RECORDER_APP, "--db", db.path(), "--multicast-ip", "127.0.0.1", "--multicast-port",
                                 std::to_string(port), "--retransmit-port", std::to_string(freeUdpPort()), "--id", "test" },
                               log.path()));
    ASSERT_TRUE(waitUntil(std::chrono::seconds(20), [&] { return slurp(log.path()).find("ready") != std::string::npos; }))
        << "the recorder never started listening: " << slurp(log.path());

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    const auto send = [&](std::uint64_t seq) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(port);
        SequencedInboundMessage<BatchCommand> message{};
        message.sequenceNumber = seq;
        message.ingressTaiNs = 1'000'000 + seq;
        message.clientSessionId = commands[seq].sessionId;
        message.command = commands[seq];
        ASSERT_EQ(::sendto(fd, &message, sizeof(message), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)),
                  static_cast<ssize_t>(sizeof(message)));
    };
    // What `look` finds in the file, which is an optional, once it finds something.
    const auto read = [&](auto&& look) {
        std::invoke_result_t<decltype(look), ResultsStore&> found;
        const auto by = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!found && std::chrono::steady_clock::now() < by) {
            try {
                ResultsStore reader(db.path(), ResultsStore::Mode::READ_ONLY);
                found = look(reader);
            } catch (const StoreError&) {
            }
            if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return found;
    };

    for (std::uint64_t seq = 0; seq < ordersCount; ++seq) send(seq);
    {
        SCOPED_TRACE("before the clear, the program writes the open orders every 50 ms");
        const std::vector<OpenOrderRow> want = { OpenOrderRow{ RowSide::BID, 0, 101, 1, termsOfCommand(commands[0], first) },
                                                 OpenOrderRow{ RowSide::ASK, 0, 201, 1, termsOfCommand(commands[1], first) } };
        const auto open = read([&](ResultsStore& store) -> std::optional<std::vector<OpenOrderRow>> {
            std::vector<OpenOrderRow> rows = store.openOrders();
            if (rows.size() == want.size()) return rows;
            return std::nullopt;
        });
        ASSERT_TRUE(open) << "no open orders were written";
        EXPECT_EQ(*open, want);
        ResultsStore reader(db.path(), ResultsStore::Mode::READ_ONLY);
        ASSERT_TRUE(reader.live());
        EXPECT_EQ(reader.live()->batchId, 0u);
        EXPECT_EQ(reader.live()->firstHour, first);
        EXPECT_FALSE(reader.batch(0)) << "nothing has cleared";
    }

    send(ordersCount);
    const std::optional<BatchHeader> recorded = read([](ResultsStore& store) { return store.batch(0); });
    ASSERT_TRUE(recorded) << "no batch was recorded";
    {
        ResultsStore reader(db.path(), ResultsStore::Mode::READ_ONLY);
        expectStoreHolds(reader, expected.front());
        EXPECT_TRUE(reader.openOrders().empty()) << "the batch took its open orders with it";
        ASSERT_TRUE(reader.live());
        EXPECT_EQ(reader.live()->batchId, 1u) << "and the open batch is the next";
        EXPECT_EQ(reader.live()->firstHour, first + 1);
    }
    ::close(fd);

    recorder.terminate();
    const std::optional<int> status = recorder.wait(std::chrono::seconds(10));
    ASSERT_TRUE(status) << "the recorder did not stop";
    EXPECT_EQ(*status, 0);
    const std::string text = slurp(log.path());
    EXPECT_NE(text.find("Recorder[test] shutting down"), std::string::npos) << text;
}
