#pragma once
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "ClearingOptions.hpp"
#include "ClearingTypes.hpp"
#include "ClearingValidation.hpp"
#include "LexCost.hpp"

// The clearing algorithm, written once and parameterized by the flow solver: NetworkSimplex in
// production, MinCostFlow as the independent reference. The graph build, the fill extraction and the
// price extraction are shared, and only the solve differs.
//
// A Flow type provides:
//   using NodeId, ArcId;                         integer types
//   struct Stats { augmentations, unitsPushed }  (integer members)
//   explicit Flow(NodeId nodeCount);             or Flow(NodeId, const ClearingOptions&),
//                                                which is used when it exists
//   ArcId addArc(NodeId from, NodeId to, std::int64_t capacity, LexCost cost);
//   std::size_t arcCount() const;
//   Stats solveWhileProfitable(NodeId source, NodeId sink);
//   std::int64_t flowOn(ArcId) const;
//   std::vector<std::optional<std::int64_t>> lowestPotentials(span<const NodeId>) const;
// and numbers its nodes as given: every arc runs from a lower to a higher node.
//
// The flow graph:
//
//   source -> ask -> item -> bid -> sink
//
//   source -> ask   one arc per curve step.  capacity = step quantity,
//                   cost = ( + step price, the ask's arrival rank ).
//   ask    -> item  one arc per offered item. capacity = the ask's perSlotCap.
//   item   -> bid   one arc per accepted item. capacity = the bid's perSlotCap.
//   bid    -> sink  one arc per curve step.  capacity = step quantity,
//                   cost = ( - step price, the bid's arrival rank ).
//
// A monotone curve is a set of parallel arcs that fill from the best price down, so it needs no extra
// constraint.
template <typename Flow>
Flow makeFlow(typename Flow::NodeId nodeCount, const ClearingOptions& options) {
    if constexpr (std::is_constructible_v<Flow, typename Flow::NodeId, const ClearingOptions&>) {
        return Flow(nodeCount, options);
    } else {
        return Flow(nodeCount);
    }
}

template <typename Flow>
ClearingResult clearWith(const ClearingInput& input, const ClearingOptions& options = {}) {
    using Clock = std::chrono::steady_clock;
    using NodeId = Flow::NodeId;
    using ArcId = Flow::ArcId;
    auto microsSince = [](const Clock::time_point start) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    };

    validateInput(input, options);

    ClearingResult result;
    const std::size_t bidCount = input.bids.size();
    const std::size_t askCount = input.asks.size();

    auto askRank = [&](const std::size_t a) {
        return static_cast<std::int64_t>(input.askRank.empty() ? a : input.askRank[a]);
    };
    auto bidRank = [&](const std::size_t b) {
        return static_cast<std::int64_t>(input.bidRank.empty() ? b : input.bidRank[b]);
    };

    const NodeId source = 0;
    const NodeId firstAsk = 1;
    const NodeId firstItem = firstAsk + static_cast<NodeId>(askCount);
    const NodeId firstBid = firstItem + input.itemCount;
    const NodeId sink = firstBid + static_cast<NodeId>(bidCount);
    const NodeId nodeCount = sink + 1;

    const auto buildStart = Clock::now();
    Flow flow = makeFlow<Flow>(nodeCount, options);

    std::vector<std::vector<ArcId>> askStepArcs(askCount);
    std::vector<std::vector<ArcId>> askItemArcs(askCount);
    std::vector<std::vector<ArcId>> bidStepArcs(bidCount);
    std::vector<std::vector<ArcId>> bidItemArcs(bidCount);

    for (std::size_t a = 0; a < askCount; ++a) {
        const ClearingOrder& ask = input.asks[a];
        const NodeId node = firstAsk + static_cast<NodeId>(a);
        const std::int64_t rank = askRank(a);
        for (std::size_t k = 0; k < ask.stepCount; ++k) {
            const LexCost cost{ .primary = ask.steps[k].price, .secondary = rank };
            askStepArcs[a].push_back(flow.addArc(source, node, ask.steps[k].quantity, cost));
        }
        for (std::size_t p = 0; p < ask.items.size(); ++p) {
            askItemArcs[a].push_back(flow.addArc(node, firstItem + ask.items[p], ask.perSlotCap, LexCost{}));
        }
    }
    for (std::size_t b = 0; b < bidCount; ++b) {
        const ClearingOrder& bid = input.bids[b];
        const NodeId node = firstBid + static_cast<NodeId>(b);
        const std::int64_t rank = bidRank(b);
        for (std::size_t p = 0; p < bid.items.size(); ++p) {
            bidItemArcs[b].push_back(flow.addArc(firstItem + bid.items[p], node, bid.perSlotCap, LexCost{}));
        }
        for (std::size_t k = 0; k < bid.stepCount; ++k) {
            const LexCost cost{ .primary = -static_cast<std::int64_t>(bid.steps[k].price), .secondary = rank };
            bidStepArcs[b].push_back(flow.addArc(node, sink, bid.steps[k].quantity, cost));
        }
    }
    result.stats.buildMicros = microsSince(buildStart);
    result.stats.nodes = nodeCount;
    result.stats.arcs = flow.arcCount();

    const auto solveStart = Clock::now();
    const auto solved = flow.solveWhileProfitable(source, sink);
    result.stats.solveMicros = microsSince(solveStart);
    result.stats.augmentations = static_cast<std::uint32_t>(solved.augmentations);
    if constexpr (requires { solved.certified; }) result.stats.certified = solved.certified;

    result.itemUnits.assign(input.itemCount, 0);
    std::int64_t bidValue = 0;
    std::int64_t askCost = 0;
    std::int64_t tieCost = 0;
    for (std::size_t a = 0; a < askCount; ++a) {
        const ClearingOrder& ask = input.asks[a];
        for (std::size_t p = 0; p < ask.items.size(); ++p) {
            const std::int64_t units = flow.flowOn(askItemArcs[a][p]);
            if (units > 0) {
                result.askFills.push_back({
                    .order = ask.id,
                    .item = ask.items[p],
                    .units = static_cast<NodeUnits>(units)
                });
            }
        }
        for (std::size_t k = 0; k < ask.stepCount; ++k) {
            const std::int64_t units = flow.flowOn(askStepArcs[a][k]);
            askCost += units * static_cast<std::int64_t>(ask.steps[k].price);
            tieCost += units * askRank(a);
        }
    }
    for (std::size_t b = 0; b < bidCount; ++b) {
        const ClearingOrder& bid = input.bids[b];
        for (std::size_t p = 0; p < bid.items.size(); ++p) {
            const std::int64_t units = flow.flowOn(bidItemArcs[b][p]);
            if (units > 0) {
                result.bidFills.push_back({
                    .order = bid.id,
                    .item = bid.items[p],
                    .units = static_cast<NodeUnits>(units)
                });
                result.itemUnits[bid.items[p]] += static_cast<NodeUnits>(units);
            }
        }
        for (std::size_t k = 0; k < bid.stepCount; ++k) {
            const std::int64_t units = flow.flowOn(bidStepArcs[b][k]);
            bidValue += units * static_cast<std::int64_t>(bid.steps[k].price);
            tieCost += units * bidRank(b);
        }
    }
    result.welfare = bidValue - askCost;
    result.tieCost = tieCost;
    result.tradedUnits = static_cast<NodeUnits>(solved.unitsPushed);

    // Minimal prices: the lowest potentials that keep every residual arc feasible, with the source and
    // the sink pinned at zero. Only items that traded get one. The set of optimal prices does not depend
    // on which optimal flow was found, so the tie-break never moves them.
    const auto priceStart = Clock::now();
    const NodeId anchors[] = { source, sink };
    const auto potentials = flow.lowestPotentials(anchors);
    result.itemPrice.assign(input.itemCount, std::nullopt);
    for (ItemIndex i = 0; i < input.itemCount; ++i) {
        if (result.itemUnits[i] == 0) continue;
        const auto& potential = potentials[firstItem + i];
        // A traded item always has a residual path back to the source, so a missing or out-of-range
        // value means the flow was not optimal.
        if (!potential || *potential < 0 ||
            *potential > static_cast<std::int64_t>(limits::MAX_PRICE_TICKS)) {
            throw std::logic_error("clear(): item " + std::to_string(i) +
                                   " traded but has no valid minimal price");
        }
        result.itemPrice[i] = static_cast<PriceTicks>(*potential);
    }
    result.stats.priceMicros = microsSince(priceStart);
    return result;
}
