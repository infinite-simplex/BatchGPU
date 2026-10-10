#include "SimplexCore.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

// Integer, because the block size must be the same on every replica.
std::uint32_t integerSquareRoot(std::uint32_t n) {
    std::uint32_t root = 0;
    for (std::uint32_t bit = 1u << 15; bit != 0; bit >>= 1) {
        const std::uint32_t candidate = root | bit;
        if (static_cast<std::uint64_t>(candidate) * candidate <= n) root = candidate;
    }
    return root;
}

// About 0.618 of the way round (the golden ratio spreads consecutive visits as evenly as any step can),
// moved up to the nearest value coprime with `chunks`, so that the visits reach every chunk exactly once.
std::uint32_t coprimeStride(std::uint32_t chunks) {
    if (chunks <= 1) return 0;
    std::uint64_t g = static_cast<std::uint64_t>(chunks) * 618034 / 1000000;
    if (g < 1) g = 1;
    auto gcd = [](std::uint64_t a, std::uint64_t b) {
        while (b != 0) {
            const std::uint64_t t = a % b;
            a = b;
            b = t;
        }
        return a;
    };
    while (gcd(g, chunks) != 1) ++g;
    return static_cast<std::uint32_t>(g);
}

} // namespace

SimplexCore::SimplexCore() {
    parent_.push_back(NONE);
    predArc_.push_back(NONE);
    predDir_.push_back(UP);
    depth_.push_back(0);
    firstChild_.push_back(NONE);
    nextSibling_.push_back(NONE);
    prevSibling_.push_back(NONE);
    pi_.push_back(LexCost{});
}

SimplexCore::NodeId SimplexCore::addNode() {
    const NodeId node = static_cast<NodeId>(parent_.size());
    const ArcId artificial = static_cast<ArcId>(arcs_.size());
    arcs_.push_back({ node, ROOT, LexCost{}, INF, 0, TREE, true });
    parent_.push_back(ROOT);
    predArc_.push_back(artificial);
    predDir_.push_back(UP);
    depth_.push_back(1);
    firstChild_.push_back(NONE);
    nextSibling_.push_back(NONE);
    prevSibling_.push_back(NONE);
    pi_.push_back(pi_[ROOT]);
    addChild(ROOT, node);
    return node;
}

SimplexCore::ArcId SimplexCore::addArc(NodeId from, NodeId to, std::int64_t capacity, LexCost cost) {
    if (from >= parent_.size() || to >= parent_.size() || from == ROOT || to == ROOT) {
        throw std::invalid_argument("SimplexCore::addArc: bad node");
    }
    if (capacity < 0) throw std::invalid_argument("SimplexCore::addArc: negative capacity");
    if (arcs_.size() >= NONE - 1) throw std::length_error("SimplexCore::addArc: too many arcs");
    arcs_.push_back({ from, to, cost, capacity, 0, LOWER, false });
    ++searchable_;
    return static_cast<ArcId>(arcs_.size() - 1);
}

void SimplexCore::addChild(NodeId parent, NodeId child) {
    nextSibling_[child] = firstChild_[parent];
    prevSibling_[child] = NONE;
    if (firstChild_[parent] != NONE) prevSibling_[firstChild_[parent]] = child;
    firstChild_[parent] = child;
}

void SimplexCore::removeChild(NodeId parent, NodeId child) {
    if (prevSibling_[child] != NONE) {
        nextSibling_[prevSibling_[child]] = nextSibling_[child];
    } else {
        firstChild_[parent] = nextSibling_[child];
    }
    if (nextSibling_[child] != NONE) prevSibling_[nextSibling_[child]] = prevSibling_[child];
    nextSibling_[child] = prevSibling_[child] = NONE;
}

// Scans from where the last search stopped and takes the most improving arc of the first block that
// holds any. Artificial arcs are skipped and do not count toward a block.
bool SimplexCore::findEntering(ArcId& out, std::uint32_t blockSize) {
    LexCost best{};
    ArcId bestArc = NONE;
    std::uint32_t count = blockSize;
    const std::uint32_t total = static_cast<std::uint32_t>(arcs_.size());
    const std::uint32_t chunks = (total + SEARCH_CHUNK - 1) / SEARCH_CHUNK;
    if (strideFor_ != chunks) {
        strideFor_ = chunks;
        stride_ = coprimeStride(chunks);
    }
    std::uint32_t chunk = nextChunk_ < chunks ? nextChunk_ : 0;
    std::uint32_t offset = nextChunk_ < chunks ? nextOffset_ : 0;
    auto lengthOf = [&](std::uint32_t c) {
        return std::min<std::uint32_t>(SEARCH_CHUNK, total - c * SEARCH_CHUNK);
    };
    auto advance = [&] {
        offset = 0;
        chunk += stride_;
        if (chunk >= chunks) chunk -= chunks;
    };

    // chunks + 1 visits: the first may start part way through a chunk, so the last repeats the front of
    // that chunk, and every arc is looked at.
    bool found = false;
    for (std::uint32_t visit = 0; visit <= chunks && !found; ++visit) {
        const std::uint32_t base = chunk * SEARCH_CHUNK;
        const std::uint32_t length = lengthOf(chunk);
        for (; offset < length; ++offset) {
            const Arc& a = arcs_[base + offset];
            if (a.artificial) continue;
            if (a.state != TREE) {
                LexCost rc = a.cost + pi_[a.from] - pi_[a.to];
                if (a.state == UPPER) rc = -rc;
                if (rc < best) {
                    best = rc;
                    bestArc = base + offset;
                }
            }
            if (--count == 0) {
                if (bestArc != NONE) {
                    ++offset;
                    found = true;
                    break;
                }
                count = blockSize;
            }
        }
        if (!found) advance();
    }
    if (found && offset >= lengthOf(chunk)) advance();
    nextChunk_ = chunk;
    nextOffset_ = offset;
    out = bestArc;
    return bestArc != NONE;
}

void SimplexCore::pivot(ArcId entering) {
    Arc& in = arcs_[entering];
    const bool atLower = in.state == LOWER;
    // The cycle runs s -> t over the entering arc, in the direction that improves it, then back from t
    // to s through the tree.
    const NodeId s = atLower ? in.from : in.to;
    const NodeId t = atLower ? in.to : in.from;

    NodeId a = s;
    NodeId b = t;
    while (depth_[a] > depth_[b]) a = parent_[a];
    while (depth_[b] > depth_[a]) b = parent_[b];
    while (a != b) {
        a = parent_[a];
        b = parent_[b];
    }
    const NodeId join = a;

    // The leaving arc is the LAST blocking arc in the order join -> s -> t -> join, which keeps the tree
    // strongly feasible and so rules out cycling. Walking up from s, the first blocker met is the last in
    // that order, so a later one must be strictly smaller to replace it. Walking up from t, the last
    // blocker met is the last in that order, so a tie replaces.
    std::int64_t delta = in.cap;
    int result = 0; // 0: the entering arc itself, 1: on the s side, 2: on the t side
    NodeId uOut = NONE;
    for (NodeId w = s; w != join; w = parent_[w]) {
        const Arc& tree = arcs_[predArc_[w]];
        const std::int64_t room = predDir_[w] == DOWN ? tree.cap - tree.flow : tree.flow;
        if (room < delta) {
            delta = room;
            uOut = w;
            result = 1;
        }
    }
    for (NodeId w = t; w != join; w = parent_[w]) {
        const Arc& tree = arcs_[predArc_[w]];
        const std::int64_t room = predDir_[w] == UP ? tree.cap - tree.flow : tree.flow;
        if (room <= delta) {
            delta = room;
            uOut = w;
            result = 2;
        }
    }
    if (delta >= INF / 2) throw std::logic_error("SimplexCore: unbounded cycle");

    if (delta > 0) {
        in.flow += atLower ? delta : -delta;
        for (NodeId w = s; w != join; w = parent_[w]) {
            Arc& tree = arcs_[predArc_[w]];
            tree.flow += predDir_[w] == DOWN ? delta : -delta;
        }
        for (NodeId w = t; w != join; w = parent_[w]) {
            Arc& tree = arcs_[predArc_[w]];
            tree.flow += predDir_[w] == UP ? delta : -delta;
        }
    }

    if (result == 0) {
        in.state = static_cast<std::int8_t>(-in.state); // lower <-> upper, no tree change
        return;
    }

    // The part of the tree below the leaving arc is cut off and hung from the entering arc's other end,
    // re-rooted at the end of the entering arc it contains.
    const NodeId uIn = result == 1 ? s : t;
    const NodeId vIn = result == 1 ? t : s;
    Arc& out = arcs_[predArc_[uOut]];
    out.state = out.flow == 0 ? LOWER : UPPER;
    in.state = TREE;

    // The entering arc's reduced cost must become zero, so every node of the moved part shifts by it:
    // up by the reduced cost if the part holds the arc's head, down if it holds the tail.
    const LexCost rc = in.cost + pi_[in.from] - pi_[in.to];
    const LexCost shift = uIn == in.to ? rc : -rc;

    const NodeId vOut = parent_[uOut];
    removeChild(vOut, uOut);
    NodeId prev = vIn;
    ArcId prevArc = entering;
    std::int8_t prevDir = uIn == in.from ? UP : DOWN;
    NodeId w = uIn;
    while (true) {
        const NodeId oldParent = parent_[w];
        const ArcId oldArc = predArc_[w];
        const std::int8_t oldDir = predDir_[w];
        if (w != uOut) removeChild(oldParent, w);
        parent_[w] = prev;
        predArc_[w] = prevArc;
        predDir_[w] = prevDir;
        addChild(prev, w);
        if (w == uOut) break;
        // The old parent hangs below w now, over the same arc seen from the other end.
        prev = w;
        prevArc = oldArc;
        prevDir = static_cast<std::int8_t>(-oldDir);
        w = oldParent;
    }

    depth_[uIn] = depth_[vIn] + 1;
    pi_[uIn] = pi_[uIn] + shift;
    NodeId u = uIn;
    while (true) {
        const NodeId child = firstChild_[u];
        if (child != NONE) {
            u = child;
        } else {
            while (u != uIn && nextSibling_[u] == NONE) u = parent_[u];
            if (u == uIn) break;
            u = nextSibling_[u];
        }
        depth_[u] = depth_[parent_[u]] + 1;
        pi_[u] = pi_[u] + shift;
    }
}

SimplexCore::OptimizeResult SimplexCore::optimize(std::uint64_t maxPivots, bool selfCheck) {
    const std::uint32_t blockSize = std::max<std::uint32_t>(10, integerSquareRoot(searchable_) / 4);
    OptimizeResult result;
    ArcId entering = NONE;
    if (selfCheck) validate(false);
    while (findEntering(entering, blockSize)) {
        // An improving arc exists but the budget is spent: the flow is feasible, not optimal.
        if (result.pivots >= maxPivots) return result;
        pivot(entering);
        ++pivots_;
        ++result.pivots;
        if (selfCheck && parent_.size() <= 401) validate(false);
    }
    if (selfCheck) validate(true);
    result.optimal = true;
    return result;
}

void SimplexCore::verifyCertificate(std::span<const Arc> arcs, std::span<const LexCost> potentials) {
    auto fail = [](const std::string& what) {
        throw std::logic_error("SimplexCore: the final flow is not provably optimal: " + what);
    };
    std::vector<std::int64_t> balance(potentials.size(), 0);
    for (std::size_t j = 0; j < arcs.size(); ++j) {
        const Arc& a = arcs[j];
        if (a.from >= potentials.size() || a.to >= potentials.size()) {
            fail("arc " + std::to_string(j) + " has an end that is not a node");
        }
        if (a.flow < 0 || a.flow > a.cap) fail("flow out of bounds on arc " + std::to_string(j));
        balance[a.from] -= a.flow;
        balance[a.to] += a.flow;
        if (a.artificial) {
            if (a.flow != 0) fail("an artificial arc carries flow");
            continue;
        }
        if (a.cap == 0) continue; // an arc that can carry nothing says nothing about the potentials
        const LexCost rc = a.cost + potentials[a.from] - potentials[a.to];
        if (a.flow == 0) {
            if (rc < LexCost{}) fail("arc " + std::to_string(j) + " is empty but has a negative reduced cost");
        } else if (a.flow == a.cap) {
            if (rc > LexCost{}) fail("arc " + std::to_string(j) + " is full but has a positive reduced cost");
        } else if (rc != LexCost{}) {
            fail("arc " + std::to_string(j) + " is partly used but has a nonzero reduced cost");
        }
    }
    for (std::size_t u = 0; u < balance.size(); ++u) {
        if (balance[u] != 0) fail("flow is not conserved at node " + std::to_string(u));
    }
}

void SimplexCore::validate(bool optimal) const {
    auto fail = [](const std::string& what) { throw std::logic_error("SimplexCore self-check: " + what); };
    const std::size_t nodes = parent_.size();
    std::vector<std::int64_t> balance(nodes, 0);
    std::size_t treeArcs = 0;
    for (std::size_t j = 0; j < arcs_.size(); ++j) {
        const Arc& a = arcs_[j];
        if (a.flow < 0 || a.flow > a.cap) fail("flow out of bounds on arc " + std::to_string(j));
        if (a.state == LOWER && a.flow != 0) fail("lower arc with flow");
        if (a.state == UPPER && a.flow != a.cap) fail("upper arc below capacity");
        balance[a.from] -= a.flow;
        balance[a.to] += a.flow;
        if (a.state == TREE) ++treeArcs;
    }
    for (std::size_t u = 0; u < nodes; ++u) {
        if (balance[u] != 0) fail("flow not conserved at node " + std::to_string(u));
    }
    if (treeArcs != nodes - 1) {
        fail("tree has " + std::to_string(treeArcs) + " arcs, expected " + std::to_string(nodes - 1));
    }
    if (depth_[ROOT] != 0) fail("root depth");
    std::vector<std::uint32_t> children(nodes, 0);
    for (NodeId u = 1; u < nodes; ++u) {
        const NodeId p = parent_[u];
        if (p == NONE) fail("node without parent");
        if (depth_[u] != depth_[p] + 1) fail("depth mismatch at node " + std::to_string(u));
        const Arc& a = arcs_[predArc_[u]];
        if (a.state != TREE) fail("pred arc not in the tree");
        if (predDir_[u] == UP) {
            if (a.from != u || a.to != p) fail("UP pred arc misdirected");
        } else {
            if (a.from != p || a.to != u) fail("DOWN pred arc misdirected");
        }
        if (a.cost + pi_[a.from] - pi_[a.to] != LexCost{}) fail("tree arc with nonzero reduced cost");
        // Strong feasibility: positive flow can be sent from every node up to the root. An arc pointing
        // up must have room, an arc pointing down must carry flow to take back.
        if (predDir_[u] == UP && a.flow >= a.cap) fail("tree is not strongly feasible: a full upward arc");
        if (predDir_[u] == DOWN && a.flow <= 0) fail("tree is not strongly feasible: an empty downward arc");
        ++children[p];
    }
    for (NodeId p = 0; p < nodes; ++p) {
        std::uint32_t seen = 0;
        for (NodeId c = firstChild_[p]; c != NONE; c = nextSibling_[c]) {
            if (parent_[c] != p) fail("child list holds a node of another parent");
            ++seen;
        }
        if (seen != children[p]) fail("child list size mismatch at node " + std::to_string(p));
    }
    if (!optimal) return;
    for (const Arc& a : arcs_) {
        if (a.state == TREE || a.artificial) continue;
        const LexCost rc = a.cost + pi_[a.from] - pi_[a.to];
        if (a.state == LOWER && rc < LexCost{}) fail("lower arc with negative reduced cost");
        if (a.state == UPPER && rc > LexCost{}) fail("upper arc with positive reduced cost");
    }
}
