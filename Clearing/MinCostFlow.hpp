#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "LexCost.hpp"

// Integer min-cost flow by successive shortest paths with potentials. The result is a deterministic
// function of the arcs and their order: nothing depends on pointers, hash order, threads or the clock.
//
// Built for one graph shape, and it enforces what it relies on:
//   - Every arc runs from a lower node number to a higher one, so the starting potentials need one pass.
//   - solveWhileProfitable() runs once per instance.
class MinCostFlow {
public:
    using NodeId = std::uint32_t;
    using ArcId = std::uint32_t;

    struct Stats {
        std::uint32_t augmentations{ 0 };
        std::uint64_t unitsPushed{ 0 };
    };

    explicit MinCostFlow(NodeId nodeCount);

    // Also adds the zero-capacity reverse twin. Returns the forward arc's id.
    ArcId addArc(NodeId from, NodeId to, std::int64_t capacity, LexCost cost);

    std::size_t arcCount() const { return edges_.size() / 2; }

    // Stops when the cheapest path costs more than zero. A path costing exactly zero adds nothing to
    // welfare but moves more units, so among equally good flows this trades the most.
    Stats solveWhileProfitable(NodeId source, NodeId sink);

    // The flow on a forward arc is the residual capacity of its twin.
    std::int64_t flowOn(ArcId arc) const { return edges_[arc ^ 1u].capacity; }

    // The lowest potential that satisfies every residual arc with the anchors pinned at zero: minus the
    // shortest primary-cost distance to any anchor in the residual graph. A node that cannot reach an
    // anchor has no lower bound and gets std::nullopt. Throws std::logic_error on a negative cycle,
    // which means the flow was not optimal.
    std::vector<std::optional<std::int64_t>> lowestPotentials(
        std::span<const NodeId> anchors) const;

private:
    struct Edge {
        NodeId to;
        std::int64_t capacity; // residual capacity
        LexCost cost;
    };

    std::vector<Edge> edges_; // arc i and its twin i ^ 1 are stored together
    std::vector<std::vector<ArcId>> adjacency_;
    bool solved_{ false };
};
