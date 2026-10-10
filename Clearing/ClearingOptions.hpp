#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

#include "Limits.hpp"

// The options change only whether a clear is refused or abandoned, never what it computes.
struct ClearingOptions {
    std::uint64_t maxArcs{ limits::MAX_ARCS_PER_BATCH };

    // The pivot sequence depends only on the input, so every replica stops at the same pivot.
    std::uint64_t maxPivots{ limits::MAX_CLEAR_PIVOTS };

    // Checks the whole spanning tree, the flow and the potentials after every pivot (the per-pivot
    // check only on small graphs). For tests: it is far slower. The cheap final optimality check
    // runs on every clear regardless.
    bool selfCheck{ false };
};

// The batch is too hard for the pivot budget. Every replica reaches this at the same pivot.
class ClearingBudgetExceeded : public std::runtime_error {
public:
    ClearingBudgetExceeded(std::uint64_t pivots, std::uint64_t arcs)
        : std::runtime_error("clear(): stopped after " + std::to_string(pivots) + " pivots on a graph of " +
                             std::to_string(arcs) + " arcs, over the pivot budget"),
          pivots_(pivots),
          arcs_(arcs) {}

    std::uint64_t pivots() const { return pivots_; }
    std::uint64_t arcs() const { return arcs_; }

private:
    std::uint64_t pivots_;
    std::uint64_t arcs_;
};
