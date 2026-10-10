#pragma once
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "BatchClearing.hpp"
#include "ClearingInvariants.hpp"

namespace clearfix {

inline ClearingOrder order(OrderRef id, NodeUnits cap, std::initializer_list<Step> steps,
                           std::vector<ItemIndex> items) {
    ClearingOrder o;
    o.id = id;
    o.perSlotCap = cap;
    o.stepCount = static_cast<std::uint8_t>(steps.size());
    std::size_t k = 0;
    for (const Step& s : steps) o.steps[k++] = s;
    o.items = std::move(items);
    return o;
}

// Orders are listed in arrival order: the first bid and the first ask arrived first.
inline ClearingInput batch(std::uint32_t itemCount, std::vector<ClearingOrder> bids,
                           std::vector<ClearingOrder> asks) {
    ClearingInput in;
    in.itemCount = itemCount;
    in.bids = std::move(bids);
    in.asks = std::move(asks);
    return in;
}

inline ClearingInput reversed(ClearingInput in) {
    std::reverse(in.bids.begin(), in.bids.end());
    std::reverse(in.asks.begin(), in.asks.end());
    return in;
}

inline ClearingInput rotated(ClearingInput in) {
    if (in.bids.size() > 1) std::rotate(in.bids.begin(), in.bids.begin() + 1, in.bids.end());
    if (in.asks.size() > 1) std::rotate(in.asks.begin(), in.asks.begin() + 1, in.asks.end());
    return in;
}

// Welfare, volume and the price of an item that trades in both never depend on the tie-break. The rank
// sum is the same for every correct solver, but only if the orders are listed as the reference was
// given them: pass compareRankSum = false for a re-listed batch.
inline void expectAgreement(const ClearingInput& in, const ClearingResult& reference,
                            const ClearingResult& r, bool compareRankSum = true) {
    invariants::expectValid(in, r);
    EXPECT_EQ(r.welfare, reference.welfare);
    EXPECT_EQ(r.tradedUnits, reference.tradedUnits);
    if (compareRankSum) {
        EXPECT_EQ(r.tieCost, reference.tieCost);
    }
    for (std::size_t i = 0; i < r.itemPrice.size(); ++i) {
        if (r.itemPrice[i] && reference.itemPrice[i]) {
            EXPECT_EQ(*r.itemPrice[i], *reference.itemPrice[i]) << "item " << i;
        }
    }
}

} // namespace clearfix
