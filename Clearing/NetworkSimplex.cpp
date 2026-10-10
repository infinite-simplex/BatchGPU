#include "NetworkSimplex.hpp"

#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

namespace {

// Above any real distance (Limits.hpp proves they stay below 2^62), and far enough below overflow to
// add one more arc cost.
constexpr std::int64_t INF = std::numeric_limits<std::int64_t>::max() / 4;

// Keeps the sum of the secondary costs, which sets the return arc's cost, far from overflow however
// many arcs there are. A rank is at most limits::MAX_RANK, so this is generous.
constexpr std::int64_t MAX_ARC_SECONDARY = std::int64_t{ 1 } << 32;

} // namespace

NetworkSimplex::NetworkSimplex(NodeId nodeCount, const ClearingOptions& options)
    : options_(options), nodes_(nodeCount) {
    // The core numbers its nodes from 1 (0 is its artificial root), so this solver's node v is the
    // core's node v + 1.
    for (NodeId v = 0; v < nodeCount; ++v) core_.addNode();
}

NetworkSimplex::ArcId NetworkSimplex::addArc(NodeId from, NodeId to, std::int64_t capacity, LexCost cost) {
    if (from >= nodes_ || to >= nodes_) throw std::invalid_argument("NetworkSimplex::addArc: node out of range");
    if (from == to) throw std::invalid_argument("NetworkSimplex::addArc: an arc from a node to itself");
    if (capacity < 0) throw std::invalid_argument("NetworkSimplex::addArc: negative capacity");
    const std::int64_t secondary = cost.secondary < 0 ? -cost.secondary : cost.secondary;
    if (secondary > MAX_ARC_SECONDARY) throw std::invalid_argument("NetworkSimplex::addArc: secondary cost too large");
    sumSecondary_ += secondary;
    ++arcs_;
    return core_.addArc(from + 1, to + 1, capacity, cost);
}

NetworkSimplex::Stats NetworkSimplex::solveWhileProfitable(NodeId source, NodeId sink) {
    if (solved_) throw std::logic_error("NetworkSimplex::solveWhileProfitable: already solved");
    solved_ = true;
    if (source >= nodes_ || sink >= nodes_ || source == sink) {
        throw std::invalid_argument("NetworkSimplex: bad source or sink");
    }

    // Added last, so it is also the last arc a pivot search reaches.
    returnArc_ = core_.addArc(sink + 1, source + 1, SimplexCore::INF, LexCost{ 0, -(sumSecondary_ + 1) });

    const SimplexCore::OptimizeResult optimized = core_.optimize(options_.maxPivots, options_.selfCheck);
    if (!optimized.optimal) throw ClearingBudgetExceeded(optimized.pivots, arcs_);

    const bool certified = core_.checkCertificate();

    Stats stats;
    stats.augmentations = static_cast<std::uint32_t>(core_.totalPivots());
    stats.unitsPushed = static_cast<std::uint64_t>(core_.arc(returnArc_).flow);
    stats.certified = certified;
    return stats;
}

std::vector<std::optional<std::int64_t>> NetworkSimplex::lowestPotentials(
    std::span<const NodeId> anchors) const {
    if (!solved_) throw std::logic_error("NetworkSimplex::lowestPotentials: the flow has not been solved");
    const std::size_t n = nodes_;
    // The real arcs are the core's arcs from `first` up to the return arc.
    const SimplexCore::ArcId first = static_cast<SimplexCore::ArcId>(n);
    const SimplexCore::ArcId last = returnArc_;
    if (last >= (std::uint32_t{ 1 } << 31)) throw std::length_error("NetworkSimplex::lowestPotentials: too many arcs");

    // The reversed residual graph, in compressed rows. An entry in row v is a residual arc that ENDS
    // at v, stored as (arc id * 2 + direction): direction 0 is the arc itself (room left on it),
    // direction 1 its twin (flow that can be taken back).
    std::vector<std::uint32_t> start(n + 1, 0);
    for (SimplexCore::ArcId id = first; id < last; ++id) {
        const SimplexCore::Arc& a = core_.arc(id);
        // Row v is the core's node v + 1, and row v's count is kept in start[v + 1] until the prefix
        // sum, so the core's node id indexes it directly.
        if (a.flow < a.cap) ++start[a.to];
        if (a.flow > 0) ++start[a.from];
    }
    for (std::size_t v = 0; v < n; ++v) start[v + 1] += start[v];
    std::vector<std::uint32_t> entries(start[n]);
    {
        std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
        for (SimplexCore::ArcId id = first; id < last; ++id) {
            const SimplexCore::Arc& a = core_.arc(id);
            if (a.flow < a.cap) entries[fill[a.to - 1]++] = id * 2;
            if (a.flow > 0) entries[fill[a.from - 1]++] = id * 2 + 1;
        }
    }

    // Dijkstra on the reversed residual graph, which applies because the certificate has just proved
    // every residual arc's reduced cost non-negative. With
    //     reduced(u -> v) = cost(u -> v) + pi(u) - pi(v)  >= 0
    // a path's cost is its reduced cost plus pi(end) - pi(start). So label(v) = pi(v) + distance(v) obeys
    // label(u) = min over residual arcs u -> v of label(v) + reduced(u -> v), and an anchor, at distance 0,
    // starts at its own potential. Distances are unique, so the result does not depend on how ties leave
    // the heap.
    std::vector<std::int64_t> pi(n);
    for (NodeId v = 0; v < n; ++v) pi[v] = core_.potential(v + 1).primary;
    std::vector<std::int64_t> label(n, INF);
    using Entry = std::pair<std::int64_t, NodeId>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
    for (const NodeId a : anchors) {
        if (a >= n) throw std::invalid_argument("NetworkSimplex::lowestPotentials: bad anchor");
        if (pi[a] < label[a]) {
            label[a] = pi[a];
            heap.push({ label[a], a });
        }
    }
    while (!heap.empty()) {
        const auto [key, v] = heap.top();
        heap.pop();
        if (key != label[v]) continue;
        for (std::uint32_t e = start[v]; e < start[v + 1]; ++e) {
            const std::uint32_t entry = entries[e];
            const SimplexCore::Arc& a = core_.arc(entry >> 1);
            NodeId u;
            std::int64_t cost;
            if ((entry & 1u) == 0) {
                u = a.from - 1;
                cost = a.cost.primary;
            } else {
                u = a.to - 1;
                cost = -a.cost.primary;
            }
            const std::int64_t reduced = cost + pi[u] - pi[v];
            if (reduced < 0) {
                throw std::logic_error("NetworkSimplex::lowestPotentials: a residual arc has a negative reduced cost");
            }
            const std::int64_t candidate = key + reduced;
            if (candidate < label[u]) {
                label[u] = candidate;
                heap.push({ candidate, u });
            }
        }
    }

    std::vector<std::optional<std::int64_t>> result(n);
    for (NodeId v = 0; v < n; ++v) {
        if (label[v] != INF) result[v] = pi[v] - label[v]; // minus the distance
    }
    return result;
}
