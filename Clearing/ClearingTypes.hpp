#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include "GpuTypes.hpp"
#include "Limits.hpp"

// The input is already expanded: an order lists item indices, not masks, so this module knows nothing
// about GPU models, regions or the partial order. The engine expands masks into item lists.

// This many node-hours at this price. A bid's prices never rise from step to step, and an ask's never fall.
struct Step {
    NodeUnits quantity{ 0 };
    PriceTicks price{ 0 };
};

struct ClearingOrder {
    OrderRef id{ 0 };       // unique within its side
    NodeUnits perSlotCap{ 0 }; // most nodes taken (bid) or offered (ask) at any one item
    std::uint8_t stepCount{ 0 };
    std::array<Step, limits::MAX_STEPS> steps{};
    std::vector<ItemIndex> items; // accepted (bid) or offered (ask); each < itemCount, no repeats
};

// Orders are listed in arrival order, and an order's position in its vector is its arrival rank: when
// two allocations are equally good, the one favoring earlier orders wins. So the order of these vectors
// is part of the input. bidRank and askRank are optional; when present they hold one strictly increasing
// rank per order and replace the position. A gap is an order that arrived and was cancelled.
struct ClearingInput {
    std::uint32_t itemCount{ 0 };
    std::vector<ClearingOrder> bids;
    std::vector<ClearingOrder> asks;
    std::vector<std::uint32_t> bidRank;
    std::vector<std::uint32_t> askRank;
};

struct Fill {
    OrderRef order{ 0 };
    ItemIndex item{ 0 };
    NodeUnits units{ 0 };
};

struct ClearingStats {
    std::uint64_t nodes{ 0 };
    std::uint64_t arcs{ 0 };
    // Shortest-path searches for the reference solver, pivots for network simplex.
    std::uint32_t augmentations{ 0 };
    // True when the solver proved the flow optimal before returning it. Network simplex does on every
    // clear, and the reference solver does not.
    bool certified{ false };
    // Wall time per phase, in microseconds. Nothing in the result depends on it.
    std::uint64_t buildMicros{ 0 };
    std::uint64_t solveMicros{ 0 };
    std::uint64_t priceMicros{ 0 };
};

struct ClearingResult {
    std::vector<Fill> bidFills;
    std::vector<Fill> askFills;
    // Both are indexed by item.
    std::vector<NodeUnits> itemUnits;
    // The minimal optimal price, empty for an item that did not trade.
    std::vector<std::optional<PriceTicks>> itemPrice;
    std::int64_t welfare{ 0 }; // bid value minus ask cost, in ticks
    // The sum, over every unit traded, of the arrival ranks of its bid and its ask. Minimized among
    // allocations with equal welfare and volume, so every correct solver gives the same value even when
    // the allocations differ.
    std::int64_t tieCost{ 0 };
    NodeUnits tradedUnits{ 0 };
    ClearingStats stats;
};
