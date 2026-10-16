#pragma once
#include <cstdint>
#include <type_traits>

#include "Alias.hpp" // SessionId (Core)
#include "BatchCommand.hpp"
#include "GpuTypes.hpp"
#include "RejectReason.hpp"

// One fixed-size POD with a type tag. A field means something only for the types that list it below,
// and is zero otherwise. There is no wall clock, so an event carries the batch id and the hour
// instead of a timestamp: two replicas then emit the same events byte for byte. Core requires
// session_id, because it routes the private ack by it.
//
//   ACCEPTED      an order entered the batch: batchId, participantId, orderToken, session_id, side,
//                 rank, units (the order's whole quantity).
//   REJECTED      an order or a cancel was refused: batchId, participantId, orderToken, session_id,
//                 side (as sent, 0 for a cancel), rejectReason, rejectDetail, and for
//                 SLOT_NOT_ELIGIBLE the hour (the lowest hour that cannot be sold).
//   CANCELED      a live order was cancelled: as ACCEPTED, with that order's own session_id.
//   FILL          one fill of an order at one item: batchId, participantId, orderToken, session_id,
//                 side, rank, accelerator, interconnect, region, hour (the item), price (the item's
//                 one price), units, lifted (1 when some item the order accepts is strictly below
//                 this one; always 0 for an ask).
//   ORDER_DONE    the last event of an order in a cleared batch, which every order gets: batchId,
//                 participantId, orderToken, session_id, side, rank, units (filled in all),
//                 unfilledUnits, liftedUnits, amount (what a bid paid or an ask received, in ticks).
//   BATCH_DONE    a batch cleared: batchId, hour (the first hour the batch sold), units (traded),
//                 liftedUnits, amount (the welfare), rankSum (the tie-break objective), fills (the
//                 FILL events), certified (the clear proved its answer optimal), bids, asks, arcs,
//                 pivots (the size of the clear and the work it did).
//   BATCH_VOIDED  a batch could not be cleared and nothing traded: batchId, hour (as in BATCH_DONE),
//                 voidReason, bids, asks, arcs, pivots (where it stopped).
//
// For a cleared batch the events come in this order: each bid in arrival order, its FILLs (by item)
// and then its ORDER_DONE; the same for each ask; then BATCH_DONE. A voided batch emits only
// BATCH_VOIDED.
enum class EventType : std::uint8_t {
    ACCEPTED,
    REJECTED,
    CANCELED,
    FILL,
    ORDER_DONE,
    BATCH_DONE,
    BATCH_VOIDED,
};

constexpr const char* toString(EventType type) {
    switch (type) {
    case EventType::ACCEPTED: return "ACCEPTED";
    case EventType::REJECTED: return "REJECTED";
    case EventType::CANCELED: return "CANCELED";
    case EventType::FILL: return "FILL";
    case EventType::ORDER_DONE: return "ORDER_DONE";
    case EventType::BATCH_DONE: return "BATCH_DONE";
    case EventType::BATCH_VOIDED: return "BATCH_VOIDED";
    }
    return "?";
}

enum class VoidReason : std::uint8_t {
    NONE = 0,
    PIVOT_BUDGET = 1,  // the clear ran out of pivots
    REFUSED_INPUT = 2, // the clear refused the batch, which the engine's own limits should prevent
    SOLVER_ERROR = 3,  // the clear failed its own check, which is a bug
};

constexpr const char* toString(VoidReason reason) {
    switch (reason) {
    case VoidReason::NONE: return "NONE";
    case VoidReason::PIVOT_BUDGET: return "PIVOT_BUDGET";
    case VoidReason::REFUSED_INPUT: return "REFUSED_INPUT";
    case VoidReason::SOLVER_ERROR: return "SOLVER_ERROR";
    }
    return "?";
}

struct BatchEvent {
    std::uint64_t sequence_number; // monotonic on the feed, so a subscriber can see a gap

    BatchId batchId;

    ParticipantId participantId;
    OrderToken orderToken;
    SessionId session_id;
    Side side;
    // How many orders the side had accepted before this one in the batch.
    std::uint32_t rank;

    RejectReason rejectReason;
    std::uint16_t rejectDetail;

    std::uint8_t accelerator;
    std::uint8_t interconnect;
    std::uint8_t region;
    std::uint8_t lifted;
    HourIndex hour;
    PriceTicks price;

    NodeUnits units;
    NodeUnits unfilledUnits;
    NodeUnits liftedUnits;
    std::int64_t amount;
    // The sum, over every unit traded, of the ranks of its bid and its ask.
    std::int64_t rankSum;

    std::uint32_t fills;
    std::uint32_t bids;
    std::uint32_t asks;
    std::uint32_t arcs;
    std::uint32_t pivots;
    std::uint8_t certified;
    VoidReason voidReason;

    EventType type;
};

static_assert(std::is_trivially_copyable_v<BatchEvent>, "an event is copied through queues and sent as a datagram");
static_assert(std::is_standard_layout_v<BatchEvent>, "an event is a plain struct");
static_assert(sizeof(BatchEvent) <= 128, "an event, with the egress batch header, must stay far below one datagram");
