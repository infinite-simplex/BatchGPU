#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "BatchBook.hpp"
#include "BatchClearing.hpp"
#include "BatchCommand.hpp"
#include "BatchEvent.hpp"
#include "Catalog.hpp"
#include "ClearingOptions.hpp"
#include "GpuExtSlot.hpp"
#include "Schedule.hpp"
#include "Telemetry.hpp" // Core: complete_trace, the trace arena

// The TEngine that Core's MatchingService hands every command to. It dispatches on the command type,
// keeps the open batch in a BatchBook, runs the clear on BATCH_CLEAR, and emits every result as an
// event through the policy.
//
// Every event depends on the commands alone, in log order. There is no randomness and no iteration over
// an unordered container, so every replica that applies the same commands emits the same events with the
// same sequence numbers. The only clock it reads is the telemetry stamp in finishTrace.

// A test may replace the clear, to look at the input the engine builds or to make a clear fail.
using ClearFunction = ClearingResult (*)(const ClearingInput&, const ClearingOptions&);

inline ClearingResult productionClear(const ClearingInput& input, const ClearingOptions& options) {
    return clear(input, options);
}

// Every replica must run the same configuration. The defaults are what an exchange runs.
struct EngineConfig {
    schedule::Schedule schedule;
    BookLimits limits;
    BatchId firstBatchId{ 0 };
    ClearingOptions clearing;
    ClearFunction clearFunction{ &productionClear };
};

template <typename TelemetryPolicy>
class BatchEngine {
public:
    explicit BatchEngine(TelemetryPolicy& policy, const EngineConfig& config = {})
        : policy_(policy), config_(config), book_(config.schedule, config.limits, config.firstBatchId) {
        if (config_.clearFunction == nullptr) throw std::invalid_argument("BatchEngine: no clear function");
    }

    // One call per command, in log order. A command that the handler never produces is a bug, and stops
    // the replica instead of letting it go on with a batch it can no longer trust.
    void submit_order(BatchCommand& cmd) {
        switch (cmd.type) {
        case CommandType::INVALID:
            // Refused through the normal pipeline with the handler's reason, without touching the batch.
            reject(cmd, Reject{ cmd.invalidReason, cmd.invalidDetail });
            finishTrace(cmd.trace_id);
            return;
        case CommandType::ORDER:
            onOrder(cmd);
            return;
        case CommandType::CANCEL:
            onCancel(cmd);
            return;
        case CommandType::BATCH_CLEAR:
            onBatchClear(cmd);
            return;
        }
        throw std::logic_error("BatchEngine: a command of a type that the handler never produces");
    }

    const BatchBook& book() const noexcept { return book_; }

private:
    void emit(BatchEvent& event) {
        event.sequence_number = nextSequenceNumber_++;
        policy_.on_order_event(event);
    }

    static std::int32_t clampToInt32(std::uint64_t value) {
        return static_cast<std::int32_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::int32_t>::max()));
    }

    // Core stamps match_done_tai_ns only after submit_order returns, which is after a trace completed inside
    // it has been copied out, so the engine stamps it first. A slot keeps what its last occupant left, so all
    // four counters are written every time.
    void finishTrace(TraceId id, const std::array<std::int32_t, 4>& counters = {}) {
        OrderTrace& trace = g_telemetry_arena[trace_index(id)];
        trace.ext_counters = counters;
        trace.match_done_tai_ns = get_synced_time_ns();
        complete_trace(policy_, id);
    }

    HourIndex batchFirstHour() const {
        return static_cast<HourIndex>(std::min<std::uint64_t>(book_.firstHour(), std::numeric_limits<HourIndex>::max()));
    }

    // cmd.side is what the client sent, 0 for a cancel.
    void reject(const BatchCommand& cmd, Reject why) {
        BatchEvent event{};
        event.type = EventType::REJECTED;
        event.batchId = book_.batchId();
        event.participantId = cmd.participantId;
        event.orderToken = cmd.orderToken;
        event.session_id = cmd.sessionId;
        event.side = cmd.side;
        event.rejectReason = why.reason;
        event.rejectDetail = why.detail;
        if (why.reason == RejectReason::SLOT_NOT_ELIGIBLE) {
            // Name the hour, which the 16-bit detail cannot hold. It stops at what an event can carry.
            event.hour = static_cast<HourIndex>(std::min<std::uint64_t>(std::uint64_t{ cmd.baseHour } + why.detail,
                                                                       std::numeric_limits<HourIndex>::max()));
        }
        emit(event);
    }

    void onOrder(const BatchCommand& cmd) {
        const BatchBook::Admission admission = book_.admit(cmd);
        if (admission.reject) {
            reject(cmd, admission.reject);
            finishTrace(cmd.trace_id);
            return;
        }
        const StoredOrder& stored = book_.order({ cmd.side, admission.rank });
        BatchEvent event{};
        event.type = EventType::ACCEPTED;
        event.batchId = book_.batchId();
        event.participantId = stored.participant;
        event.orderToken = stored.token;
        event.session_id = stored.session;
        event.side = stored.side;
        event.rank = admission.rank;
        event.units = stored.totalUnits;
        emit(event);
        finishTrace(cmd.trace_id);
    }

    void onCancel(const BatchCommand& cmd) {
        const std::optional<BatchBook::Ref> ref = book_.cancel(cmd.participantId, cmd.orderToken);
        if (!ref) {
            reject(cmd, Reject{ RejectReason::UNKNOWN_ORDER, 0 });
        } else {
            const StoredOrder& stored = book_.order(*ref);
            BatchEvent event{};
            event.type = EventType::CANCELED;
            event.batchId = book_.batchId();
            event.participantId = stored.participant;
            event.orderToken = stored.token;
            event.session_id = stored.session; // the order's own session
            event.side = stored.side;
            event.rank = ref->rank;
            event.units = stored.totalUnits;
            emit(event);
        }
        finishTrace(cmd.trace_id);
    }

    struct Totals {
        std::uint64_t fills{ 0 };
        std::uint64_t liftedUnits{ 0 };
        std::uint32_t filledOrders{ 0 };
        std::uint32_t unfilledOrders{ 0 };
    };

    // The clearing module lists fills order by order, in the order of its input, so one pass over them
    // is enough.
    Totals emitSide(BatchId id, Side side, const std::vector<ClearingOrder>& orders, const std::vector<Fill>& fills,
                    const ClearingResult& result) {
        Totals totals;
        std::size_t next = 0;
        for (const ClearingOrder& ordered : orders) {
            const std::uint32_t rank = static_cast<std::uint32_t>(ordered.id); // an order's id is its rank
            const StoredOrder& stored = book_.order({ side, rank });
            std::uint64_t filled = 0;
            std::uint64_t lifted = 0;
            std::int64_t amount = 0;
            for (; next < fills.size() && fills[next].order == ordered.id; ++next) {
                const Fill& fill = fills[next];
                const std::optional<PriceTicks>& price = result.itemPrice.at(fill.item);
                if (!price) throw std::logic_error("BatchEngine: the clear filled an item that has no price");
                const catalog::Sku sku = catalog::itemSku(fill.item);
                const bool isLifted = side == Side::BID && catalog::isLifted(sku, stored.skus);

                BatchEvent event{};
                event.type = EventType::FILL;
                event.batchId = id;
                event.participantId = stored.participant;
                event.orderToken = stored.token;
                event.session_id = stored.session;
                event.side = side;
                event.rank = rank;
                event.accelerator = sku.accelerator;
                event.interconnect = sku.interconnect;
                event.region = sku.region;
                event.lifted = isLifted ? 1 : 0;
                event.hour = static_cast<HourIndex>(book_.firstHour() + catalog::itemSlot(fill.item));
                event.price = *price;
                event.units = fill.units;
                emit(event);

                filled += fill.units;
                if (isLifted) lifted += fill.units;
                amount += static_cast<std::int64_t>(fill.units) * static_cast<std::int64_t>(*price);
                ++totals.fills;
            }
            if (filled > stored.totalUnits) throw std::logic_error("BatchEngine: the clear filled more than the order asked for");

            BatchEvent done{};
            done.type = EventType::ORDER_DONE;
            done.batchId = id;
            done.participantId = stored.participant;
            done.orderToken = stored.token;
            done.session_id = stored.session;
            done.side = side;
            done.rank = rank;
            done.units = static_cast<NodeUnits>(filled);
            done.unfilledUnits = stored.totalUnits - static_cast<NodeUnits>(filled);
            done.liftedUnits = static_cast<NodeUnits>(lifted);
            done.amount = amount;
            emit(done);

            totals.liftedUnits += lifted;
            if (filled > 0) ++totals.filledOrders; else ++totals.unfilledOrders;
        }
        if (next != fills.size()) throw std::logic_error("BatchEngine: the clear listed a fill out of order");
        return totals;
    }

    void onBatchClear(const BatchCommand& cmd) {
        const BatchId id = cmd.batchId; // not checked against earlier ids
        const ClearingInput input = book_.clearingInput();

        std::optional<ClearingResult> result;
        VoidReason voided = VoidReason::NONE;
        std::uint64_t voidArcs = book_.arcs();
        std::uint64_t voidPivots = 0;
        try {
            result = config_.clearFunction(input, config_.clearing);
        } catch (const ClearingBudgetExceeded& stopped) {
            // Every replica stops at the same pivot.
            voided = VoidReason::PIVOT_BUDGET;
            voidArcs = stopped.arcs();
            voidPivots = stopped.pivots();
        } catch (const std::invalid_argument&) {
            // The book's limits should keep the clear from ever seeing such a batch.
            voided = VoidReason::REFUSED_INPUT;
        } catch (const std::logic_error&) {
            // The clear failed its own optimality check: a bug in the solver, not a property of the input.
            voided = VoidReason::SOLVER_ERROR;
        }

        std::uint64_t liftedUnits = 0;
        std::uint64_t filledBids = 0;
        std::uint64_t unfilledBids = 0;
        std::uint64_t pivots = voidPivots;
        if (voided != VoidReason::NONE) {
            // No fills, and one event that names the reason. Every order lapses with the batch.
            BatchEvent event{};
            event.type = EventType::BATCH_VOIDED;
            event.batchId = id;
            event.session_id = cmd.sessionId;
            event.hour = batchFirstHour();
            event.voidReason = voided;
            event.bids = static_cast<std::uint32_t>(input.bids.size());
            event.asks = static_cast<std::uint32_t>(input.asks.size());
            event.arcs = static_cast<std::uint32_t>(voidArcs);
            event.pivots = static_cast<std::uint32_t>(voidPivots);
            emit(event);
        } else {
            const Totals bids = emitSide(id, Side::BID, input.bids, result->bidFills, *result);
            const Totals asks = emitSide(id, Side::ASK, input.asks, result->askFills, *result);
            liftedUnits = bids.liftedUnits;
            filledBids = bids.filledOrders;
            unfilledBids = bids.unfilledOrders;
            pivots = result->stats.augmentations;

            BatchEvent event{};
            event.type = EventType::BATCH_DONE;
            event.batchId = id;
            event.session_id = cmd.sessionId;
            event.hour = batchFirstHour();
            event.units = result->tradedUnits;
            event.liftedUnits = static_cast<NodeUnits>(liftedUnits);
            event.amount = result->welfare;
            event.rankSum = result->tieCost;
            event.fills = static_cast<std::uint32_t>(bids.fills + asks.fills);
            event.certified = result->stats.certified ? 1 : 0;
            event.bids = static_cast<std::uint32_t>(input.bids.size());
            event.asks = static_cast<std::uint32_t>(input.asks.size());
            event.arcs = static_cast<std::uint32_t>(result->stats.arcs);
            event.pivots = static_cast<std::uint32_t>(pivots);
            emit(event);
        }

        // The batch statistics ride on the trace of the command that did the work.
        std::array<std::int32_t, 4> counters{};
        counters[static_cast<std::size_t>(GpuExtSlot::LiftedUnits)] = clampToInt32(liftedUnits);
        counters[static_cast<std::size_t>(GpuExtSlot::BidsFilled)] = clampToInt32(filledBids);
        counters[static_cast<std::size_t>(GpuExtSlot::BidsUnfilled)] = clampToInt32(unfilledBids);
        counters[static_cast<std::size_t>(GpuExtSlot::SolverPivots)] = clampToInt32(pivots);
        finishTrace(cmd.trace_id, counters);

        book_.startBatchAfter(id);
    }

    TelemetryPolicy& policy_;
    EngineConfig config_;
    BatchBook book_;
    std::uint64_t nextSequenceNumber_{ 1 }; // one feed
};
