#include "MinCostFlow.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

namespace {
// Above any real distance (Limits.hpp proves they stay below 2^62), and far enough below overflow to
// add one more arc cost.
constexpr std::int64_t INF = std::numeric_limits<std::int64_t>::max() / 4;
constexpr LexCost UNREACHED{ INF, 0 };
} // namespace

MinCostFlow::MinCostFlow(NodeId nodeCount) : adjacency_(nodeCount) {}

MinCostFlow::ArcId MinCostFlow::addArc(NodeId from, NodeId to, std::int64_t capacity,
                                       LexCost cost) {
    if (from >= adjacency_.size() || to >= adjacency_.size()) {
        throw std::invalid_argument("MinCostFlow::addArc: node out of range");
    }
    if (from >= to) {
        throw std::invalid_argument(
            "MinCostFlow::addArc: nodes must be numbered in topological order (from < to)");
    }
    if (capacity < 0) {
        throw std::invalid_argument("MinCostFlow::addArc: negative capacity");
    }
    const ArcId id = static_cast<ArcId>(edges_.size());
    edges_.push_back({ to, capacity, cost });
    edges_.push_back({ from, 0, -cost });
    adjacency_[from].push_back(id);
    adjacency_[to].push_back(id + 1);
    return id;
}

MinCostFlow::Stats MinCostFlow::solveWhileProfitable(NodeId source, NodeId sink) {
    if (solved_) {
        throw std::logic_error("MinCostFlow::solveWhileProfitable: already solved");
    }
    solved_ = true;

    const NodeId n = static_cast<NodeId>(adjacency_.size());
    if (source >= n || sink >= n || source >= sink) {
        throw std::invalid_argument("MinCostFlow: bad source or sink");
    }

    // Exact shortest distances from the source, in one pass because the graph is acyclic and numbered
    // in topological order. Nodes the source cannot reach keep potential zero: they are never visited,
    // and stay unreachable because augmenting only adds reverse arcs between nodes already reached.
    std::vector<LexCost> potential(n);
    {
        std::vector<LexCost> distance(n, UNREACHED);
        distance[source] = LexCost{};
        for (NodeId u = 0; u < n; ++u) {
            if (distance[u].primary == INF) continue;
            for (ArcId e : adjacency_[u]) {
                const Edge& edge = edges_[e];
                if (edge.capacity <= 0) continue;
                const LexCost candidate = distance[u] + edge.cost;
                if (candidate < distance[edge.to]) distance[edge.to] = candidate;
            }
        }
        for (NodeId v = 0; v < n; ++v) {
            if (distance[v].primary != INF) potential[v] = distance[v];
        }
    }

    Stats stats;
    std::vector<LexCost> distance(n);
    std::vector<ArcId> parent(n);
    std::vector<char> done(n);
    using HeapItem = std::pair<LexCost, NodeId>;
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<>> heap;

    while (true) {
        std::fill(distance.begin(), distance.end(), UNREACHED);
        std::fill(done.begin(), done.end(), char{ 0 });
        distance[source] = LexCost{};
        heap = {};
        heap.push({ distance[source], source });

        // Ties pop in node order, so the search is deterministic.
        while (!heap.empty()) {
            const auto [d, u] = heap.top();
            heap.pop();
            if (done[u]) continue;
            done[u] = 1;
            if (u == sink) break;
            for (ArcId e : adjacency_[u]) {
                const Edge& edge = edges_[e];
                if (edge.capacity <= 0 || done[edge.to]) continue;
                const LexCost candidate = d + edge.cost + potential[u] - potential[edge.to];
                if (candidate < distance[edge.to]) {
                    distance[edge.to] = candidate;
                    parent[edge.to] = e;
                    heap.push({ candidate, edge.to });
                }
            }
        }
        if (!done[sink]) break;

        const LexCost real = distance[sink] + potential[sink] - potential[source];
        if (real.primary > 0) break; // the next unit would lower welfare

        // For a search stopped at the sink, every node moves by min(its distance, the sink's), which
        // keeps every reduced cost non-negative.
        for (NodeId v = 0; v < n; ++v) {
            potential[v] = potential[v] + std::min(distance[v], distance[sink]);
        }

        std::int64_t push = std::numeric_limits<std::int64_t>::max();
        for (NodeId v = sink; v != source; v = edges_[parent[v] ^ 1u].to) {
            push = std::min(push, edges_[parent[v]].capacity);
        }
        for (NodeId v = sink; v != source; v = edges_[parent[v] ^ 1u].to) {
            edges_[parent[v]].capacity -= push;
            edges_[parent[v] ^ 1u].capacity += push;
        }
        ++stats.augmentations;
        stats.unitsPushed += static_cast<std::uint64_t>(push);
    }
    return stats;
}

std::vector<std::optional<std::int64_t>> MinCostFlow::lowestPotentials(
    std::span<const NodeId> anchors) const {
    const NodeId n = static_cast<NodeId>(adjacency_.size());
    std::vector<std::int64_t> distance(n, INF);
    std::vector<char> queued(n, 0);
    std::vector<std::uint32_t> pops(n, 0);
    std::deque<NodeId> queue;
    for (NodeId a : anchors) {
        if (a >= n) throw std::invalid_argument("MinCostFlow::lowestPotentials: bad anchor");
        distance[a] = 0;
        if (!queued[a]) {
            queued[a] = 1;
            queue.push_back(a);
        }
    }

    // SPFA on the reversed residual graph: distance[u] is the shortest primary cost of a residual path
    // from u to any anchor. The queue order follows the adjacency order, so the result is deterministic.
    while (!queue.empty()) {
        const NodeId v = queue.front();
        queue.pop_front();
        queued[v] = 0;
        // A node is only queued more than n times if a negative cycle keeps improving it.
        if (++pops[v] > n) {
            throw std::logic_error(
                "MinCostFlow::lowestPotentials: negative cycle in the residual graph");
        }
        for (ArcId e : adjacency_[v]) {
            const Edge& twin = edges_[e ^ 1u]; // the arc from edges_[e].to into v
            if (twin.capacity <= 0) continue;
            const NodeId u = edges_[e].to;
            const std::int64_t candidate = distance[v] + twin.cost.primary;
            if (candidate < distance[u]) {
                distance[u] = candidate;
                if (!queued[u]) {
                    queued[u] = 1;
                    queue.push_back(u);
                }
            }
        }
    }

    std::vector<std::optional<std::int64_t>> result(n);
    for (NodeId v = 0; v < n; ++v) {
        if (distance[v] != INF) result[v] = -distance[v];
    }
    return result;
}
