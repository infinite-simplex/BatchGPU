#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "BatchClearing.hpp"
#include "ClearingInvariants.hpp"
#include "EngineFixtures.hpp"

using namespace enginefix;

// A model of the engine, written to be obviously right instead of fast, and a random stream of commands to
// hold the engine to it. The model shares nothing with the book except the stateless checks (which have
// their own differential tests) and the clear itself (compared with an independent solver below). It scans
// lists where the book keeps counts, works out the hours in 128 bits where the schedule saturates, expands
// an order's box by trying every item of the window, and decides a lifted unit by comparing SKUs one pair
// at a time.

namespace {

using u128 = unsigned __int128;

constexpr std::uint64_t MAX32 = std::numeric_limits<HourIndex>::max();

struct ModelOrder {
    Side side;
    ParticipantId participant;
    OrderToken token;
    SessionId session;
    std::vector<u128> hours; // the hours of the order
    std::vector<catalog::Sku> skus; // the SKUs of its box
    NodeUnits perSlotCap;
    std::vector<CurveStep> steps;
    bool live{ true };
    std::uint32_t rank{ 0 };

    std::uint64_t arcs() const { return hours.size() * skus.size() + steps.size(); }
    NodeUnits total() const {
        NodeUnits sum = 0;
        for (const CurveStep& s : steps) sum += s.quantity;
        return sum;
    }
};

// The clear the engine was given, recorded by the clear function it runs.
ClearingInput g_input;
ClearingResult capturingClear(const ClearingInput& input, const ClearingOptions& options) {
    g_input = input;
    return clear(input, options);
}

bool sameInput(const ClearingInput& a, const ClearingInput& b) {
    auto sameOrders = [](const std::vector<ClearingOrder>& x, const std::vector<ClearingOrder>& y) {
        if (x.size() != y.size()) return false;
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (x[i].id != y[i].id || x[i].perSlotCap != y[i].perSlotCap || x[i].stepCount != y[i].stepCount ||
                x[i].items != y[i].items) {
                return false;
            }
            for (std::size_t k = 0; k < x[i].stepCount; ++k) {
                if (x[i].steps[k].quantity != y[i].steps[k].quantity || x[i].steps[k].price != y[i].steps[k].price) return false;
            }
        }
        return true;
    };
    return a.itemCount == b.itemCount && sameOrders(a.bids, b.bids) && sameOrders(a.asks, b.asks) &&
           a.bidRank == b.bidRank && a.askRank == b.askRank;
}

class Model {
public:
    explicit Model(const EngineConfig& config) : config_(config), batchId_(config.firstBatchId) { openBatch(); }

    std::vector<BatchEvent> apply(const BatchCommand& c) {
        std::vector<BatchEvent> out;
        switch (c.type) {
        case CommandType::INVALID:
            refuse(out, c, { c.invalidReason, c.invalidDetail });
            completedIds.push_back(c.trace_id);
            break;
        case CommandType::ORDER: onOrder(out, c); break;
        case CommandType::CANCEL: onCancel(out, c); break;
        case CommandType::BATCH_CLEAR: onClear(out, c); break;
        }
        return out;
    }

    BatchId batchId() const { return batchId_; }
    u128 firstHour() const { return firstHour_; }
    // The ids of the commands whose traces are complete, in the order they complete: each command's own,
    // while it is applied.
    std::vector<std::uint64_t> completedIds;
    ClearingInput lastInput;
    bool lastWasVoid{ false };
    std::size_t clears{ 0 };
    std::size_t voids{ 0 };
    std::size_t tradedClears{ 0 };

    std::size_t open(Side side) const {
        std::size_t n = 0;
        for (const ModelOrder& o : orders(side)) n += o.live ? 1 : 0;
        return n;
    }
    std::size_t submissions(Side side) const { return orders(side).size(); }
    std::uint64_t arcs() const {
        std::uint64_t sum = 0;
        for (const Side side : { Side::BID, Side::ASK }) {
            for (const ModelOrder& o : orders(side)) sum += o.live ? o.arcs() : 0;
        }
        return sum;
    }
    std::vector<std::pair<ParticipantId, OrderToken>> liveOrders() const {
        std::vector<std::pair<ParticipantId, OrderToken>> out;
        for (const Side side : { Side::BID, Side::ASK }) {
            for (const ModelOrder& o : orders(side)) {
                if (o.live) out.emplace_back(o.participant, o.token);
            }
        }
        return out;
    }
    BuyerCounts buyer(ParticipantId p) const {
        BuyerCounts b;
        for (const ModelOrder& o : bids_) {
            if (o.participant != p) continue;
            ++b.submissions;
            if (o.live) {
                ++b.openBids;
                b.arcs += o.arcs();
            }
        }
        return b;
    }

private:
    const std::vector<ModelOrder>& orders(Side side) const { return side == Side::BID ? bids_ : asks_; }
    std::vector<ModelOrder>& orders(Side side) { return side == Side::BID ? bids_ : asks_; }

    void openBatch() {
        // The clear with this id is at first + id * period seconds, in 128 bits so that it cannot wrap;
        // it sells from the first hour mark after that.
        const u128 seconds = u128{ config_.schedule.firstClearSeconds } + u128{ batchId_ } * config_.schedule.periodSeconds;
        firstHour_ = seconds / 3600 + 1;
    }

    bool sellable(u128 hour) const { return hour >= firstHour_ && hour - firstHour_ < 47 && hour <= MAX32; }

    void push(std::vector<BatchEvent>& out, BatchEvent e) {
        e.sequence_number = nextSequence_++;
        out.push_back(e);
    }

    void refuse(std::vector<BatchEvent>& out, const BatchCommand& c, Reject why) {
        BatchEvent e{};
        e.type = EventType::REJECTED;
        e.batchId = batchId_;
        e.participantId = c.participantId;
        e.orderToken = c.orderToken;
        e.session_id = c.sessionId;
        e.side = c.side;
        e.rejectReason = why.reason;
        e.rejectDetail = why.detail;
        if (why.reason == RejectReason::SLOT_NOT_ELIGIBLE) {
            e.hour = static_cast<HourIndex>(std::min<u128>(u128{ c.baseHour } + why.detail, MAX32));
        }
        push(out, e);
    }

    static unsigned countBits(std::uint64_t mask) {
        unsigned n = 0;
        for (unsigned b = 0; b < 64; ++b) n += (mask >> b) & 1;
        return n;
    }

    Reject stateful(const BatchCommand& c) const {
        for (const Side side : { Side::BID, Side::ASK }) {
            for (const ModelOrder& o : orders(side)) {
                if (o.live && o.participant == c.participantId && o.token == c.orderToken) {
                    return { RejectReason::TOKEN_IN_USE, 0 };
                }
            }
        }
        for (unsigned bit = 0; bit < 64; ++bit) {
            if (((c.slotMask >> bit) & 1) == 0) continue;
            if (!sellable(u128{ c.baseHour } + bit)) return { RejectReason::SLOT_NOT_ELIGIBLE, static_cast<std::uint16_t>(bit) };
        }
        const std::uint64_t newArcs = std::uint64_t{ countBits(c.acceleratorMask) } * countBits(c.interconnectMask) *
                                          countBits(c.regionMask) * countBits(c.slotMask) +
                                      c.stepCount;
        if (c.side == Side::BID) {
            const BuyerCounts b = buyer(c.participantId);
            if (b.openBids >= config_.limits.maxOpenBidsPerBuyer) {
                return { RejectReason::PARTICIPANT_QUOTA, static_cast<std::uint16_t>(QuotaLimit::OPEN_ORDERS) };
            }
            if (b.submissions >= config_.limits.maxSubmissionsPerBuyer) {
                return { RejectReason::PARTICIPANT_QUOTA, static_cast<std::uint16_t>(QuotaLimit::SUBMISSIONS) };
            }
            if (b.arcs + newArcs > config_.limits.maxArcsPerBuyer) {
                return { RejectReason::PARTICIPANT_QUOTA, static_cast<std::uint16_t>(QuotaLimit::ARCS) };
            }
        }
        if (open(c.side) >= config_.limits.maxOrdersPerSide) {
            return { RejectReason::BATCH_FULL, static_cast<std::uint16_t>(FullLimit::ORDERS) };
        }
        if (submissions(c.side) >= config_.limits.maxSubmissionsPerSide) {
            return { RejectReason::BATCH_FULL, static_cast<std::uint16_t>(FullLimit::SUBMISSIONS) };
        }
        if (arcs() + newArcs > config_.limits.maxArcs) {
            return { RejectReason::BATCH_FULL, static_cast<std::uint16_t>(FullLimit::ARCS) };
        }
        return {};
    }

    void onOrder(std::vector<BatchEvent>& out, const BatchCommand& c) {
        Reject why = checkOrder(c);
        if (!why) why = stateful(c);
        if (why) {
            refuse(out, c, why);
            completedIds.push_back(c.trace_id);
            return;
        }
        ModelOrder o;
        o.side = c.side;
        o.participant = c.participantId;
        o.token = c.orderToken;
        o.session = c.sessionId;
        for (unsigned bit = 0; bit < 64; ++bit) {
            if ((c.slotMask >> bit) & 1) o.hours.push_back(u128{ c.baseHour } + bit);
        }
        for (std::size_t k = 0; k < catalog::SKU_COUNT; ++k) {
            const catalog::Sku s = catalog::skuFromIndex(k);
            if (((c.acceleratorMask >> s.accelerator) & 1) && ((c.interconnectMask >> s.interconnect) & 1) &&
                ((c.regionMask >> s.region) & 1)) {
                o.skus.push_back(s);
            }
        }
        o.perSlotCap = c.perSlotCap;
        for (std::size_t k = 0; k < c.stepCount; ++k) o.steps.push_back(c.steps[k]);
        o.rank = static_cast<std::uint32_t>(orders(c.side).size());
        orders(c.side).push_back(o);

        BatchEvent e{};
        e.type = EventType::ACCEPTED;
        e.batchId = batchId_;
        e.participantId = o.participant;
        e.orderToken = o.token;
        e.session_id = o.session;
        e.side = o.side;
        e.rank = o.rank;
        e.units = o.total();
        push(out, e);
        completedIds.push_back(c.trace_id);
    }

    void onCancel(std::vector<BatchEvent>& out, const BatchCommand& c) {
        for (const Side side : { Side::BID, Side::ASK }) {
            for (ModelOrder& o : orders(side)) {
                if (!o.live || o.participant != c.participantId || o.token != c.orderToken) continue;
                o.live = false;
                BatchEvent e{};
                e.type = EventType::CANCELED;
                e.batchId = batchId_;
                e.participantId = o.participant;
                e.orderToken = o.token;
                e.session_id = o.session;
                e.side = o.side;
                e.rank = o.rank;
                e.units = o.total();
                push(out, e);
                completedIds.push_back(c.trace_id);
                return;
            }
        }
        refuse(out, c, { RejectReason::UNKNOWN_ORDER, 0 });
        completedIds.push_back(c.trace_id);
    }

    // The item index of (hour, SKU) in this batch's window: the hour's position times 30, plus the SKU.
    static std::uint32_t item(std::size_t slot, std::size_t sku) { return static_cast<std::uint32_t>(slot * 30 + sku); }

    ClearingInput buildInput() const {
        ClearingInput input;
        input.itemCount = 1410;
        for (const Side side : { Side::BID, Side::ASK }) {
            for (const ModelOrder& o : orders(side)) {
                if (!o.live) continue;
                ClearingOrder c;
                c.id = o.rank;
                c.perSlotCap = o.perSlotCap;
                c.stepCount = static_cast<std::uint8_t>(o.steps.size());
                for (std::size_t k = 0; k < o.steps.size(); ++k) c.steps[k] = { o.steps[k].quantity, o.steps[k].price };
                // Try every item of the window and keep the ones in the box.
                for (std::size_t slot = 0; slot < 47; ++slot) {
                    if (std::find(o.hours.begin(), o.hours.end(), firstHour_ + slot) == o.hours.end()) continue;
                    for (std::size_t k = 0; k < catalog::SKU_COUNT; ++k) {
                        const catalog::Sku s = catalog::skuFromIndex(k);
                        if (std::find(o.skus.begin(), o.skus.end(), s) != o.skus.end()) c.items.push_back(item(slot, k));
                    }
                }
                (side == Side::BID ? input.bids : input.asks).push_back(std::move(c));
                (side == Side::BID ? input.bidRank : input.askRank).push_back(o.rank);
            }
        }
        return input;
    }

    // A unit awarded at `sku` is lifted for an order when the order accepts some SKU strictly below it:
    // the same region, no better a model of the same vendor, no better an interconnect, and not the same.
    static bool lifted(const ModelOrder& o, const catalog::Sku& sku) {
        if (o.side != Side::BID) return false;
        for (const catalog::Sku& other : o.skus) {
            if (other == sku) continue;
            const auto& a = catalog::ACCELERATORS[sku.accelerator];
            const auto& b = catalog::ACCELERATORS[other.accelerator];
            if (other.region == sku.region && other.interconnect <= sku.interconnect && a.vendor == b.vendor &&
                b.chainPosition <= a.chainPosition) {
                return true;
            }
        }
        return false;
    }

    void emitSide(std::vector<BatchEvent>& out, Side side, const ClearingInput& input, const ClearingResult& result,
                  std::uint64_t& fills, std::uint64_t& liftedTotal) {
        const std::vector<Fill>& all = side == Side::BID ? result.bidFills : result.askFills;
        for (const ClearingOrder& c : (side == Side::BID ? input.bids : input.asks)) {
            const ModelOrder& o = orders(side)[c.id];
            std::vector<Fill> mine;
            for (const Fill& f : all) {
                if (f.order == c.id) mine.push_back(f);
            }
            std::sort(mine.begin(), mine.end(), [](const Fill& x, const Fill& y) { return x.item < y.item; });
            std::uint64_t filled = 0;
            std::uint64_t liftedUnits = 0;
            std::int64_t amount = 0;
            for (const Fill& f : mine) {
                const catalog::Sku sku = catalog::skuFromIndex(f.item % 30);
                const bool isLifted = lifted(o, sku);
                BatchEvent e{};
                e.type = EventType::FILL;
                e.batchId = clearId_;
                e.participantId = o.participant;
                e.orderToken = o.token;
                e.session_id = o.session;
                e.side = side;
                e.rank = o.rank;
                e.accelerator = sku.accelerator;
                e.interconnect = sku.interconnect;
                e.region = sku.region;
                e.lifted = isLifted ? 1 : 0;
                e.hour = static_cast<HourIndex>(firstHour_ + f.item / 30);
                e.price = *result.itemPrice[f.item];
                e.units = f.units;
                push(out, e);
                ++fills;
                filled += f.units;
                if (isLifted) liftedUnits += f.units;
                amount += static_cast<std::int64_t>(f.units) * e.price;
            }
            BatchEvent d{};
            d.type = EventType::ORDER_DONE;
            d.batchId = clearId_;
            d.participantId = o.participant;
            d.orderToken = o.token;
            d.session_id = o.session;
            d.side = side;
            d.rank = o.rank;
            d.units = static_cast<NodeUnits>(filled);
            d.unfilledUnits = o.total() - static_cast<NodeUnits>(filled);
            d.liftedUnits = static_cast<NodeUnits>(liftedUnits);
            d.amount = amount;
            push(out, d);
            liftedTotal += liftedUnits;
        }
    }

    void onClear(std::vector<BatchEvent>& out, const BatchCommand& c) {
        clearId_ = c.batchId;
        const ClearingInput input = buildInput();
        lastInput = input;
        ++clears;

        std::optional<ClearingResult> result;
        VoidReason voided = VoidReason::NONE;
        std::uint64_t voidPivots = 0;
        std::uint64_t voidArcs = arcs();
        try {
            result = clear(input, config_.clearing);
        } catch (const ClearingBudgetExceeded& e) {
            voided = VoidReason::PIVOT_BUDGET;
            voidPivots = e.pivots();
            voidArcs = e.arcs();
        } catch (const std::invalid_argument&) {
            voided = VoidReason::REFUSED_INPUT;
        }
        lastWasVoid = voided != VoidReason::NONE;

        if (voided != VoidReason::NONE) {
            ++voids;
            BatchEvent e{};
            e.type = EventType::BATCH_VOIDED;
            e.batchId = clearId_;
            e.session_id = c.sessionId;
            e.hour = static_cast<HourIndex>(std::min<u128>(firstHour_, MAX32));
            e.voidReason = voided;
            e.bids = static_cast<std::uint32_t>(input.bids.size());
            e.asks = static_cast<std::uint32_t>(input.asks.size());
            e.arcs = static_cast<std::uint32_t>(voidArcs);
            e.pivots = static_cast<std::uint32_t>(voidPivots);
            push(out, e);
        } else {
            // What the clear says must be internally consistent, and agree with the independent solver on
            // everything that does not depend on which of several equal allocations it picked.
            invariants::expectValid(input, *result);
            if (input.bids.size() + input.asks.size() <= 40) {
                const ClearingResult reference = clearReference(input);
                EXPECT_EQ(result->welfare, reference.welfare);
                EXPECT_EQ(result->tradedUnits, reference.tradedUnits);
                EXPECT_EQ(result->tieCost, reference.tieCost);
            }
            if (result->tradedUnits > 0) ++tradedClears;

            std::uint64_t fills = 0;
            std::uint64_t liftedTotal = 0;
            emitSide(out, Side::BID, input, *result, fills, liftedTotal);
            emitSide(out, Side::ASK, input, *result, fills, liftedTotal);
            BatchEvent e{};
            e.type = EventType::BATCH_DONE;
            e.batchId = clearId_;
            e.session_id = c.sessionId;
            e.hour = static_cast<HourIndex>(std::min<u128>(firstHour_, MAX32));
            e.units = result->tradedUnits;
            e.liftedUnits = static_cast<NodeUnits>(liftedTotal);
            e.amount = result->welfare;
            e.rankSum = result->tieCost;
            e.fills = static_cast<std::uint32_t>(fills);
            e.certified = result->stats.certified ? 1 : 0;
            e.bids = static_cast<std::uint32_t>(input.bids.size());
            e.asks = static_cast<std::uint32_t>(input.asks.size());
            e.arcs = static_cast<std::uint32_t>(result->stats.arcs);
            e.pivots = result->stats.augmentations;
            push(out, e);
        }

        completedIds.push_back(c.trace_id);
        bids_.clear();
        asks_.clear();
        batchId_ = clearId_ == std::numeric_limits<BatchId>::max() ? clearId_ : clearId_ + 1;
        openBatch();
    }

    EngineConfig config_;
    BatchId batchId_;
    BatchId clearId_{ 0 };
    u128 firstHour_{ 0 };
    std::vector<ModelOrder> bids_;
    std::vector<ModelOrder> asks_;
    std::uint64_t nextSequence_{ 1 };
};

struct Mix {
    unsigned orderPercent{ 76 };
    unsigned cancelPercent{ 20 };
    // the rest are clears: 4 percent, so a batch is about 25 commands
    unsigned wideHoursPercent{ 4 };
    unsigned badOrderPercent{ 8 };
    unsigned extremeHourPercent{ 2 };
};

class Generator {
public:
    Generator(std::uint64_t seed, const Model& model, Mix mix) : rng_(seed), model_(model), mix_(mix) {}

    BatchCommand next() {
        const unsigned roll = pick(100);
        BatchCommand c{};
        if (roll < mix_.orderPercent) {
            c = order();
        } else if (roll < mix_.orderPercent + mix_.cancelPercent) {
            c.type = CommandType::CANCEL;
            // Mostly an order that is live, now and then one that is not.
            const auto live = model_.liveOrders();
            if (!live.empty() && chance(75)) {
                const auto& pair = live[pick(static_cast<unsigned>(live.size()))];
                c.participantId = pair.first;
                c.orderToken = pair.second;
            } else {
                c.participantId = 1 + pick(7);
                c.orderToken = 1 + pick(25);
            }
        } else {
            c.type = CommandType::BATCH_CLEAR;
            c.batchId = clearId();
        }
        c.sessionId = 1 + pick(5);
        return c;
    }

private:
    unsigned pick(unsigned n) { return static_cast<unsigned>(rng_() % n); }
    bool chance(unsigned percent) { return pick(100) < percent; }

    BatchId clearId() {
        const unsigned roll = pick(100);
        const BatchId expected = model_.batchId();
        if (roll < 75) return expected;
        if (roll < 84) return expected == 0 ? 0 : expected - 1; // the same again
        if (roll < 92) return pick(20);                          // a rewind or a skip
        if (roll < 98) return expected + 1 + pick(5);
        if (roll < 99) return std::numeric_limits<BatchId>::max();
        return std::numeric_limits<BatchId>::max() - pick(3);
    }

    BatchCommand order() {
        const Side side = chance(50) ? Side::BID : Side::ASK;
        Order o;
        o.side = side;
        o.participant = 1 + pick(6);
        o.token = 1 + pick(24);

        // The hours: near the start of the window most of the time, sometimes at its far end, and a few
        // that are past or too far.
        const std::uint64_t first = static_cast<std::uint64_t>(std::min<u128>(model_.firstHour(), MAX32 - 200));
        std::int64_t delta = 0;
        const unsigned where = pick(100);
        if (where < 6) {
            delta = -static_cast<std::int64_t>(1 + pick(3)); // already past
        } else if (where < 16) {
            delta = 40 + pick(12); // the far end: 40 to 51, so the last few are too far
        } else {
            delta = pick(8); // the first eight hours
        }
        std::uint64_t base = static_cast<std::uint64_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(first) + delta));
        if (chance(mix_.extremeHourPercent)) base = chance(50) ? 0 : MAX32;
        o.baseHour = static_cast<HourIndex>(base);

        unsigned width = 1 + pick(6);
        if (chance(mix_.wideHoursPercent)) width = 20 + pick(28);
        std::uint64_t slots = 0;
        while (slots == 0) {
            for (unsigned b = 0; b < width; ++b) {
                if (chance(55)) slots |= std::uint64_t{ 1 } << b;
            }
        }
        if (chance(2)) slots |= std::uint64_t{ 1 } << (47 + pick(17)); // a bit beyond the window: a stateless reject
        o.slots = slots;

        auto subset = [&](unsigned bits) {
            std::uint32_t mask = 0;
            while (mask == 0) {
                for (unsigned b = 0; b < bits; ++b) {
                    if (chance(50)) mask |= 1u << b;
                }
            }
            return mask;
        };
        auto single = [&](unsigned bits) { return 1u << pick(bits); };
        const unsigned models = chance(10) ? 5 : 4; // the TPU is rare
        if (side == Side::BID) {
            o.accel = subset(models);
            o.inter = static_cast<std::uint8_t>(subset(2));
            o.region = subset(chance(25) ? 3 : 2);
        } else {
            o.accel = single(models);
            o.inter = static_cast<std::uint8_t>(single(2));
            o.region = single(chance(25) ? 3 : 2);
        }
        o.cap = 1 + pick(4);

        const unsigned steps = 1 + pick(4);
        std::vector<PriceTicks> prices;
        for (unsigned k = 0; k < steps; ++k) prices.push_back(20 + pick(400));
        std::sort(prices.begin(), prices.end());
        if (side == Side::BID) std::reverse(prices.begin(), prices.end());
        o.steps.clear();
        for (unsigned k = 0; k < steps; ++k) o.steps.push_back({ 1 + pick(5), prices[k] });
        BatchCommand c = o.command();

        // Occasionally break a stateless rule, to see it refused with the handler's reason.
        if (chance(mix_.badOrderPercent)) {
            switch (pick(8)) {
            case 0: c.acceleratorMask = 0; break;
            case 1: c.regionMask |= 1u << (3 + pick(3)); break;
            case 2: c.perSlotCap = chance(50) ? 0 : limits::MAX_PER_SLOT_CAP + 1; break;
            case 3: c.stepCount = chance(50) ? 0 : 5; break;
            case 4: c.steps[0].quantity = 0; break;
            case 5: c.steps[0].price = limits::MAX_PRICE_TICKS + 1; break;
            case 6: c.side = static_cast<Side>('?'); break;
            default:
                if (c.stepCount >= 2) std::swap(c.steps[0].price, c.steps[1].price);
                break;
            }
        }
        // Send a failing order as the handler would, as an INVALID command, most of the time, and as a raw
        // ORDER now and then, to see the engine refuse it on its own.
        const Reject why = checkOrder(c);
        if (why && chance(70)) {
            c.type = CommandType::INVALID;
            c.invalidReason = why.reason;
            c.invalidDetail = why.detail;
        }
        return c;
    }

    std::mt19937_64 rng_;
    const Model& model_;
    Mix mix_;
};

struct Totals {
    std::size_t commands{ 0 };
    std::size_t clears{ 0 };
    std::size_t voids{ 0 };
    std::size_t tradedClears{ 0 };
    std::map<RejectReason, std::size_t> rejects;
    std::map<std::pair<RejectReason, std::uint16_t>, std::size_t> detailed; // by reason and detail
    std::size_t fills{ 0 };
    std::size_t lifted{ 0 };
    std::size_t bidsFilled{ 0 };
    std::size_t bidsUnfilled{ 0 };

    std::size_t count(RejectReason reason, std::uint16_t detail) const {
        const auto found = detailed.find(std::make_pair(reason, detail));
        return found == detailed.end() ? 0 : found->second;
    }
};

void runStream(std::uint64_t seed, std::size_t commands, EngineConfig config, Mix mix, Totals& totals) {
    config.clearFunction = &capturingClear;
    Harness h(config);
    Model model(config);
    Generator gen(seed, model, mix);

    for (std::size_t i = 0; i < commands; ++i) {
        BatchCommand cmd = gen.next();
        // The harness stamps the trace id and the session; the model sees the same command.
        cmd.trace_id = static_cast<TraceId>(i + 1);
        const std::size_t clearsBefore = model.clears;
        const std::vector<BatchEvent> expected = model.apply(cmd);
        const std::vector<BatchEvent> actual = h.send(cmd, cmd.sessionId);
        const std::string where = "seed " + std::to_string(seed) + ", command " + std::to_string(i) + " (type " +
                                  std::to_string(static_cast<int>(cmd.type)) + ", side " + std::to_string(static_cast<int>(cmd.side)) + ")";

        ASSERT_TRUE(sameEvents(expected, actual)) << where;
        ASSERT_EQ(h.completedTraceIds(), model.completedIds) << where << ": the traces that completed";
        {
            const OrderTrace& done = h.traces.back();
            ASSERT_NE(done.match_done_tai_ns, 0u) << where << ": the engine stamps the end of its own call";
            ASSERT_GE(done.match_done_tai_ns, done.engine_pop_tai_ns) << where;
            if (cmd.type != CommandType::BATCH_CLEAR) {
                for (const std::int32_t value : done.ext_counters) ASSERT_EQ(value, 0) << where << ": only a clear carries counters";
            }
        }

        const BatchBook& book = h.engine.book();
        ASSERT_EQ(book.batchId(), model.batchId()) << where;
        const u128 modelHour = model.firstHour();
        if (modelHour <= MAX32 + 1000) {
            ASSERT_EQ(book.firstHour(), static_cast<std::uint64_t>(modelHour)) << where;
        } else {
            ASSERT_GT(book.firstHour(), std::uint64_t{ MAX32 } + 64) << where << ": a window that no order can reach";
        }
        for (const Side side : { Side::BID, Side::ASK }) {
            ASSERT_EQ(book.openOrders(side), model.open(side)) << where;
            ASSERT_EQ(book.submissions(side), model.submissions(side)) << where;
        }
        ASSERT_EQ(book.arcs(), model.arcs()) << where;
        for (ParticipantId p = 1; p <= 7; ++p) {
            const BuyerCounts a = book.buyer(p);
            const BuyerCounts b = model.buyer(p);
            ASSERT_EQ(a.openBids, b.openBids) << where << ", buyer " << p;
            ASSERT_EQ(a.submissions, b.submissions) << where << ", buyer " << p;
            ASSERT_EQ(a.arcs, b.arcs) << where << ", buyer " << p;
        }

        if (model.clears != clearsBefore) {
            // The clear got exactly the input that the model built by trying every item.
            ASSERT_TRUE(sameInput(model.lastInput, g_input)) << where << ": the input to the clear";
            // Every fill on one side adds up to the other, and the exchange pays out what it takes in.
            const auto counter = [&](GpuExtSlot slot) { return h.traces.back().ext_counters[static_cast<std::size_t>(slot)]; };
            if (!model.lastWasVoid) {
                std::uint64_t bidUnits = 0;
                std::uint64_t askUnits = 0;
                std::int64_t paid = 0;
                std::int64_t received = 0;
                std::int32_t bidsFilled = 0;
                std::int32_t bidsUnfilled = 0;
                for (const BatchEvent& e : actual) {
                    if (e.type == EventType::FILL) {
                        (e.side == Side::BID ? bidUnits : askUnits) += e.units;
                        ++totals.fills;
                        totals.lifted += e.lifted;
                    }
                    if (e.type == EventType::ORDER_DONE) {
                        (e.side == Side::BID ? paid : received) += e.amount;
                        if (e.side == Side::BID) (e.units > 0 ? bidsFilled : bidsUnfilled) += 1;
                    }
                }
                ASSERT_EQ(bidUnits, askUnits) << where;
                ASSERT_EQ(paid, received) << where;
                ASSERT_EQ(actual.back().type, EventType::BATCH_DONE) << where;
                ASSERT_EQ(bidUnits, actual.back().units) << where;
                // The batch statistics on the clear's trace: all four, from the events.
                ASSERT_EQ(counter(GpuExtSlot::LiftedUnits), static_cast<std::int32_t>(actual.back().liftedUnits)) << where;
                ASSERT_EQ(counter(GpuExtSlot::BidsFilled), bidsFilled) << where;
                ASSERT_EQ(counter(GpuExtSlot::BidsUnfilled), bidsUnfilled) << where;
                ASSERT_EQ(counter(GpuExtSlot::SolverPivots), static_cast<std::int32_t>(actual.back().pivots)) << where;
                totals.bidsFilled += static_cast<std::size_t>(bidsFilled);
                totals.bidsUnfilled += static_cast<std::size_t>(bidsUnfilled);
            } else {
                // A voided batch writes zeros, except for the pivots it stopped at.
                ASSERT_EQ(actual.back().type, EventType::BATCH_VOIDED) << where;
                ASSERT_EQ(counter(GpuExtSlot::LiftedUnits), 0) << where;
                ASSERT_EQ(counter(GpuExtSlot::BidsFilled), 0) << where;
                ASSERT_EQ(counter(GpuExtSlot::BidsUnfilled), 0) << where;
                ASSERT_EQ(counter(GpuExtSlot::SolverPivots), static_cast<std::int32_t>(actual.back().pivots)) << where;
            }
        }
        for (const BatchEvent& e : actual) {
            if (e.type == EventType::REJECTED) {
                ++totals.rejects[e.rejectReason];
                ++totals.detailed[std::make_pair(e.rejectReason, e.rejectDetail)];
            }
        }
        ++totals.commands;
    }
    totals.clears += model.clears;
    totals.voids += model.voids;
    totals.tradedClears += model.tradedClears;
}

EngineConfig smallLimits() {
    EngineConfig config = testConfig();
    config.limits.maxOrdersPerSide = 10;
    config.limits.maxSubmissionsPerSide = 18;
    config.limits.maxArcs = 110;
    config.limits.maxOpenBidsPerBuyer = 3;
    config.limits.maxSubmissionsPerBuyer = 5;
    config.limits.maxArcsPerBuyer = 36;
    return config;
}

// Limits that most orders fit under, and a busy stream still meets now and then.
EngineConfig middlingLimits() {
    EngineConfig config = testConfig();
    config.limits.maxOrdersPerSide = 14;
    config.limits.maxSubmissionsPerSide = 40;
    config.limits.maxArcs = 400;
    config.limits.maxOpenBidsPerBuyer = 5;
    config.limits.maxSubmissionsPerBuyer = 12;
    config.limits.maxArcsPerBuyer = 120;
    return config;
}

} // namespace

TEST(EngineModel, AgreesOnRandomCommandsUnderTheSettledLimitsAndAcrossFirstIdsAndSchedules) {
    Totals totals;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        runStream(seed, 400, testConfig(), Mix{}, totals);
        if (::testing::Test::HasFatalFailure()) return;
    }
    // The streams reach what they are meant to: enough of every kind of command and answer.
    EXPECT_EQ(totals.commands, 60u * 400u);
    EXPECT_GT(totals.clears, 400u);
    EXPECT_GT(totals.tradedClears, 150u) << "most clears must trade, or the fills are not exercised";
    EXPECT_GT(totals.fills, 1000u);
    EXPECT_GT(totals.lifted, 100u) << "lifted units must occur";
    EXPECT_GT(totals.bidsFilled, 200u);
    EXPECT_GT(totals.bidsUnfilled, 200u) << "and bids that get nothing, or the two counters could not be told apart";
    EXPECT_NE(totals.bidsFilled, totals.bidsUnfilled);
    EXPECT_GT(totals.rejects[RejectReason::TOKEN_IN_USE], 50u);
    EXPECT_GT(totals.rejects[RejectReason::SLOT_NOT_ELIGIBLE], 200u);
    EXPECT_GT(totals.rejects[RejectReason::UNKNOWN_ORDER], 100u);
    EXPECT_GT(totals.rejects[RejectReason::EMPTY_MASK], 20u);
    EXPECT_GT(totals.rejects[RejectReason::STEP_PRICE_ORDER], 5u);

    // The batches start at other ids and the schedules differ.
    Totals others;
    for (std::uint64_t seed = 301; seed <= 330; ++seed) {
        EngineConfig config = testConfig();
        config.firstBatchId = seed % 11;
        config.schedule.firstClearSeconds = 1000 * 3600 + 1800 + (seed % 7) * 977;
        runStream(seed, 300, config, Mix{}, others);
        if (::testing::Test::HasFatalFailure()) return;
    }
    EXPECT_GT(others.tradedClears, 60u);
}

TEST(EngineModel, AgreesWhenEveryLimitIsSmallEnoughToMeetAndWhenMostOrdersAreLetIn) {
    Totals totals;
    Mix mix;
    mix.orderPercent = 78;
    mix.cancelPercent = 18;
    for (std::uint64_t seed = 101; seed <= 160; ++seed) {
        runStream(seed, 400, smallLimits(), mix, totals);
        if (::testing::Test::HasFatalFailure()) return;
    }
    EXPECT_GT(totals.tradedClears, 100u);
    EXPECT_GT(totals.rejects[RejectReason::BATCH_FULL], 100u);
    EXPECT_GT(totals.rejects[RejectReason::PARTICIPANT_QUOTA], 100u);
    EXPECT_GT(totals.rejects[RejectReason::TOKEN_IN_USE], 50u);
    // Every limit is reached, each of them as the reason that is named.
    for (const FullLimit limit : { FullLimit::ORDERS, FullLimit::SUBMISSIONS, FullLimit::ARCS }) {
        EXPECT_GT(totals.count(RejectReason::BATCH_FULL, static_cast<std::uint16_t>(limit)), 10u)
            << "BATCH_FULL " << static_cast<int>(limit);
    }
    for (const QuotaLimit limit : { QuotaLimit::OPEN_ORDERS, QuotaLimit::SUBMISSIONS, QuotaLimit::ARCS }) {
        EXPECT_GT(totals.count(RejectReason::PARTICIPANT_QUOTA, static_cast<std::uint16_t>(limit)), 10u)
            << "PARTICIPANT_QUOTA " << static_cast<int>(limit);
    }

    Totals middling;
    Mix mostlyLetIn;
    mostlyLetIn.orderPercent = 70;
    mostlyLetIn.cancelPercent = 26;
    mostlyLetIn.badOrderPercent = 3;
    for (std::uint64_t seed = 401; seed <= 440; ++seed) {
        runStream(seed, 400, middlingLimits(), mostlyLetIn, middling);
        if (::testing::Test::HasFatalFailure()) return;
    }
    EXPECT_GT(middling.tradedClears, 100u);
    EXPECT_GT(middling.rejects[RejectReason::BATCH_FULL] + middling.rejects[RejectReason::PARTICIPANT_QUOTA], 50u);
    EXPECT_GT(middling.fills, 1000u);
}

TEST(EngineModel, AgreesWhenTheClearRunsOutOfPivots) {
    Totals totals;
    EngineConfig config = testConfig();
    config.clearing.maxPivots = 3;
    for (std::uint64_t seed = 201; seed <= 240; ++seed) {
        runStream(seed, 300, config, Mix{}, totals);
        if (::testing::Test::HasFatalFailure()) return;
    }
    EXPECT_GT(totals.voids, 30u) << "a budget of 3 pivots voids batches of any size";
    EXPECT_GT(totals.clears, totals.voids) << "and clears the empty and trivial ones";
}
