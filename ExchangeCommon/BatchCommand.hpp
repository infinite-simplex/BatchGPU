#pragma once
#include <cstdint>
#include <type_traits>

#include "Alias.hpp" // TraceId, SessionId (Core)
#include "GpuTypes.hpp"
#include "Limits.hpp"
#include "RejectReason.hpp"

// The one fixed-size POD that the gateway sequences, multicasts and stores, and that every replica
// applies. A type tag says which fields mean anything. The client's own frame is variable length
// and is parsed into this by the handler.

// The byte a client sends is the value, so a side that is neither is still representable, and a
// check can reject it (BAD_SIDE) and name it.
enum class Side : std::uint8_t { BID = 'B', ASK = 'A' };

enum class CommandType : std::uint8_t {
    ORDER,
    CANCEL,      // by the client's token
    BATCH_CLEAR, // any one is accepted
    INVALID,     // an ORDER that failed a stateless check. It carries the reason, so that the engine
                 // refuses it through the normal pipeline.
};

// This many node-hours at this price. A bid's prices never rise from one step to the next, and an
// ask's never fall.
struct CurveStep {
    NodeUnits quantity;
    PriceTicks price;
};

struct BatchCommand {
    // Written by the gateway after the handler returns. Core requires both names.
    TraceId trace_id;
    SessionId sessionId;

    CommandType type;

    // ORDER, CANCEL, INVALID: who sent it (asserted by the client) and the client's token.
    ParticipantId participantId;
    OrderToken orderToken;

    // BATCH_CLEAR: sets the nominal time. Not checked.
    BatchId batchId;

    // ORDER, INVALID: bit i of a mask is entry i of the matching table in Catalog.hpp, and bit i
    // of the slot mask is the hour baseHour + i.
    Side side;
    std::uint32_t acceleratorMask;
    std::uint8_t interconnectMask;
    std::uint32_t regionMask;
    HourIndex baseHour;
    std::uint64_t slotMask;
    NodeUnits perSlotCap; // the most nodes this order may take (a bid) or offer (an ask) in one slot
    std::uint8_t stepCount;
    CurveStep steps[limits::MAX_STEPS]; // only the first stepCount mean anything
    std::uint8_t workloadClass;         // reserved, always 0

    // INVALID: the first stateless check the order failed.
    RejectReason invalidReason;
    std::uint16_t invalidDetail;
};

static_assert(std::is_trivially_copyable_v<BatchCommand>,
              "a command is copied into the sequence store and sent as a datagram");
static_assert(std::is_standard_layout_v<BatchCommand>, "a command is a plain struct");
static_assert(sizeof(BatchCommand) <= 128,
              "a sequenced command must stay far below one Ethernet datagram");
