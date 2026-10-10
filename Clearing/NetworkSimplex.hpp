#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ClearingOptions.hpp"
#include "LexCost.hpp"
#include "SimplexCore.hpp"

// The production flow solver: primal network simplex. The problem is written as a circulation: a
// return arc from the sink to the source costs (0, -V), where V is more than the secondary cost of
// any simple cycle (one more than the sum of the secondary costs of all the arcs). A unit sent through
// the market then earns V, which outranks every difference in rank, so the optimum maximizes welfare,
// then volume, then minimizes the rank sum.
//
// What a caller can rely on, beyond the Flow interface in ClearingImpl.hpp:
//   - Integer arithmetic only, so the optimum is exact.
//   - The result is a function of the arcs and the order they were added in. Which of several equally
//     good allocations comes out is decided by the pivot rule, so the solver's version is part of the
//     protocol: every replica must run the same code.
//   - Every solve ends by checking an optimality certificate over the whole flow, so a bug in the tree
//     bookkeeping throws std::logic_error instead of returning a wrong answer.
//   - A solve that needs more than ClearingOptions::maxPivots pivots throws ClearingBudgetExceeded.
//   - The price extraction is Dijkstra on reduced costs, so its work is O(arcs log nodes) whatever the
//     batch looks like.
class NetworkSimplex {
public:
    using NodeId = std::uint32_t;
    using ArcId = std::uint32_t;

    struct Stats {
        std::uint32_t augmentations{ 0 }; // pivots
        std::uint64_t unitsPushed{ 0 };   // units through the market: the flow on the return arc
        bool certified{ false };          // the optimality certificate passed (always, if the solve returns)
    };

    explicit NetworkSimplex(NodeId nodeCount, const ClearingOptions& options = {});

    // Unlike MinCostFlow, arcs may run between any two nodes: no numbering order is required.
    ArcId addArc(NodeId from, NodeId to, std::int64_t capacity, LexCost cost);

    std::size_t arcCount() const { return arcs_; }

    // Adds the return arc and pivots to the optimum. Runs once per instance.
    Stats solveWhileProfitable(NodeId source, NodeId sink);

    std::int64_t flowOn(ArcId arc) const { return core_.arc(arc).flow; }

    // The lowest potential that satisfies every residual arc with the anchors pinned at zero: minus the
    // shortest primary-cost distance to any anchor in the residual graph, not counting the return arc.
    // std::nullopt for a node that cannot reach an anchor. Needs a solved flow: throws std::logic_error
    // before the solve, or when a residual arc has a negative reduced cost (the flow was not optimal).
    std::vector<std::optional<std::int64_t>> lowestPotentials(std::span<const NodeId> anchors) const;

private:
    ClearingOptions options_;
    SimplexCore core_;
    NodeId nodes_;
    std::size_t arcs_{ 0 };
    std::int64_t sumSecondary_{ 0 };
    SimplexCore::ArcId returnArc_{ 0 };
    bool solved_{ false };
};
