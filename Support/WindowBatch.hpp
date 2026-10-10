#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "ClearingTypes.hpp"
#include "SyntheticBatch.hpp"

// The item structure this exchange will have: an item is a GPU-and-region combination (a "group") and an
// hourly slot, and an order names a WINDOW of consecutive slots. Items of one group are linked in a chain
// by the windows that overlap, so the tree a solver builds is deep, and a pivot that links two distant
// slots has a long cycle.
//
//   ask  offers `askWindow` consecutive slots of ONE group.
//   bid  accepts `bidWindow` consecutive slots in each of `groupsPerBid` groups.
//
// Prices and quantities are uniform random: a stress test, not a forecast. Integer-only, from the same
// stream as SyntheticBatch.hpp, so the batch is the same on every platform.
namespace window {

struct Params {
    std::uint32_t groups{ 100 };
    std::uint32_t slots{ static_cast<std::uint32_t>(limits::SLOT_WINDOW) };
    std::size_t asks{ 5000 };
    std::size_t bids{ 5000 };
    std::uint32_t askWindow{ 12 };
    std::uint32_t bidWindow{ 6 };
    std::uint32_t groupsPerBid{ 3 };
    PriceTicks maxPrice{ 2000 };
    NodeUnits maxQuantity{ 20 };
};

inline ClearingInput makeBatch(const Params& p, std::uint64_t seed) {
    synthetic::Rng rng(seed);
    ClearingInput input;
    input.itemCount = p.groups * p.slots;
    const std::uint32_t askWindow = std::min(p.askWindow, p.slots);
    const std::uint32_t bidWindow = std::min(p.bidWindow, p.slots);
    const std::uint32_t groupsPerBid = std::min(p.groupsPerBid, p.groups);

    auto windowItems = [&](std::uint32_t group, std::uint32_t length, std::vector<ItemIndex>& out) {
        const std::uint32_t start = static_cast<std::uint32_t>(rng.below(p.slots - length + 1));
        for (std::uint32_t s = 0; s < length; ++s) out.push_back(group * p.slots + start + s);
    };
    auto fill = [&](ClearingOrder& o, OrderRef id, bool isBid) {
        o.id = id;
        const NodeUnits quantity = static_cast<NodeUnits>(rng.between(1, p.maxQuantity));
        o.perSlotCap = quantity;
        o.stepCount = 1;
        o.steps[0] = { quantity, static_cast<PriceTicks>(isBid ? rng.between(1, p.maxPrice) : rng.between(0, p.maxPrice)) };
    };

    for (std::size_t a = 0; a < p.asks; ++a) {
        ClearingOrder o;
        fill(o, 1 + a, false);
        windowItems(static_cast<std::uint32_t>(rng.below(p.groups)), askWindow, o.items);
        input.asks.push_back(std::move(o));
    }
    std::vector<char> used(p.groups, 0);
    for (std::size_t b = 0; b < p.bids; ++b) {
        ClearingOrder o;
        fill(o, 1'000'000 + b, true);
        // One window start for all the groups, as a buyer who wants these hours would have.
        std::vector<std::uint32_t> groups;
        while (groups.size() < groupsPerBid) {
            const std::uint32_t g = static_cast<std::uint32_t>(rng.below(p.groups));
            if (used[g]) continue;
            used[g] = 1;
            groups.push_back(g);
        }
        const std::uint32_t start = static_cast<std::uint32_t>(rng.below(p.slots - bidWindow + 1));
        for (std::uint32_t g : groups) {
            used[g] = 0;
            for (std::uint32_t s = 0; s < bidWindow; ++s) o.items.push_back(g * p.slots + start + s);
        }
        std::sort(o.items.begin(), o.items.end());
        input.bids.push_back(std::move(o));
    }
    return input;
}

} // namespace window
