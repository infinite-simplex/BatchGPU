#pragma once
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "LexCost.hpp"

// The primal network simplex machinery, separated from the clearing problem: it knows nothing about
// bids, asks or the return arc. It solves a min-cost circulation. Every node has zero supply, so the
// all-zero flow is feasible, and the optimum is bounded when no negative cycle has infinite capacity.
//
// Node 0 is an artificial root. Every other node hangs from it by an artificial arc (cost zero,
// infinite capacity, zero flow, pointing toward the root), which makes the starting tree strongly
// feasible. Artificial arcs are never searched for pivots: they carry no flow in any optimum, and a
// common cost shifts every potential equally. optimize() resumes from wherever the tree is, so a call
// that stopped at its pivot budget can be continued by another.
//
// Pivot search. The entering arc is the best arc of the first block that holds an improving one. A block
// is a sample, not a run of consecutive arcs: the arcs are grouped in chunks of SEARCH_CHUNK, and chunks
// are visited in the order c -> (c + stride) mod chunks, with a stride coprime with the number of chunks
// (about 0.618 of it), so a block spreads over the whole arc array and every arc is reached once per
// pass. A block is a quarter of sqrt(arcs) arcs. Plain blocks of consecutive arcs covered only a few
// consecutive orders, which made the entering arc so local that dense batches needed up to 8 times more
// pivots.
//
// Determinism. Everything here is integer arithmetic and fixed-order scans, so the pivot sequence, and
// the flow that comes out, is a function of the arcs and the order they were added in. Any change to the
// pivot rule, the chunk or block size or the starting tree can change which of several equally good
// allocations comes out, so it is a change to the protocol. The tests pin the output of fixed batches
// to catch an accidental one.
class SimplexCore {
public:
    using NodeId = std::uint32_t;
    using ArcId = std::uint32_t;

    static constexpr std::int64_t INF = std::numeric_limits<std::int64_t>::max() / 4;
    static constexpr NodeId ROOT = 0;

    static constexpr std::int8_t UPPER = -1;
    static constexpr std::int8_t TREE = 0;
    static constexpr std::int8_t LOWER = 1;

    struct Arc {
        std::uint32_t from{};
        std::uint32_t to{};
        LexCost cost;
        std::int64_t cap{};
        std::int64_t flow{};
        std::int8_t state{};
        bool artificial{};
    };

    struct OptimizeResult {
        std::uint64_t pivots{ 0 }; // made by this call
        bool optimal{ false };     // false: stopped at the pivot budget, the flow is feasible but not optimal
    };

    SimplexCore();

    // Ids start at 1: 0 is the root.
    NodeId addNode();

    ArcId addArc(NodeId from, NodeId to, std::int64_t capacity, LexCost cost);

    // Pivots until no arc improves the flow, or maxPivots pivots have been made in this call. With
    // selfCheck, validates after every pivot (only on small graphs) and at the end. A failed check throws
    // std::logic_error.
    OptimizeResult optimize(std::uint64_t maxPivots = std::numeric_limits<std::uint64_t>::max(),
                            bool selfCheck = false);

    [[nodiscard]] const Arc& arc(const ArcId id) const { return arcs_[id]; }
    [[nodiscard]] std::size_t arcCount() const { return arcs_.size(); }
    [[nodiscard]] std::uint64_t totalPivots() const { return pivots_; }
    [[nodiscard]] const LexCost& potential(const NodeId node) const { return pi_[node]; }

    // The optimality certificate. Throws std::logic_error unless the flow respects every bound and
    // conserves flow at every node, and the potentials satisfy complementary slackness on every arc:
    // reduced cost non-negative where the flow is zero, non-positive where it is at capacity, zero in
    // between. Those conditions prove the flow optimal whatever the tree looks like, so a bug in the tree
    // bookkeeping cannot produce a wrong answer that passes. One pass over the arcs, valid only after
    // optimize() returned an optimal result. Returns true when it returns at all, so that a caller can
    // record that the check ran.
    [[nodiscard]] bool checkCertificate() const {
        verifyCertificate(arcs_, pi_);
        return true;
    }

    // The same check on plain data: `potentials` holds one value per node, the root first. Separate so
    // that the tests can hand it flows that are wrong, which the solver itself never produces.
    static void verifyCertificate(std::span<const Arc> arcs, std::span<const LexCost> potentials);

    // Stricter than the certificate: it also checks the tree itself, including strong feasibility.
    // Throws std::logic_error.
    void validate(bool optimal) const;

private:
    static constexpr std::uint32_t NONE = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t SEARCH_CHUNK = 128; // consecutive arcs scanned together
    static constexpr std::int8_t UP = 1;    // the tree arc runs from the child to its parent
    static constexpr std::int8_t DOWN = -1; // the tree arc runs from the parent to the child

    void addChild(NodeId parent, NodeId child);
    void removeChild(NodeId parent, NodeId child);
    bool findEntering(ArcId& out, std::uint32_t blockSize);
    void pivot(ArcId entering);

    std::vector<Arc> arcs_;
    std::vector<std::uint32_t> parent_;
    std::vector<std::uint32_t> predArc_;
    std::vector<std::int8_t> predDir_;
    std::vector<std::uint32_t> depth_;
    std::vector<std::uint32_t> firstChild_;
    std::vector<std::uint32_t> nextSibling_;
    std::vector<std::uint32_t> prevSibling_;
    std::vector<LexCost> pi_;
    std::uint32_t searchable_{ 0 }; // arcs that are not artificial
    std::uint32_t nextChunk_{ 0 };  // where the next pivot search starts: a chunk
    std::uint32_t nextOffset_{ 0 };  //   and an offset into it
    std::uint32_t stride_{ 0 };      // the step between visited chunks, valid for strideFor_ chunks
    std::uint32_t strideFor_{ 0 };
    std::uint64_t pivots_{ 0 };
};
