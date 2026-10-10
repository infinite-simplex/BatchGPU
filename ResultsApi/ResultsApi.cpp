#include "ResultsApi.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include "BatchEvent.hpp"
#include "Catalog.hpp"
#include "HourTime.hpp"
#include "Json.hpp"
#include "Limits.hpp"
#include "ResultsStore.hpp"

namespace api {

namespace {

using recorder::AcceleratorPriceRow;
using recorder::BatchHeader;
using recorder::BatchStatus;
using recorder::FillRow;
using recorder::OpenOrderRow;
using recorder::OrderRow;
using recorder::OrderTerms;
using recorder::ResultsStore;
using recorder::RowSide;
using recorder::StoreError;

constexpr std::size_t DEFAULT_LIMIT = 100;
constexpr std::size_t MAX_LIMIT = 1000;
constexpr std::uint64_t MAX_OFFSET = 1'000'000'000;
constexpr std::uint64_t MAX_WAIT_MS = 5000;
constexpr auto WAIT_STEP = std::chrono::milliseconds(20);

// What a client did wrong, or what cannot be served, with the status to answer.
struct ApiError {
    int status;
    std::string message;
};

// Client text that goes into a message: letters, digits and a few marks, and nothing that could mean anything else.
std::string printable(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size() && i < 40; ++i) {
        const char c = text[i];
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-') ? c : '?';
    }
    if (text.size() > 40) out += "...";
    return out;
}

bool parseUnsigned(std::string_view text, std::uint64_t& out) {
    if (text.empty() || text.size() > 20 || text.find_first_not_of("0123456789") != std::string_view::npos) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc() && end == text.data() + text.size();
}

std::uint64_t parseId(std::string_view text, const char* what) {
    std::uint64_t id = 0;
    if (!parseUnsigned(text, id)) throw ApiError{ 400, std::string(what) + " must be a whole number of at most 20 digits, not " + printable(text) };
    return id;
}

// The query parameters of a request: each from a known list and each at most once.
class Params {
public:
    Params(const Request& request, std::initializer_list<std::string_view> known) {
        for (const auto& [key, value] : request.query) {
            if (std::find(known.begin(), known.end(), std::string_view(key)) == known.end()) {
                throw ApiError{ 400, "unknown parameter " + printable(key) };
            }
            if (!values_.emplace(key, value).second) throw ApiError{ 400, "parameter " + key + " is given twice" };
        }
    }

    std::optional<std::string> text(const std::string& name) const {
        const auto it = values_.find(name);
        if (it == values_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<std::uint64_t> number(const std::string& name, std::uint64_t most = std::numeric_limits<std::uint64_t>::max()) const {
        const std::optional<std::string> value = text(name);
        if (!value) return std::nullopt;
        std::uint64_t n = 0;
        if (!parseUnsigned(*value, n) || n > most) {
            throw ApiError{ 400, name + " must be a whole number" +
                                     (most == std::numeric_limits<std::uint64_t>::max() ? "" : " of at most " + std::to_string(most)) };
        }
        return n;
    }

    std::size_t limit() const {
        const std::optional<std::uint64_t> n = number("limit");
        if (!n) return DEFAULT_LIMIT;
        if (*n < 1 || *n > MAX_LIMIT) throw ApiError{ 400, "limit must be from 1 to " + std::to_string(MAX_LIMIT) };
        return static_cast<std::size_t>(*n);
    }

    std::size_t offset() const { return static_cast<std::size_t>(number("offset", MAX_OFFSET).value_or(0)); }

    std::chrono::milliseconds waitMs() const {
        return std::chrono::milliseconds(static_cast<std::int64_t>(number("wait_ms", MAX_WAIT_MS).value_or(0)));
    }

    bool flag(const std::string& name) const {
        const std::optional<std::string> value = text(name);
        if (!value) return false;
        if (*value == "1" || *value == "true") return true;
        if (*value == "0" || *value == "false") return false;
        throw ApiError{ 400, name + " must be 1, 0, true or false" };
    }

private:
    std::map<std::string, std::string> values_;
};

std::string hex(const recorder::Digest& digest) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : digest) {
        out += DIGITS[byte >> 4];
        out += DIGITS[byte & 15];
    }
    return out;
}

const char* sideName(RowSide side) { return side == RowSide::BID ? "bid" : "ask"; }

const char* acceleratorName(unsigned accelerator) {
    return accelerator < catalog::ACCELERATOR_COUNT ? catalog::ACCELERATORS[accelerator].name : "?";
}

std::string skuName(unsigned accelerator, unsigned interconnect, unsigned region) {
    return std::string(acceleratorName(accelerator)) + "/" +
           (interconnect < catalog::INTERCONNECT_COUNT ? catalog::INTERCONNECTS[interconnect] : "?") + "/" +
           (region < catalog::REGION_COUNT ? catalog::REGIONS[region] : "?");
}

void writeStarts(JsonWriter& json, const char* key, std::uint64_t hour) {
    json.key(key);
    if (const std::optional<std::string> text = hourStart(hour)) json.string(*text);
    else json.null();
}

void writeHeader(JsonWriter& json, const BatchHeader& h) {
    json.beginObject();
    json.key("batch_id").id(h.batchId);
    json.key("status").string(h.status == BatchStatus::CLEARED ? "cleared" : "voided");
    json.key("void_reason");
    if (h.status == BatchStatus::VOIDED) json.string(toString(static_cast<VoidReason>(h.voidReason)));
    else json.null();
    json.key("first_hour").number(h.firstHour);
    writeStarts(json, "first_hour_starts", h.firstHour);
    json.key("event_seq").number(h.eventSeq);
    json.key("bids").number(h.bids);
    json.key("asks").number(h.asks);
    json.key("arcs").number(h.arcs);
    json.key("pivots").number(h.pivots);
    json.key("units").number(h.units);
    json.key("lifted_units").number(h.liftedUnits);
    json.key("welfare").number(h.welfare);
    json.key("rank_sum").number(h.rankSum);
    json.key("fills").number(h.fills);
    json.key("certified").boolean(h.certified != 0);
    json.key("solver_version").number(h.solverVersion);
    json.key("digest").string(hex(h.digest));
    json.endObject();
}

template <typename NameOf>
void writeNames(JsonWriter& json, const char* key, std::uint64_t mask, std::size_t count, NameOf nameOf) {
    json.key(key).beginArray();
    for (std::size_t i = 0; i < count && i < 64; ++i) {
        if ((mask >> i) & 1) json.string(nameOf(i));
    }
    json.endArray();
}

// The runs of consecutive hours of an order. Bit s of the mask is the hour firstHour + s.
void writeHours(JsonWriter& json, HourIndex firstHour, std::uint64_t slotMask) {
    json.key("hours").beginArray();
    unsigned bit = 0;
    while (bit < 64) {
        if (((slotMask >> bit) & 1) == 0) {
            ++bit;
            continue;
        }
        unsigned last = bit;
        while (last + 1 < 64 && ((slotMask >> (last + 1)) & 1) != 0) ++last;
        const std::uint64_t from = std::uint64_t{ firstHour } + bit;
        const std::uint64_t to = std::uint64_t{ firstHour } + last;
        json.beginObject();
        json.key("from").number(from);
        writeStarts(json, "from_starts", from);
        json.key("to").number(to);
        writeStarts(json, "to_starts", to);
        json.endObject();
        bit = last + 1;
    }
    json.endArray();
}

void writeTerms(JsonWriter& json, const OrderTerms& t, HourIndex firstHour) {
    writeNames(json, "accelerators", t.acceleratorMask, catalog::ACCELERATOR_COUNT, [](std::size_t i) { return catalog::ACCELERATORS[i].name; });
    writeNames(json, "interconnects", t.interconnectMask, catalog::INTERCONNECT_COUNT, [](std::size_t i) { return catalog::INTERCONNECTS[i]; });
    writeNames(json, "regions", t.regionMask, catalog::REGION_COUNT, [](std::size_t i) { return catalog::REGIONS[i]; });
    writeHours(json, firstHour, t.slotMask);
    json.key("per_slot_cap").number(t.perSlotCap);
    json.key("total_units").number(t.totalUnits);
    json.key("curve").beginArray();
    for (std::size_t k = 0; k < t.stepCount && k < t.stepQuantity.size(); ++k) {
        json.beginObject().key("quantity").number(t.stepQuantity[k]).key("price").number(t.stepPrice[k]).endObject();
    }
    json.endArray();
}

void writeOrderFields(JsonWriter& json, const OrderRow& r, HourIndex firstHour) {
    json.key("side").string(sideName(r.side));
    json.key("rank").number(r.rank);
    json.key("participant_id").number(r.participant);
    json.key("order_token").id(r.token);
    writeTerms(json, r.terms, firstHour);
    json.key("filled_units").number(r.filled);
    json.key("unfilled_units").number(r.unfilled());
    json.key("lifted_units").number(r.lifted);
    json.key("amount").number(r.amount);
}

void writeOpenOrder(JsonWriter& json, const OpenOrderRow& r, HourIndex firstHour) {
    json.beginObject();
    json.key("side").string(sideName(r.side));
    json.key("rank").number(r.rank);
    json.key("participant_id").number(r.participant);
    json.key("order_token").id(r.token);
    writeTerms(json, r.terms, firstHour);
    json.endObject();
}

void writeItemFields(JsonWriter& json, std::uint32_t hour, unsigned accelerator, unsigned interconnect, unsigned region) {
    json.key("hour").number(hour);
    writeStarts(json, "starts", hour);
    json.key("accelerator").number(accelerator);
    json.key("interconnect").number(interconnect);
    json.key("region").number(region);
    json.key("sku").string(skuName(accelerator, interconnect, region));
}

void writeFill(JsonWriter& json, const FillRow& r) {
    json.beginObject();
    json.key("side").string(sideName(r.side));
    json.key("rank").number(r.rank);
    writeItemFields(json, r.hour, r.accelerator, r.interconnect, r.region);
    json.key("price").number(r.price);
    json.key("units").number(r.units);
    json.key("lifted").boolean(r.lifted != 0);
    json.endObject();
}

// The fields of an accelerator's price in an hour, written into an object that is open. The average is the
// money over the units, to a hundredth of a tick, rounded half up.
void writePriceFields(JsonWriter& json, const AcceleratorPriceRow& r) {
    json.key("hour").number(r.hour);
    writeStarts(json, "starts", r.hour);
    json.key("accelerator").number(r.accelerator);
    json.key("name").string(acceleratorName(r.accelerator));
    json.key("units").number(r.units);
    json.key("amount").number(r.amount);
    json.key("price_min").number(r.priceMin);
    json.key("price_max").number(r.priceMax);
    json.key("price_avg");
    if (r.units == 0 || r.amount < 0) {
        json.null();
    } else {
        json.fixed((static_cast<std::uint64_t>(r.amount) * 100 + r.units / 2) / r.units, 2);
    }
}

Response ok(JsonWriter& json) { return Response{ 200, json.take(), "" }; }

} // namespace

struct ResultsApi::Impl {
    struct Source {
        std::string name;
        std::string path;
        std::unique_ptr<ResultsStore> store;
    };

    // A recorder that has a batch, with its header.
    struct Held {
        Source* source;
        BatchHeader header;
    };

    std::vector<Source> sources;
    ApiOptions options;

    // The file may not be there yet when the API starts, or be being made, so it is opened when a request needs it.
    ResultsStore* open(Source& source) {
        if (!source.store) {
            try {
                source.store = std::make_unique<ResultsStore>(source.path, ResultsStore::Mode::READ_ONLY);
            } catch (const StoreError&) {
                return nullptr;
            }
        }
        return source.store.get();
    }

    // A read that fails drops the connection, which the next request opens again.
    template <typename F>
    auto read(Source& source, F&& f) {
        ResultsStore* store = open(source);
        if (!store) throw ApiError{ 503, source.name + " could not be opened" };
        try {
            return f(*store);
        } catch (const StoreError&) {
            source.store.reset();
            throw ApiError{ 503, source.name + " could not be read" };
        }
    }

    bool anyReadable() {
        return std::any_of(sources.begin(), sources.end(), [&](Source& s) { return open(s) != nullptr; });
    }

    std::vector<Held> holders(BatchId id) {
        std::vector<Held> found;
        for (Source& source : sources) {
            if (!open(source)) continue;
            try {
                if (const std::optional<BatchHeader> header = source.store->batch(id)) found.push_back(Held{ &source, *header });
            } catch (const StoreError&) {
                source.store.reset();
            }
        }
        return found;
    }

    // A recorder may be a moment behind the engine, so a client can ask to wait for the batch.
    std::vector<Held> awaitHolders(BatchId id, std::chrono::milliseconds wait) {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        for (;;) {
            std::vector<Held> found = holders(id);
            if (!found.empty() || std::chrono::steady_clock::now() >= deadline) return found;
            std::this_thread::sleep_for(WAIT_STEP);
        }
    }

    static bool agree(const std::vector<Held>& held) {
        return std::all_of(held.begin(), held.end(), [&](const Held& h) { return h.header.digest == held.front().header.digest; });
    }

    // How far each recorder has got, so that a client who finds nothing can tell "not there" from "not yet".
    void writeWatermarks(JsonWriter& json) {
        json.key("watermarks").beginArray();
        for (Source& source : sources) {
            std::optional<ResultsStore::Watermark> mark;
            if (open(source)) {
                try {
                    mark = source.store->watermark();
                } catch (const StoreError&) {
                    source.store.reset();
                }
            }
            json.beginObject().key("recorder").string(source.name).key("readable").boolean(mark.has_value());
            if (mark) {
                json.key("batches").number(mark->batches);
                json.key("last_batch_id");
                if (mark->batches == 0) json.null();
                else json.id(mark->lastBatchId);
                json.key("last_event_seq").number(mark->lastEventSeq);
            }
            json.endObject();
        }
        json.endArray();
    }

    static void writeHeldBy(JsonWriter& json, const std::vector<Held>& held) {
        json.key("held_by").beginArray();
        for (const Held& h : held) json.string(h.source->name);
        json.endArray();
        json.key("agree").boolean(agree(held));
    }

    Response notFound(const std::string& what) {
        if (!anyReadable()) throw ApiError{ 503, "no recorder file can be read" };
        JsonWriter json;
        json.beginObject().key("error").string(what);
        writeWatermarks(json);
        json.endObject();
        return Response{ 404, json.take(), "" };
    }

    Response root() {
        JsonWriter json;
        json.beginObject();
        json.key("service").string("BatchGPU results");
        json.key("read_only").boolean(true);
        json.key("ids").string("batch ids and order tokens are 64-bit numbers, written as strings");
        json.key("units").beginObject();
        json.key("price").string("ticks; 1 tick is $0.01 per node-hour");
        json.key("price_avg").string("ticks, to a hundredth: the money of a model in an hour over its units");
        json.key("amount").string("ticks");
        json.key("quantity").string("node-hours");
        json.endObject();
        json.key("endpoints").beginArray();
        for (const char* path : { "/status", "/batches?limit&before", "/batches/{id}?wait_ms&verify",
                                  "/batches/{id}/orders?limit&offset&wait_ms", "/batches/{id}/orders/{bid|ask}/{rank}?wait_ms",
                                  "/batches/{id}/fills?limit&offset&wait_ms", "/batches/{id}/prices?wait_ms",
                                  "/prices?accelerator&hour&lead&limit&before", "/participants/{id}/orders?limit&cursor&token" }) {
            json.string(path);
        }
        if (options.serveOpenOrders) json.string("/participants/{id}/open_orders");
        json.endArray();
        json.endObject();
        return ok(json);
    }

    Response status(const Request& request) {
        Params params(request, {});
        JsonWriter json;
        json.beginObject().key("ok").boolean(anyReadable());
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    Response listBatches(const Request& request) {
        const Params params(request, { "limit", "before" });
        const std::size_t limit = params.limit();
        const std::optional<std::uint64_t> before = params.number("before");
        if (!anyReadable()) throw ApiError{ 503, "no recorder file can be read" };

        // Each recorder gives its largest ids below `before`, and the largest of all of them are among those.
        std::map<BatchId, std::vector<Held>, std::greater<>> merged;
        for (Source& source : sources) {
            if (!open(source)) continue;
            for (const BatchHeader& header : read(source, [&](ResultsStore& store) { return store.batches(before, limit + 1); })) {
                merged[header.batchId].push_back(Held{ &source, header });
            }
        }

        JsonWriter json;
        json.beginObject().key("batches").beginArray();
        std::size_t written = 0;
        std::optional<BatchId> last;
        for (const auto& [id, held] : merged) {
            if (written == limit) break;
            json.beginObject();
            json.key("batch");
            writeHeader(json, held.front().header);
            writeHeldBy(json, held);
            json.endObject();
            last = id;
            ++written;
        }
        json.endArray();
        json.key("next_before");
        if (merged.size() > limit && last) json.id(*last);
        else json.null();
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    Response oneBatch(const std::string& idText, const Request& request) {
        const Params params(request, { "wait_ms", "verify" });
        const BatchId id = parseId(idText, "the batch id");
        const bool verify = params.flag("verify");
        const std::vector<Held> held = awaitHolders(id, params.waitMs());
        if (held.empty()) return notFound("batch " + std::to_string(id) + " is not recorded");

        JsonWriter json;
        json.beginObject();
        json.key("batch");
        writeHeader(json, held.front().header);
        writeHeldBy(json, held);
        json.key("digests").beginObject();
        for (const Held& h : held) json.key(h.source->name).string(hex(h.header.digest));
        json.endObject();
        if (verify) {
            json.key("intact").beginObject();
            for (const Held& h : held) {
                json.key(h.source->name).boolean(read(*h.source, [&](ResultsStore& store) { return store.intact(id); }));
            }
            json.endObject();
        }
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    // The start of an answer about the rows of a batch: which batch, from which recorder, and whether they agree.
    static void beginBatchAnswer(JsonWriter& json, BatchId id, const std::vector<Held>& held) {
        json.beginObject();
        json.key("batch_id").id(id);
        json.key("recorder").string(held.front().source->name);
        json.key("agree").boolean(agree(held));
    }

    static void writePage(JsonWriter& json, std::size_t total, std::size_t offset, std::size_t limit, std::size_t returned) {
        json.key("total").number(total);
        json.key("offset").number(offset);
        json.key("limit").number(limit);
        json.key("next_offset");
        if (offset + returned < total) json.number(offset + returned);
        else json.null();
    }

    Response batchOrders(const std::string& idText, const Request& request) {
        const Params params(request, { "limit", "offset", "wait_ms" });
        const BatchId id = parseId(idText, "the batch id");
        const std::size_t limit = params.limit();
        const std::size_t offset = params.offset();
        const std::vector<Held> held = awaitHolders(id, params.waitMs());
        if (held.empty()) return notFound("batch " + std::to_string(id) + " is not recorded");

        const BatchHeader& header = held.front().header;
        const std::vector<OrderRow> rows = read(*held.front().source, [&](ResultsStore& store) { return store.orders(id, offset, limit); });
        JsonWriter json;
        beginBatchAnswer(json, id, held);
        writePage(json, header.status == BatchStatus::CLEARED ? std::size_t{ header.bids } + header.asks : 0, offset, limit, rows.size());
        json.key("orders").beginArray();
        for (const OrderRow& row : rows) {
            json.beginObject();
            writeOrderFields(json, row, header.firstHour);
            json.endObject();
        }
        json.endArray();
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    Response batchFills(const std::string& idText, const Request& request) {
        const Params params(request, { "limit", "offset", "wait_ms" });
        const BatchId id = parseId(idText, "the batch id");
        const std::size_t limit = params.limit();
        const std::size_t offset = params.offset();
        const std::vector<Held> held = awaitHolders(id, params.waitMs());
        if (held.empty()) return notFound("batch " + std::to_string(id) + " is not recorded");

        const std::vector<FillRow> rows = read(*held.front().source, [&](ResultsStore& store) { return store.fills(id, offset, limit); });
        JsonWriter json;
        beginBatchAnswer(json, id, held);
        writePage(json, held.front().header.fills, offset, limit, rows.size());
        json.key("fills").beginArray();
        for (const FillRow& row : rows) writeFill(json, row);
        json.endArray();
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    Response batchPrices(const std::string& idText, const Request& request) {
        const Params params(request, { "wait_ms" });
        const BatchId id = parseId(idText, "the batch id");
        const std::vector<Held> held = awaitHolders(id, params.waitMs());
        if (held.empty()) return notFound("batch " + std::to_string(id) + " is not recorded");

        const std::vector<AcceleratorPriceRow> rows = read(*held.front().source, [&](ResultsStore& store) { return store.prices(id); });
        JsonWriter json;
        beginBatchAnswer(json, id, held);
        json.key("prices").beginArray();
        for (const AcceleratorPriceRow& row : rows) {
            json.beginObject();
            writePriceFields(json, row);
            json.endObject();
        }
        json.endArray();
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    // A model by its name or its number.
    static std::uint8_t parseAccelerator(const std::string& text) {
        std::uint64_t number = 0;
        if (parseUnsigned(text, number)) {
            if (number < catalog::ACCELERATOR_COUNT) return static_cast<std::uint8_t>(number);
        } else {
            for (std::size_t i = 0; i < catalog::ACCELERATOR_COUNT; ++i) {
                if (text == catalog::ACCELERATORS[i].name) return static_cast<std::uint8_t>(i);
            }
        }
        throw ApiError{ 400, std::string("accelerator must be the name of a model, such as ") + catalog::ACCELERATORS[0].name +
                                 ", or its number from 0 to " + std::to_string(catalog::ACCELERATOR_COUNT - 1) };
    }

    Response priceSeries(const Request& request) {
        const Params params(request, { "accelerator", "hour", "lead", "limit", "before" });
        const std::optional<std::string> modelText = params.text("accelerator");
        if (!modelText) throw ApiError{ 400, "accelerator is required: the name of a model, or its number" };
        const std::uint8_t model = parseAccelerator(*modelText);
        const std::optional<std::uint64_t> hour = params.number("hour", std::numeric_limits<HourIndex>::max());
        const std::optional<std::uint64_t> lead = params.number("lead", limits::SLOT_WINDOW - 1);
        if (hour.has_value() == lead.has_value()) throw ApiError{ 400, "give exactly one of hour and lead" };
        const std::size_t limit = params.limit();
        const std::optional<std::uint64_t> before = params.number("before");
        if (!anyReadable()) throw ApiError{ 503, "no recorder file can be read" };

        struct Seen {
            Source* source;
            AcceleratorPriceRow row;
        };
        std::map<BatchId, std::vector<Seen>, std::greater<>> merged;
        for (Source& source : sources) {
            if (!open(source)) continue;
            const std::vector<ResultsStore::PricePoint> points = read(source, [&](ResultsStore& store) {
                return hour ? store.priceSeries(model, static_cast<HourIndex>(*hour), before, limit + 1)
                            : store.priceSeriesAtLead(model, static_cast<std::uint32_t>(*lead), before, limit + 1);
            });
            for (const ResultsStore::PricePoint& point : points) merged[point.batchId].push_back(Seen{ &source, point.row });
        }

        JsonWriter json;
        json.beginObject();
        json.key("accelerator").number(model);
        json.key("name").string(acceleratorName(model));
        if (hour) json.key("hour").number(*hour);
        else json.key("lead").number(*lead);
        json.key("prices").beginArray();
        std::size_t written = 0;
        std::optional<BatchId> last;
        for (const auto& [id, seen] : merged) {
            if (written == limit) break;
            json.beginObject();
            json.key("batch_id").id(id);
            json.key("held_by").beginArray();
            for (const Seen& s : seen) json.string(s.source->name);
            json.endArray();
            json.key("agree").boolean(std::all_of(seen.begin(), seen.end(), [&](const Seen& s) { return s.row == seen.front().row; }));
            writePriceFields(json, seen.front().row);
            json.endObject();
            last = id;
            ++written;
        }
        json.endArray();
        json.key("next_before");
        if (merged.size() > limit && last) json.id(*last);
        else json.null();
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    Response oneOrder(const std::string& idText, const std::string& sideText, const std::string& rankText, const Request& request) {
        const Params params(request, { "wait_ms" });
        const BatchId id = parseId(idText, "the batch id");
        if (sideText != "bid" && sideText != "ask") throw ApiError{ 400, "the side must be bid or ask, not " + printable(sideText) };
        const RowSide side = sideText == "bid" ? RowSide::BID : RowSide::ASK;
        const std::uint64_t rank = parseId(rankText, "the rank");
        if (rank > std::numeric_limits<std::uint32_t>::max()) throw ApiError{ 400, "the rank is over 4294967295" };
        const std::vector<Held> held = awaitHolders(id, params.waitMs());
        if (held.empty()) return notFound("batch " + std::to_string(id) + " is not recorded");

        const auto order = read(*held.front().source, [&](ResultsStore& store) { return store.order(id, side, static_cast<std::uint32_t>(rank)); });
        if (!order) return notFound("batch " + std::to_string(id) + " has no " + sideText + " with rank " + std::to_string(rank));
        const std::vector<FillRow> fills = read(*held.front().source, [&](ResultsStore& store) {
            return store.fillsOf(id, side, static_cast<std::uint32_t>(rank));
        });

        JsonWriter json;
        beginBatchAnswer(json, id, held);
        json.key("order").beginObject();
        writeOrderFields(json, *order, held.front().header.firstHour);
        json.endObject();
        json.key("fills").beginArray();
        for (const FillRow& row : fills) writeFill(json, row);
        json.endArray();
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    using Key = std::tuple<BatchId, std::uint8_t, std::uint32_t>;

    static Key keyOf(const ResultsStore::Position& p) { return Key{ p.batchId, static_cast<std::uint8_t>(p.side), p.rank }; }

    static std::string cursorOf(const ResultsStore::Position& p) {
        return std::to_string(p.batchId) + "." + sideName(p.side) + "." + std::to_string(p.rank);
    }

    static ResultsStore::Position parseCursor(const std::string& text) {
        const std::size_t first = text.find('.');
        const std::size_t second = first == std::string::npos ? std::string::npos : text.find('.', first + 1);
        std::uint64_t batch = 0;
        std::uint64_t rank = 0;
        if (second == std::string::npos || !parseUnsigned(std::string_view(text).substr(0, first), batch) ||
            !parseUnsigned(std::string_view(text).substr(second + 1), rank) || rank > std::numeric_limits<std::uint32_t>::max()) {
            throw ApiError{ 400, "cursor must be as the one a page gives: batch.side.rank" };
        }
        const std::string side = text.substr(first + 1, second - first - 1);
        if (side != "bid" && side != "ask") throw ApiError{ 400, "cursor must be as the one a page gives: batch.side.rank" };
        return ResultsStore::Position{ batch, side == "bid" ? RowSide::BID : RowSide::ASK, static_cast<std::uint32_t>(rank) };
    }

    static std::uint32_t parseParticipant(const std::string& text) {
        const std::uint64_t participant = parseId(text, "the participant id");
        if (participant > std::numeric_limits<std::uint32_t>::max()) throw ApiError{ 400, "the participant id is over 4294967295" };
        return static_cast<std::uint32_t>(participant);
    }

    Response participantOrders(const std::string& idText, const Request& request) {
        const Params params(request, { "limit", "cursor", "token" });
        const std::uint32_t participant = parseParticipant(idText);
        const std::size_t limit = params.limit();
        std::optional<ResultsStore::Position> before;
        if (const std::optional<std::string> cursor = params.text("cursor")) before = parseCursor(*cursor);
        const std::optional<std::uint64_t> token = params.number("token");
        if (!anyReadable()) throw ApiError{ 503, "no recorder file can be read" };

        std::map<Key, ResultsStore::ParticipantOrder, std::greater<>> merged;
        for (Source& source : sources) {
            if (!open(source)) continue;
            for (const ResultsStore::ParticipantOrder& row : read(source, [&](ResultsStore& store) {
                     return store.ordersOf(participant, before, token, limit + 1);
                 })) {
                merged.emplace(keyOf(row.position), row);
            }
        }

        JsonWriter json;
        json.beginObject().key("participant_id").number(participant).key("orders").beginArray();
        std::size_t written = 0;
        std::optional<ResultsStore::Position> last;
        for (const auto& [key, row] : merged) {
            if (written == limit) break;
            json.beginObject().key("batch_id").id(row.position.batchId);
            json.key("first_hour").number(row.firstHour);
            writeOrderFields(json, row.row, row.firstHour);
            json.endObject();
            last = row.position;
            ++written;
        }
        json.endArray();
        json.key("next_cursor");
        if (merged.size() > limit && last) json.string(cursorOf(*last));
        else json.null();
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    // What the freshest recorder says about the open batch: the one that has applied the most events.
    Response openOrders(const std::string& idText, const Request& request) {
        const Params params(request, {});
        if (!options.serveOpenOrders) {
            throw ApiError{ 403, "open orders are not served: they are bids and asks that have not cleared, and this API has no "
                                 "authentication. Start it with --open-orders to serve them to anyone who can reach it" };
        }
        const std::uint32_t participant = parseParticipant(idText);
        if (!anyReadable()) throw ApiError{ 503, "no recorder file can be read" };

        const Source* freshest = nullptr;
        std::optional<ResultsStore::OpenOrders> best;
        for (Source& source : sources) {
            if (!open(source)) continue;
            std::optional<ResultsStore::OpenOrders> mine = read(source, [&](ResultsStore& store) { return store.openOrdersOf(participant); });
            if (mine && (!best || mine->state.eventSeq > best->state.eventSeq)) {
                best = std::move(mine);
                freshest = &source;
            }
        }

        JsonWriter json;
        json.beginObject();
        json.key("participant_id").number(participant);
        json.key("open_batch");
        if (best) {
            json.beginObject();
            json.key("batch_id").id(best->state.batchId);
            json.key("first_hour").number(best->state.firstHour);
            writeStarts(json, "first_hour_starts", best->state.firstHour);
            json.key("event_seq").number(best->state.eventSeq);
            json.key("recorder").string(freshest->name);
            json.endObject();
        } else {
            json.null();
        }
        json.key("orders").beginArray();
        if (best) {
            for (const OpenOrderRow& row : best->orders) writeOpenOrder(json, row, best->state.firstHour);
        }
        json.endArray();
        writeWatermarks(json);
        json.endObject();
        return ok(json);
    }

    Response route(const Request& request) {
        const std::vector<std::string>& s = request.segments;
        if (s.empty()) return root();
        if (s[0] == "status" && s.size() == 1) return status(request);
        if (s[0] == "batches") {
            if (s.size() == 1) return listBatches(request);
            if (s.size() == 2) return oneBatch(s[1], request);
            if (s.size() == 3 && s[2] == "orders") return batchOrders(s[1], request);
            if (s.size() == 3 && s[2] == "fills") return batchFills(s[1], request);
            if (s.size() == 3 && s[2] == "prices") return batchPrices(s[1], request);
            if (s.size() == 5 && s[2] == "orders") return oneOrder(s[1], s[3], s[4], request);
        }
        if (s[0] == "prices" && s.size() == 1) return priceSeries(request);
        if (s[0] == "participants" && s.size() == 3 && s[2] == "orders") return participantOrders(s[1], request);
        if (s[0] == "participants" && s.size() == 3 && s[2] == "open_orders") return openOrders(s[1], request);
        throw ApiError{ 404, "there is nothing at this path" };
    }
};

ResultsApi::ResultsApi(std::vector<RecorderFile> files, ApiOptions options) : impl_(std::make_unique<Impl>()) {
    for (RecorderFile& file : files) impl_->sources.push_back(Impl::Source{ std::move(file.name), std::move(file.path), nullptr });
    impl_->options = options;
}

ResultsApi::~ResultsApi() = default;

Response ResultsApi::handle(const Request& request) {
    try {
        if (request.method != "GET") {
            Response response = errorResponse(405, "only GET is served");
            response.allow = "GET";
            return response;
        }
        return impl_->route(request);
    } catch (const ApiError& e) {
        return errorResponse(e.status, e.message);
    }
}

} // namespace api
