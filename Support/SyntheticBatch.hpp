#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "ClearingTypes.hpp"

// The numbers are synthetic and say nothing about what real bidders and providers will send: they
// exercise the solver and show how its cost grows.
//
// It uses its own SplitMix64 stream rather than <random>, because the distributions in <random> differ
// between standard libraries and a test must draw the same batch everywhere.
namespace synthetic {

struct Params {
    std::uint32_t items{ 50 };
    std::size_t asks{ 20 };
    std::size_t bids{ 100 };
    std::uint32_t offerPerAsk{ 5 };    // items each ask offers (at most SLOT_WINDOW)
    std::uint32_t acceptPerBid{ 10 };  // items each bid accepts
    NodeUnits maxStepQuantity{ 8 };
    NodeUnits maxPerSlotCap{ 4 };
    PriceTicks maxPrice{ 60 };         // a small range makes exact ties common
    std::size_t maxSteps{ limits::MAX_STEPS };
};

// SplitMix64 (Vigna's public-domain reference).
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // A value in [0, n). The modulo bias is irrelevant for test data.
    std::uint64_t below(std::uint64_t n) { return next() % n; }
    // A value in [lo, hi].
    std::uint64_t between(std::uint64_t lo, std::uint64_t hi) { return lo + below(hi - lo + 1); }

private:
    std::uint64_t state_;
};

// Rejection sampling: fast when count is much smaller than items.
inline std::vector<ItemIndex> pickItems(Rng& rng, std::uint32_t items, std::uint32_t count,
                                        std::vector<char>& used) {
    count = std::min(count, items);
    std::vector<ItemIndex> chosen;
    chosen.reserve(count);
    while (chosen.size() < count) {
        const ItemIndex item = static_cast<ItemIndex>(rng.below(items));
        if (used[item]) continue;
        used[item] = 1;
        chosen.push_back(item);
    }
    for (ItemIndex item : chosen) used[item] = 0;
    return chosen;
}

inline ClearingOrder makeOrder(Rng& rng, OrderRef id, bool isBid, const Params& p,
                               std::vector<char>& used, std::uint32_t itemCount) {
    ClearingOrder order;
    order.id = id;
    order.perSlotCap = static_cast<NodeUnits>(rng.between(1, p.maxPerSlotCap));
    const std::size_t steps = static_cast<std::size_t>(
        rng.between(1, std::min<std::size_t>(p.maxSteps, limits::MAX_STEPS)));
    order.stepCount = static_cast<std::uint8_t>(steps);
    std::vector<PriceTicks> prices;
    for (std::size_t k = 0; k < steps; ++k) {
        prices.push_back(static_cast<PriceTicks>(rng.between(isBid ? 1 : 0, p.maxPrice)));
    }
    if (isBid) {
        std::sort(prices.begin(), prices.end(), std::greater<>());
    } else {
        std::sort(prices.begin(), prices.end());
    }
    for (std::size_t k = 0; k < steps; ++k) {
        order.steps[k] = { static_cast<NodeUnits>(rng.between(1, p.maxStepQuantity)), prices[k] };
    }
    order.items = pickItems(rng, itemCount, isBid ? p.acceptPerBid : p.offerPerAsk, used);
    return order;
}

inline ClearingInput makeBatch(const Params& p, std::uint64_t rngSeed) {
    Rng rng(rngSeed);
    ClearingInput input;
    input.itemCount = p.items;
    std::vector<char> used(p.items, 0);
    Params capped = p;
    capped.offerPerAsk = std::min<std::uint32_t>(p.offerPerAsk, static_cast<std::uint32_t>(limits::SLOT_WINDOW));
    for (std::size_t a = 0; a < p.asks; ++a) {
        input.asks.push_back(makeOrder(rng, 1 + a, false, capped, used, p.items));
    }
    for (std::size_t b = 0; b < p.bids; ++b) {
        input.bids.push_back(makeOrder(rng, 1'000'000 + b, true, capped, used, p.items));
    }
    return input;
}

} // namespace synthetic
