#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>

#include "BatchCommand.hpp"
#include "Catalog.hpp"
#include "Limits.hpp"
#include "RejectReason.hpp"

struct Reject {
    RejectReason reason{ RejectReason::NONE };
    std::uint16_t detail{ 0 };

    explicit constexpr operator bool() const { return reason != RejectReason::NONE; }
    friend constexpr bool operator==(const Reject&, const Reject&) = default;
};

namespace order_checks_detail {

constexpr std::uint64_t maskOf(const BatchCommand& c, catalog::Attribute attribute) {
    switch (attribute) {
    case catalog::Attribute::ACCELERATOR: return c.acceleratorMask;
    case catalog::Attribute::INTERCONNECT: return c.interconnectMask;
    case catalog::Attribute::REGION: return c.regionMask;
    case catalog::Attribute::SLOTS: return c.slotMask;
    }
    return 0;
}

} // namespace order_checks_detail

// Looks at the order alone, so the handler can decide it before the order is sequenced and the engine
// can repeat it as a guard. Returns the first problem, or none. The order of the checks is part of the
// protocol: the handler and every replica must agree on which problem is first.
constexpr Reject checkOrder(const BatchCommand& c) {
    using catalog::Attribute;

    if (c.side != Side::BID && c.side != Side::ASK) {
        return { RejectReason::BAD_SIDE, static_cast<std::uint16_t>(c.side) };
    }

    for (std::size_t i = 0; i < catalog::ATTRIBUTE_COUNT; ++i) {
        const Attribute attribute = static_cast<Attribute>(i);
        const std::uint64_t mask = order_checks_detail::maskOf(c, attribute);
        if (mask == 0) return { RejectReason::EMPTY_MASK, maskDetail(attribute, 0) };
        const int bad = catalog::lowestBitOutside(mask, catalog::optionCount(attribute));
        if (bad >= 0) {
            return { RejectReason::MASK_BIT_OUT_OF_RANGE, maskDetail(attribute, static_cast<unsigned>(bad)) };
        }
    }

    // An ask offers one accelerator, one interconnect and one region: the first three attributes.
    if (c.side == Side::ASK) {
        for (std::size_t i = 0; i < 3; ++i) {
            const Attribute attribute = static_cast<Attribute>(i);
            if (std::popcount(order_checks_detail::maskOf(c, attribute)) != 1) {
                return { RejectReason::ASK_MASK_NOT_SINGLE, maskDetail(attribute, 0) };
            }
        }
    }

    if (c.perSlotCap == 0 || c.perSlotCap > limits::MAX_PER_SLOT_CAP) {
        return { RejectReason::PER_SLOT_CAP_OUT_OF_RANGE, 0 };
    }

    if (c.stepCount < 1 || c.stepCount > limits::MAX_STEPS) {
        return { RejectReason::STEP_COUNT_OUT_OF_RANGE, c.stepCount };
    }
    for (std::size_t k = 0; k < c.stepCount; ++k) {
        const CurveStep& step = c.steps[k];
        const std::uint16_t index = static_cast<std::uint16_t>(k);
        if (step.quantity == 0) return { RejectReason::STEP_QUANTITY_ZERO, index };
        if (step.price > limits::MAX_PRICE_TICKS) return { RejectReason::STEP_PRICE_OVER_MAX, index };
        if (step.quantity > limits::MAX_ORDER_UNITS) return { RejectReason::STEP_QUANTITY_OVER_MAX, index };
        if (k > 0) {
            const PriceTicks before = c.steps[k - 1].price;
            const bool breaksOrder = c.side == Side::BID ? step.price > before : step.price < before;
            if (breaksOrder) return { RejectReason::STEP_PRICE_ORDER, index };
        }
    }

    // Summed in 64 bits, so that it cannot wrap.
    std::uint64_t total = 0;
    for (std::size_t k = 0; k < c.stepCount; ++k) {
        total += c.steps[k].quantity;
        if (total > limits::MAX_ORDER_UNITS) {
            return { RejectReason::TOTAL_QUANTITY_OVER_MAX, static_cast<std::uint16_t>(k) };
        }
    }

    return {};
}
