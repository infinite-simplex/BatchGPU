#pragma once
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "BatchCommand.hpp"
#include "BatchEngine.hpp"
#include "BatchEvent.hpp"
#include "SPSCProducerPolicy.hpp" // Core: TestingPolicy

namespace enginefix {

// In the order of the tables in Catalog.hpp.
constexpr std::uint32_t A100 = 1u << 0;
constexpr std::uint32_t H100 = 1u << 1;
constexpr std::uint32_t H200 = 1u << 2;
constexpr std::uint32_t MI300X = 1u << 3;
constexpr std::uint8_t PCIE = 1u << 0;
constexpr std::uint8_t NVLINK = 1u << 1;
constexpr std::uint32_t US_WEST = 1u << 0;
constexpr std::uint32_t US_EAST = 1u << 1;
constexpr std::uint32_t EU_WEST = 1u << 2;

// Clear 0 is at 30 minutes past hour 1000, so it sells hours 1001 to 1047; clear 1 sells 1002 to 1048.
constexpr std::uint64_t FIRST_CLEAR_SECONDS = 1000 * 3600 + 1800;
constexpr HourIndex H0 = 1001; // the first hour of batch 0

inline schedule::Schedule testSchedule() { return schedule::Schedule{ FIRST_CLEAR_SECONDS, 3600 }; }

inline EngineConfig testConfig() {
    EngineConfig config;
    config.schedule = testSchedule();
    return config;
}

struct Order {
    Side side{ Side::BID };
    ParticipantId participant{ 1 };
    OrderToken token{ 1 };
    HourIndex baseHour{ H0 };
    std::uint64_t slots{ 1 }; // bit s is hour baseHour + s
    std::uint32_t accel{ A100 };
    std::uint8_t inter{ NVLINK };
    std::uint32_t region{ US_WEST };
    NodeUnits cap{ 1 };
    std::vector<CurveStep> steps{ { 1, 100 } };

    BatchCommand command() const {
        BatchCommand c{};
        c.type = CommandType::ORDER;
        c.participantId = participant;
        c.orderToken = token;
        c.side = side;
        c.acceleratorMask = accel;
        c.interconnectMask = inter;
        c.regionMask = region;
        c.baseHour = baseHour;
        c.slotMask = slots;
        c.perSlotCap = cap;
        c.stepCount = static_cast<std::uint8_t>(steps.size());
        for (std::size_t k = 0; k < steps.size() && k < limits::MAX_STEPS; ++k) c.steps[k] = steps[k];
        return c;
    }
};

inline Order ask(ParticipantId participant, OrderToken token, std::uint64_t slots, NodeUnits cap,
                 std::initializer_list<CurveStep> steps, std::uint32_t accel = A100, std::uint8_t inter = NVLINK,
                 std::uint32_t region = US_WEST) {
    Order o;
    o.side = Side::ASK;
    o.participant = participant;
    o.token = token;
    o.slots = slots;
    o.cap = cap;
    o.steps = steps;
    o.accel = accel;
    o.inter = inter;
    o.region = region;
    return o;
}

inline Order bid(ParticipantId participant, OrderToken token, std::uint64_t slots, NodeUnits cap,
                 std::initializer_list<CurveStep> steps, std::uint32_t accel = A100, std::uint8_t inter = NVLINK,
                 std::uint32_t region = US_WEST) {
    Order o = ask(participant, token, slots, cap, steps, accel, inter, region);
    o.side = Side::BID;
    return o;
}

inline std::string describe(const BatchEvent& e) {
    std::ostringstream out;
    out << "#" << e.sequence_number << " " << toString(e.type) << " batch=" << e.batchId
        << " who=" << e.participantId << "/" << e.orderToken << " session=" << e.session_id
        << " side=" << (e.side == Side::BID ? "B" : e.side == Side::ASK ? "A" : "-") << " rank=" << e.rank;
    if (e.type == EventType::REJECTED) {
        out << " reason=" << toString(e.rejectReason) << " detail=" << e.rejectDetail << " hour=" << e.hour;
    }
    if (e.type == EventType::BATCH_DONE || e.type == EventType::BATCH_VOIDED) out << " first_hour=" << e.hour;
    if (e.type == EventType::FILL) {
        out << " item=" << int(e.accelerator) << "/" << int(e.interconnect) << "/" << int(e.region) << "@" << e.hour
            << " price=" << e.price << " lifted=" << int(e.lifted);
    }
    out << " units=" << e.units << " unfilled=" << e.unfilledUnits << " lifted=" << e.liftedUnits
        << " amount=" << e.amount << " rankSum=" << e.rankSum << " fills=" << e.fills << " bids=" << e.bids
        << " asks=" << e.asks << " arcs=" << e.arcs << " pivots=" << e.pivots
        << " certified=" << int(e.certified) << " void=" << int(static_cast<std::uint8_t>(e.voidReason));
    return out.str();
}

// Field by field, since the padding between fields means nothing.
inline bool sameEvent(const BatchEvent& a, const BatchEvent& b) {
    return a.sequence_number == b.sequence_number && a.batchId == b.batchId && a.participantId == b.participantId &&
           a.orderToken == b.orderToken && a.session_id == b.session_id && a.side == b.side && a.rank == b.rank &&
           a.rejectReason == b.rejectReason && a.rejectDetail == b.rejectDetail && a.accelerator == b.accelerator &&
           a.interconnect == b.interconnect && a.region == b.region && a.lifted == b.lifted && a.hour == b.hour &&
           a.price == b.price && a.units == b.units && a.unfilledUnits == b.unfilledUnits &&
           a.liftedUnits == b.liftedUnits && a.amount == b.amount && a.rankSum == b.rankSum && a.fills == b.fills &&
           a.bids == b.bids && a.asks == b.asks && a.arcs == b.arcs && a.pivots == b.pivots &&
           a.certified == b.certified && a.voidReason == b.voidReason && a.type == b.type;
}

inline ::testing::AssertionResult sameEvents(const std::vector<BatchEvent>& expected,
                                             const std::vector<BatchEvent>& actual) {
    const std::size_t common = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (!sameEvent(expected[i], actual[i])) {
            return ::testing::AssertionFailure()
                   << "event " << i << " differs\n  expected " << describe(expected[i]) << "\n  actual   "
                   << describe(actual[i]);
        }
    }
    if (expected.size() != actual.size()) {
        return ::testing::AssertionFailure() << "expected " << expected.size() << " events, got " << actual.size();
    }
    return ::testing::AssertionSuccess();
}

inline std::vector<BatchEvent> ofType(const std::vector<BatchEvent>& events, EventType type) {
    std::vector<BatchEvent> out;
    for (const BatchEvent& e : events) {
        if (e.type == type) out.push_back(e);
    }
    return out;
}

// Drives an engine the way the gateway and Core's MatchingService do: each command gets the next trace
// id and the sender's session, and goes to submit_order.
class Harness {
public:
    explicit Harness(const EngineConfig& config = testConfig()) : engine(policy_, config) {}

    // Returns the events that this one command caused.
    std::vector<BatchEvent> send(BatchCommand cmd, SessionId session = 1) {
        cmd.trace_id = nextTrace_++;
        cmd.sessionId = session;
        // Marks the command's slot in the trace arena with its id, as the ingress would stamp a time, so
        // that the trace that completes says whose it is. The pop is stamped as Core's host stamps it, and
        // the counters hold what the slot's last occupant would have left.
        OrderTrace& slot = g_telemetry_arena[trace_index(cmd.trace_id)];
        slot.ingress_tai_ns = cmd.trace_id;
        slot.engine_pop_tai_ns = get_synced_time_ns();
        slot.match_done_tai_ns = 0;
        slot.ext_counters = { 9, 9, 9, 9 };
        const std::size_t before = events.size();
        engine.submit_order(cmd);
        ++commands;
        for (std::size_t k = before; k < events.size(); ++k) {
            if (events[k].type == EventType::ACCEPTED) acceptedFrom[k] = cmd;
        }
        return std::vector<BatchEvent>(events.begin() + static_cast<std::ptrdiff_t>(before), events.end());
    }
    std::vector<BatchEvent> order(const Order& o, SessionId session = 1) { return send(o.command(), session); }
    std::vector<BatchEvent> cancel(ParticipantId participant, OrderToken token, SessionId session = 1) {
        BatchCommand c{};
        c.type = CommandType::CANCEL;
        c.participantId = participant;
        c.orderToken = token;
        return send(c, session);
    }
    std::vector<BatchEvent> clearBatch(BatchId id, SessionId session = 99) {
        BatchCommand c{};
        c.type = CommandType::BATCH_CLEAR;
        c.batchId = id;
        return send(c, session);
    }

    std::vector<std::uint64_t> completedTraceIds() const {
        std::vector<std::uint64_t> ids;
        for (const OrderTrace& t : traces) ids.push_back(t.ingress_tai_ns);
        return ids;
    }

    std::vector<BatchEvent> events;
    std::vector<OrderTrace> traces;
    // The ORDER behind each ACCEPTED event, by the event's position in `events`.
    std::map<std::size_t, BatchCommand> acceptedFrom;

private:
    TestingPolicy<BatchEvent> policy_{ events, traces };

public:
    BatchEngine<TestingPolicy<BatchEvent>> engine;
    std::size_t commands{ 0 };

private:
    TraceId nextTrace_{ 1 };
};

} // namespace enginefix
