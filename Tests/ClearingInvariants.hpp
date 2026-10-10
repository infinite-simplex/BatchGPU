#pragma once
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BatchClearing.hpp"

// What every correct result must satisfy whatever the tie-break chose. It does not check optimality:
// the recorded answers and the reference solver do that.
namespace invariants {

inline std::int64_t curveValue(const ClearingOrder& order, std::uint64_t units) {
    std::int64_t value = 0;
    for (std::size_t k = 0; k < order.stepCount && units > 0; ++k) {
        const std::uint64_t used = std::min<std::uint64_t>(units, order.steps[k].quantity);
        value += static_cast<std::int64_t>(used) * static_cast<std::int64_t>(order.steps[k].price);
        units -= used;
    }
    return value;
}

struct Marginal {
    std::optional<PriceTicks> lastFilled;   // price of the step holding the last filled unit
    std::optional<PriceTicks> nextUnfilled; // price of the step holding the next unit
};

inline Marginal marginalOf(const ClearingOrder& order, std::uint64_t units) {
    Marginal m;
    std::uint64_t before = 0;
    for (std::size_t k = 0; k < order.stepCount; ++k) {
        const std::uint64_t after = before + order.steps[k].quantity;
        if (units >= 1 && units > before && units <= after) m.lastFilled = order.steps[k].price;
        if (units + 1 > before && units + 1 <= after) m.nextUnfilled = order.steps[k].price;
        before = after;
    }
    return m;
}

struct SideTotals {
    std::map<std::pair<OrderRef, ItemIndex>, NodeUnits> fill;
    std::unordered_map<OrderRef, std::uint64_t> total;
    std::vector<std::uint64_t> atItem;
};

inline void collect(const std::vector<ClearingOrder>& orders, const std::vector<Fill>& fills,
                    std::uint32_t itemCount, SideTotals& out) {
    std::unordered_map<OrderRef, const ClearingOrder*> byId;
    for (const ClearingOrder& o : orders) byId[o.id] = &o;
    out.atItem.assign(itemCount, 0);
    for (const Fill& f : fills) {
        const auto it = byId.find(f.order);
        ASSERT_NE(it, byId.end()) << "fill names an unknown order " << f.order;
        const ClearingOrder& o = *it->second;
        ASSERT_NE(std::find(o.items.begin(), o.items.end(), f.item), o.items.end())
            << "order " << f.order << " filled at an item it does not list";
        ASSERT_GE(f.units, 1u);
        ASSERT_LE(f.units, o.perSlotCap) << "order " << f.order << " exceeds its per-slot cap";
        ASSERT_TRUE(out.fill.emplace(std::make_pair(f.order, f.item), f.units).second)
            << "two fill lines for order " << f.order << " at item " << f.item;
        out.total[f.order] += f.units;
        out.atItem[f.item] += f.units;
    }
    for (const ClearingOrder& o : orders) {
        std::uint64_t curve = 0;
        for (std::size_t k = 0; k < o.stepCount; ++k) curve += o.steps[k].quantity;
        ASSERT_LE(out.total[o.id], curve) << "order " << o.id << " exceeds its curve";
    }
}

inline void expectValid(const ClearingInput& in, const ClearingResult& r) {
    ASSERT_EQ(r.itemUnits.size(), in.itemCount);
    ASSERT_EQ(r.itemPrice.size(), in.itemCount);

    SideTotals bids;
    SideTotals asks;
    collect(in.bids, r.bidFills, in.itemCount, bids);
    if (::testing::Test::HasFatalFailure()) return;
    collect(in.asks, r.askFills, in.itemCount, asks);
    if (::testing::Test::HasFatalFailure()) return;

    std::uint64_t traded = 0;
    for (ItemIndex i = 0; i < in.itemCount; ++i) {
        ASSERT_EQ(bids.atItem[i], asks.atItem[i]) << "item " << i << " is out of balance";
        ASSERT_EQ(bids.atItem[i], r.itemUnits[i]);
        traded += r.itemUnits[i];
        ASSERT_EQ(r.itemPrice[i].has_value(), r.itemUnits[i] > 0) << "item " << i;
    }
    ASSERT_EQ(traded, r.tradedUnits);

    std::int64_t bidValue = 0;
    std::int64_t askCost = 0;
    for (const ClearingOrder& o : in.bids) bidValue += curveValue(o, bids.total[o.id]);
    for (const ClearingOrder& o : in.asks) askCost += curveValue(o, asks.total[o.id]);
    ASSERT_EQ(r.welfare, bidValue - askCost);

    // Necessary conditions on the prices, from the residual arcs: a bid never pays more than its last
    // filled step is worth and never leaves unbought a unit worth more than the price; an ask never
    // sells below its last filled step and never holds back a unit it would sell for less than the price.
    for (const ClearingOrder& o : in.bids) {
        const Marginal m = marginalOf(o, bids.total[o.id]);
        for (ItemIndex item : o.items) {
            if (!r.itemPrice[item]) continue;
            const PriceTicks p = *r.itemPrice[item];
            const auto found = bids.fill.find({ o.id, item });
            const NodeUnits filled = found == bids.fill.end() ? 0 : found->second;
            if (filled > 0 && m.lastFilled) {
                EXPECT_LE(p, *m.lastFilled) << "bid " << o.id << " overpays at item " << item;
            }
            if (filled < o.perSlotCap && m.nextUnfilled) {
                EXPECT_GE(p, *m.nextUnfilled)
                    << "bid " << o.id << " would buy more at item " << item;
            }
        }
    }
    for (const ClearingOrder& o : in.asks) {
        const Marginal m = marginalOf(o, asks.total[o.id]);
        for (ItemIndex item : o.items) {
            if (!r.itemPrice[item]) continue;
            const PriceTicks p = *r.itemPrice[item];
            const auto found = asks.fill.find({ o.id, item });
            const NodeUnits filled = found == asks.fill.end() ? 0 : found->second;
            if (filled > 0 && m.lastFilled) {
                EXPECT_GE(p, *m.lastFilled) << "ask " << o.id << " sells below cost at item " << item;
            }
            if (filled < o.perSlotCap && m.nextUnfilled) {
                EXPECT_LE(p, *m.nextUnfilled)
                    << "ask " << o.id << " would sell more at item " << item;
            }
        }
    }
}

} // namespace invariants
