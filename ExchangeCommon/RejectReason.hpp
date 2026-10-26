#pragma once
#include <cstdint>

#include "Catalog.hpp"

// Why an order or a cancel was refused. A reject names the first problem, in a fixed check order,
// and its detail value points at it. The values are part of the protocol: clients see them in
// events, so add new reasons at the end.
enum class RejectReason : std::uint8_t {
    NONE = 0,

    // Decided by the handler, in the order the checks are made. A malformed frame closes the
    // connection instead of rejecting.
    BAD_SIDE,                  // the side byte is neither a bid nor an ask. Detail: the byte.
    EMPTY_MASK,                // a mask has no bit set. Detail: the attribute (maskDetail).
    MASK_BIT_OUT_OF_RANGE,     // a mask has a bit beyond the catalog. Detail: the attribute and the lowest such bit.
    ASK_MASK_NOT_SINGLE,       // an ask must name exactly one value. Detail: the first attribute that does not.
    PER_SLOT_CAP_OUT_OF_RANGE, // the per-slot cap is zero or over the maximum.
    STEP_COUNT_OUT_OF_RANGE,   // the step count is outside 1 to MAX_STEPS. Detail: the count.
    STEP_QUANTITY_ZERO,        // Detail: the step index.
    STEP_PRICE_OVER_MAX,       // Detail: the step index.
    STEP_QUANTITY_OVER_MAX,    // Detail: the step index.
    STEP_PRICE_ORDER,          // a bid's price rises, or an ask's falls. Detail: the step index.
    TOTAL_QUANTITY_OVER_MAX,   // Detail: the step at which the running total first passes the maximum.

    // Decided by the engine in log order. The engine repeats the checks above first, as a guard,
    // and gives the same reasons.
    TOKEN_IN_USE,      // the participant already has a live order with this token. Detail: 0.
    SLOT_NOT_ELIGIBLE, // a slot in the mask cannot be sold in this batch. Detail: the bit of the lowest such slot.
    PARTICIPANT_QUOTA, // a buyer's own quota is used up. Detail: a QuotaLimit.
    BATCH_FULL,        // the batch has no room for the order. Detail: a FullLimit.

    UNKNOWN_ORDER, // a cancel named no live order.
};

enum class FullLimit : std::uint8_t {
    ORDERS = 0,      // orders on the side
    SUBMISSIONS = 1, // submissions on the side, cancelled ones included
    ARCS = 2,        // arcs in the batch
};

// Sellers have no quota.
enum class QuotaLimit : std::uint8_t {
    OPEN_ORDERS = 0, // live bids of the buyer
    SUBMISSIONS = 1, // bids the buyer has submitted in this batch, cancelled ones included
    ARCS = 2,        // arcs over the buyer's live bids, with the new one
};

// The detail of a reject that names a mask: the attribute in the high byte and a bit in the
// low byte. The bit is 0 when the reject does not point at one.
constexpr std::uint16_t maskDetail(catalog::Attribute attribute, const unsigned bit) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(attribute) << 8) | (bit & 0xFFu));
}

constexpr const char* toString(const RejectReason reason) {
    switch (reason) {
    case RejectReason::NONE: return "NONE";
    case RejectReason::BAD_SIDE: return "BAD_SIDE";
    case RejectReason::EMPTY_MASK: return "EMPTY_MASK";
    case RejectReason::MASK_BIT_OUT_OF_RANGE: return "MASK_BIT_OUT_OF_RANGE";
    case RejectReason::ASK_MASK_NOT_SINGLE: return "ASK_MASK_NOT_SINGLE";
    case RejectReason::PER_SLOT_CAP_OUT_OF_RANGE: return "PER_SLOT_CAP_OUT_OF_RANGE";
    case RejectReason::STEP_COUNT_OUT_OF_RANGE: return "STEP_COUNT_OUT_OF_RANGE";
    case RejectReason::STEP_QUANTITY_ZERO: return "STEP_QUANTITY_ZERO";
    case RejectReason::STEP_PRICE_OVER_MAX: return "STEP_PRICE_OVER_MAX";
    case RejectReason::STEP_QUANTITY_OVER_MAX: return "STEP_QUANTITY_OVER_MAX";
    case RejectReason::STEP_PRICE_ORDER: return "STEP_PRICE_ORDER";
    case RejectReason::TOTAL_QUANTITY_OVER_MAX: return "TOTAL_QUANTITY_OVER_MAX";
    case RejectReason::TOKEN_IN_USE: return "TOKEN_IN_USE";
    case RejectReason::SLOT_NOT_ELIGIBLE: return "SLOT_NOT_ELIGIBLE";
    case RejectReason::PARTICIPANT_QUOTA: return "PARTICIPANT_QUOTA";
    case RejectReason::BATCH_FULL: return "BATCH_FULL";
    case RejectReason::UNKNOWN_ORDER: return "UNKNOWN_ORDER";
    }
    return "?";
}
