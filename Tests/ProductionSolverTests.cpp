#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "BatchClearing.hpp"
#include "ClearingFixtures.hpp"
#include "ClearingInvariants.hpp"
#include "ClearingValidation.hpp"
#include "NetworkSimplex.hpp"
#include "RealisticBatch.hpp"
#include "ResultCompare.hpp"
#include "SimplexCore.hpp"
#include "SyntheticBatch.hpp"

using namespace clearfix;

namespace {

ClearingInput mediumBatch() {
    realistic::Params p;
    p.items = 40;
    p.bids = 200;
    p.acceptPerBid = 4;
    p.offerPerAsk = 3;
    return realistic::makeBatch(p, 7);
}

} // namespace

TEST(ProductionSolver, StopsAtItsPivotBudgetTheSameWayEveryTimeAndABudgetOfExactlyTheNeededPivotsSucceeds) {
    const ClearingInput in = mediumBatch();
    const ClearingResult full = clear(in);
    ASSERT_GT(full.stats.augmentations, 20u) << "the batch must need more pivots than the budget below";

    ClearingOptions tight;
    tight.maxPivots = 5;
    for (int run = 0; run < 3; ++run) {
        try {
            clear(in, tight);
            FAIL() << "a clear over its pivot budget must throw";
        } catch (const ClearingBudgetExceeded& e) {
            EXPECT_EQ(e.pivots(), 5u);
            EXPECT_EQ(e.arcs(), full.stats.arcs);
        }
    }

    ClearingOptions exact;
    exact.maxPivots = full.stats.augmentations;
    EXPECT_TRUE(bitIdentical(full, clear(in, exact)));
    ClearingOptions oneShort;
    oneShort.maxPivots = full.stats.augmentations - 1;
    EXPECT_THROW(clear(in, oneShort), ClearingBudgetExceeded);
}

TEST(ProductionSolver, GivesTheSameAnswerAndPivotsEveryTimeProvesItOptimalAndIsNotChangedByItsSelfCheck) {
    const ClearingInput in = mediumBatch();
    const ClearingResult first = clear(in);
    for (int run = 0; run < 3; ++run) {
        const ClearingResult again = clear(in);
        EXPECT_TRUE(bitIdentical(first, again));
        EXPECT_EQ(first.stats.augmentations, again.stats.augmentations);
    }

    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        synthetic::Params p;
        p.items = static_cast<std::uint32_t>(6 + seed % 12);
        p.asks = 4 + seed % 8;
        p.bids = 10 + seed % 20;
        p.offerPerAsk = static_cast<std::uint32_t>(1 + seed % 4);
        p.acceptPerBid = static_cast<std::uint32_t>(1 + seed % 5);
        p.maxPrice = static_cast<PriceTicks>(3 + seed % 20);
        const ClearingInput generated = synthetic::makeBatch(p, seed);
        ClearingOptions checked;
        checked.selfCheck = true;
        const ClearingResult plain = clear(generated);
        const ClearingResult withCheck = clear(generated, checked);
        EXPECT_TRUE(bitIdentical(plain, withCheck)) << "seed " << seed;
        EXPECT_EQ(plain.stats.augmentations, withCheck.stats.augmentations) << "seed " << seed;
    }

    // A result that came back has passed the certificate, and the statistics say so, which is how a test
    // can tell that the check is run at all.
    EXPECT_TRUE(first.stats.certified);
    ClearingInput empty;
    empty.itemCount = 3;
    EXPECT_TRUE(clear(empty).stats.certified);
    EXPECT_FALSE(clearReference(in).stats.certified);
}

TEST(ProductionSolver, ClearsTheLargestOrderTheLimitsAllowWithoutOverflow) {
    // One ask and one bid at the maximum quantity, per-slot cap and price, across a full slot window of items.
    constexpr std::uint32_t items = static_cast<std::uint32_t>(limits::SLOT_WINDOW);
    std::vector<ItemIndex> all;
    for (ItemIndex i = 0; i < items; ++i) all.push_back(i);

    ClearingOrder ask;
    ask.id = 1;
    ask.perSlotCap = limits::MAX_PER_SLOT_CAP;
    ask.stepCount = 1;
    ask.steps[0] = { limits::MAX_ORDER_UNITS, 0 };
    ask.items = all;

    ClearingOrder bid;
    bid.id = 2;
    bid.perSlotCap = limits::MAX_PER_SLOT_CAP;
    bid.stepCount = 1;
    bid.steps[0] = { limits::MAX_ORDER_UNITS, limits::MAX_PRICE_TICKS };
    bid.items = all;

    ClearingInput in;
    in.itemCount = items;
    in.bids = { bid };
    in.asks = { ask };

    const ClearingResult r = clear(in);
    invariants::expectValid(in, r);

    // Recompute the expected welfare in 128 bits, so the check itself cannot overflow.
    const unsigned __int128 expected = static_cast<unsigned __int128>(limits::MAX_ORDER_UNITS) * limits::MAX_PRICE_TICKS;
    EXPECT_TRUE(static_cast<unsigned __int128>(r.welfare) == expected)
        << "welfare " << r.welfare << " differs from MAX_ORDER_UNITS * MAX_PRICE_TICKS";
    EXPECT_EQ(r.tradedUnits, limits::MAX_ORDER_UNITS);
    for (ItemIndex i = 0; i < items; ++i) {
        ASSERT_TRUE(r.itemPrice[i].has_value());
        EXPECT_EQ(*r.itemPrice[i], 0u) << "the ask costs nothing, so the minimal price is zero";
    }
}

TEST(ProductionSolver, CountsTheArcsOfAnOrderAsTheGraphIsBuilt) {
    // Holds the engine's arc arithmetic (arcsOf) to the graph the clear really builds.
    for (std::uint32_t k = 1; k <= 3; ++k) {
        realistic::Params p;
        p.items = 60;
        p.bids = 150;
        p.acceptPerBid = 3 + 4 * k;
        p.offerPerAsk = 2 + k;
        const ClearingInput in = realistic::makeBatch(p, k);
        std::uint64_t counted = 0;
        for (const ClearingOrder& o : in.bids) counted += arcsOf(o);
        for (const ClearingOrder& o : in.asks) counted += arcsOf(o);
        ASSERT_GT(counted, 0u);
        EXPECT_EQ(clear(in).stats.arcs, counted) << "batch " << k;
        EXPECT_EQ(clearReference(in).stats.arcs, counted) << "batch " << k;
    }
}

TEST(SimplexCore, OptimizesCertifiesStopsAtItsBudgetAndRefusesPricesForAFlowThatIsNotOptimal) {
    {
        SCOPED_TRACE("the certificate accepts an optimal flow and rejects anything else");
        // A profitable cycle: arc `gain` earns 5 a unit (cost -5, room for 3), and the way back is free. The
        // best flow sends 3 round it.
        SimplexCore core;
        const auto a = core.addNode();
        const auto b = core.addNode();
        const auto gain = core.addArc(a, b, 3, LexCost{ -5, 0 });
        const auto back = core.addArc(b, a, SimplexCore::INF, LexCost{ 0, 0 });

        // The empty arc is worth filling, so the flow is not optimal yet.
        EXPECT_THROW(core.checkCertificate(), std::logic_error);

        const auto result = core.optimize();
        EXPECT_TRUE(result.optimal);
        EXPECT_EQ(core.arc(gain).flow, 3);
        EXPECT_EQ(core.arc(back).flow, 3);
        EXPECT_NO_THROW(core.checkCertificate());
        EXPECT_NO_THROW(core.validate(true));
    }
    {
        SCOPED_TRACE("a budget of zero pivots stops at once and leaves the flow feasible");
        SimplexCore core;
        const auto a = core.addNode();
        const auto b = core.addNode();
        core.addArc(a, b, 3, LexCost{ -5, 0 });
        core.addArc(b, a, SimplexCore::INF, LexCost{ 0, 0 });
        const auto stopped = core.optimize(0);
        EXPECT_FALSE(stopped.optimal);
        EXPECT_EQ(stopped.pivots, 0u);
        EXPECT_NO_THROW(core.validate(false)); // still a feasible flow on a valid tree

        const auto finished = core.optimize();
        EXPECT_TRUE(finished.optimal);
        EXPECT_NO_THROW(core.checkCertificate());
    }
    {
        SCOPED_TRACE("the same arcs give the same pivots and the same flow");
        auto build = [](SimplexCore& core) {
            constexpr int nodes = 40;
            for (int i = 0; i < nodes; ++i) core.addNode();
            std::uint64_t state = 12345;
            auto next = [&] {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                return static_cast<std::uint32_t>(state >> 33);
            };
            for (int k = 0; k < 300; ++k) {
                const auto from = 1 + next() % nodes;
                auto to = 1 + next() % nodes;
                if (to == from) to = 1 + (to % nodes);
                core.addArc(from, to, 1 + next() % 5, LexCost{ static_cast<std::int64_t>(next() % 21) - 10, static_cast<std::int64_t>(next() % 7) });
            }
        };
        SimplexCore first;
        build(first);
        ASSERT_TRUE(first.optimize().optimal);
        first.checkCertificate();
        for (int run = 0; run < 3; ++run) {
            SimplexCore again;
            build(again);
            ASSERT_TRUE(again.optimize().optimal);
            EXPECT_EQ(again.totalPivots(), first.totalPivots());
            ASSERT_EQ(again.arcCount(), first.arcCount());
            for (std::size_t j = 0; j < first.arcCount(); ++j) {
                EXPECT_EQ(again.arc(static_cast<SimplexCore::ArcId>(j)).flow, first.arc(static_cast<SimplexCore::ArcId>(j)).flow);
            }
        }
    }
    {
        SCOPED_TRACE("prices are refused for a flow that is not optimal");
        // Stopped before its first pivot, the flow is empty but the bid's arc to the sink is worth filling,
        // so the potentials do not make every residual arc's reduced cost non-negative, which the price
        // search relies on. It must say so, not answer.
        ClearingOptions none;
        none.maxPivots = 0;
        NetworkSimplex flow(4, none); // source 0, ask 1, bid 2, sink 3
        flow.addArc(0, 1, 5, LexCost{ 1, 0 });
        flow.addArc(1, 2, 5, LexCost{});
        flow.addArc(2, 3, 5, LexCost{ -9, 0 });
        EXPECT_THROW(flow.solveWhileProfitable(0, 3), ClearingBudgetExceeded);
        const NetworkSimplex::NodeId anchors[] = { 0, 3 };
        EXPECT_THROW(flow.lowestPotentials(anchors), std::logic_error);
        NetworkSimplex unsolved(4);
        EXPECT_THROW(unsolved.lowestPotentials(anchors), std::logic_error);
    }
}

namespace {

// Nodes 0 (the root), 1 and 2. Arc GAIN, 1 -> 2, earns 5 a unit and holds 3. Arc BACK, 2 -> 1, is free and
// unbounded. Sending 3 round the cycle is optimal, with every potential zero.
struct CertificateExample {
    static constexpr std::size_t GAIN = 2;
    static constexpr std::size_t BACK = 3;

    std::vector<SimplexCore::Arc> arcs;
    std::vector<LexCost> potentials{ LexCost{}, LexCost{}, LexCost{} };

    CertificateExample() {
        using Arc = SimplexCore::Arc;
        arcs = {
            Arc{ 1, 0, LexCost{}, SimplexCore::INF, 0, SimplexCore::TREE, true },
            Arc{ 2, 0, LexCost{}, SimplexCore::INF, 0, SimplexCore::TREE, true },
            Arc{ 1, 2, LexCost{ -5, 0 }, 3, 3, SimplexCore::UPPER, false },
            Arc{ 2, 1, LexCost{}, SimplexCore::INF, 3, SimplexCore::TREE, false },
        };
    }

    void check() const { SimplexCore::verifyCertificate(arcs, potentials); }
};

} // namespace

TEST(Certificate, AcceptsACorrectFlowAndRejectsEveryWayAFlowCanBeWrong) {
    // The solver never returns a flow that fails the certificate, so the data is built by hand: a correct
    // example, then the same example broken in one way at a time.
    using Example = CertificateExample;
    struct Case {
        const char* what;
        void (*spoil)(Example&);
        const char* message; // the part of the explanation that names the problem, or none if it must pass
    };
    const Case cases[] = {
        { "a correct flow", [](Example&) {}, nullptr },
        { "flow over capacity", [](Example& e) { e.arcs[Example::GAIN].flow = 4; }, "out of bounds" },
        { "negative flow", [](Example& e) { e.arcs[Example::BACK].flow = -1; }, "out of bounds" },
        { "flow that is not conserved: 3 leaves node 1 and only 2 comes back",
          [](Example& e) { e.arcs[Example::BACK].flow = 2; }, "not conserved" },
        { "flow on an artificial arc", [](Example& e) { e.arcs[0].flow = 1; }, "artificial" },
        { "an empty arc that is worth filling",
          [](Example& e) {
              e.arcs[Example::GAIN].flow = 0;
              e.arcs[Example::BACK].flow = 0;
          },
          "empty but has a negative" },
        { "a full arc that is not worth filling: it costs 5 a unit and carries its 3",
          [](Example& e) { e.arcs[Example::GAIN].cost = LexCost{ 5, 0 }; }, "full but has a positive" },
        { "a partly used arc with a reduced cost: it carries 3 of an unbounded 3",
          [](Example& e) { e.arcs[Example::BACK].cost = LexCost{ 1, 0 }; }, "partly used" },
        { "an empty arc with no price gain but a better rank is worth filling: the secondary cost counts",
          [](Example& e) {
              e.arcs[Example::GAIN].flow = 0;
              e.arcs[Example::BACK].flow = 0;
              e.arcs[Example::GAIN].cost = LexCost{ 0, -1 };
          },
          "empty but has a negative" },
        { "an empty arc with no price gain and a worse rank is not",
          [](Example& e) {
              e.arcs[Example::GAIN].flow = 0;
              e.arcs[Example::BACK].flow = 0;
              e.arcs[Example::GAIN].cost = LexCost{ 0, 1 };
          },
          nullptr },
        { "an arc that can carry nothing says nothing",
          [](Example& e) { e.arcs.push_back({ 1, 2, LexCost{ -100, 0 }, 0, 0, SimplexCore::LOWER, false }); }, nullptr },
        { "an arc with an end that is not a node", [](Example& e) { e.arcs[Example::GAIN].to = 9; }, "not a node" },
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        Example example;
        c.spoil(example);
        if (c.message == nullptr) {
            EXPECT_NO_THROW(example.check());
            continue;
        }
        try {
            example.check();
            ADD_FAILURE() << "the certificate accepted a flow it must reject";
        } catch (const std::logic_error& e) {
            EXPECT_NE(std::string(e.what()).find(c.message), std::string::npos) << e.what();
        }
    }
}
