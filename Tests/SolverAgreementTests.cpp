#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "BatchClearing.hpp"
#include "ClearingFixtures.hpp"
#include "ClearingInvariants.hpp"
#include "FixtureLoader.hpp"
#include "RealisticBatch.hpp"
#include "ResultCompare.hpp"
#include "SolverRegistry.hpp"
#include "SyntheticBatch.hpp"
#include "WindowBatch.hpp"

using namespace clearfix;

// Every solver against something that is not itself: the recorded answers of the independent Python
// oracle, the reference solver on bigger batches, and the reference solver on thousands of tiny random
// batches. Welfare, volume and the rank sum must be equal, and so must the price of every item that
// trades in both.

namespace {

using fixtures::FixtureCase;
using fixtures::expectPrices;
using fixtures::load;

void runFixtureFile(const SolverUnderTest& solver, const std::string& name, std::size_t expectedCases) {
    const auto cases = load(std::string(FIXTURE_DIR) + "/" + name);
    ASSERT_EQ(cases.size(), expectedCases) << "fixture file changed size: " << name;
    for (const FixtureCase& c : cases) {
        SCOPED_TRACE(c.name);
        ASSERT_EQ(c.prices.size(), c.input.itemCount);

        const ClearingResult r = solver.clear(c.input);
        invariants::expectValid(c.input, r);
        EXPECT_EQ(r.welfare, c.welfare);
        EXPECT_EQ(r.tradedUnits, c.volume);
        EXPECT_EQ(r.tieCost, c.ranksum) << "the arrival-order tie-break objective";
        expectPrices(r, c.prices);

        // A different arrival order changes who wins a tie, and so the rank sum, but not welfare, volume
        // or any price.
        for (const ClearingInput& shuffled : { reversed(c.input), rotated(c.input), rotated(reversed(c.input)) }) {
            const ClearingResult other = solver.clear(shuffled);
            EXPECT_EQ(other.welfare, c.welfare);
            EXPECT_EQ(other.tradedUnits, c.volume);
            expectPrices(other, c.prices);
            invariants::expectValid(shuffled, other);
        }
        if (::testing::Test::HasFailure()) return; // stop at the first bad case
    }
}

struct BigBatch {
    std::string name;
    ClearingInput in;
    ClearingResult reference;
    bool reorder; // also solve the batch in other arrival orders
};

// Bigger than the recorded cases (the largest has 77 orders), each solved once by the reference.
std::vector<BigBatch> bigBatches() {
    std::vector<BigBatch> batches;
    for (std::uint64_t seed = 1; seed <= 14; ++seed) {
        realistic::Params p;
        p.items = static_cast<std::uint32_t>(15 + seed * 9);
        p.bids = 60 + seed * 30;
        p.acceptPerBid = static_cast<std::uint32_t>(1 + seed % 6);
        p.offerPerAsk = static_cast<std::uint32_t>(1 + (seed * 3) % 6);
        ClearingInput in = realistic::makeBatch(p, seed);
        batches.push_back({ "realistic batch " + std::to_string(seed), in, clearReference(in), false });
    }
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        synthetic::Params p;
        p.items = static_cast<std::uint32_t>(6 + seed % 30);
        p.asks = 4 + seed % 25;
        p.bids = 20 + (seed * 11) % 120;
        p.offerPerAsk = static_cast<std::uint32_t>(1 + seed % 5);
        p.acceptPerBid = static_cast<std::uint32_t>(1 + seed % 9);
        p.maxPrice = static_cast<PriceTicks>(2 + seed % 6); // a tiny range: nearly every price ties
        ClearingInput in = synthetic::makeBatch(p, seed);
        batches.push_back({ "uniform batch " + std::to_string(seed) + " with many ties", in, clearReference(in), true });
    }
    // Small price ranges, so ties are everywhere.
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        window::Params p;
        p.groups = static_cast<std::uint32_t>(2 + seed % 4);
        p.slots = 12;
        p.asks = 40 + seed * 5;
        p.bids = 80 + seed * 10;
        p.askWindow = static_cast<std::uint32_t>(2 + seed % 5);
        p.bidWindow = static_cast<std::uint32_t>(1 + seed % 4);
        p.groupsPerBid = static_cast<std::uint32_t>(1 + seed % 3);
        p.maxPrice = static_cast<PriceTicks>(10 + 20 * (seed % 3));
        p.maxQuantity = 6;
        ClearingInput in = window::makeBatch(p, seed);
        batches.push_back({ "window batch " + std::to_string(seed), in, clearReference(in), false });
    }
    return batches;
}

} // namespace

TEST(Solvers, EverySolverMatchesTheIndependentOracleOnEveryFixtureAndInEveryArrivalOrder) {
    for (const SolverUnderTest& solver : allSolvers()) {
        SCOPED_TRACE(solver.name);
        runFixtureFile(solver, "small.inst", 150);
        runFixtureFile(solver, "medium.inst", 12);
        if (::testing::Test::HasFailure()) return;
    }
}

TEST(Solvers, EverySolverAgreesWithTheReferenceOnBatchesTooBigForTheOracleAndRepeatsItselfExactly) {
    const std::vector<BigBatch> batches = bigBatches();
    realistic::Params same;
    same.items = 60;
    same.bids = 300;
    same.acceptPerBid = 4;
    same.offerPerAsk = 3;
    const ClearingInput repeated = realistic::makeBatch(same, 99);

    for (const SolverUnderTest& solver : allSolvers()) {
        for (const BigBatch& b : batches) {
            SCOPED_TRACE(std::string(solver.name) + ": " + b.name);
            expectAgreement(b.in, b.reference, solver.clear(b.in));
            if (b.reorder) {
                for (const ClearingInput& variant : { reversed(b.in), rotated(b.in), rotated(rotated(b.in)) }) {
                    expectAgreement(variant, b.reference, solver.clear(variant), false);
                }
            }
            if (::testing::Test::HasFailure()) return;
        }
        // What two replicas running the same input must produce.
        SCOPED_TRACE(std::string(solver.name) + ": the same input again");
        const ClearingResult first = solver.clear(repeated);
        for (int run = 0; run < 3; ++run) EXPECT_TRUE(bitIdentical(first, solver.clear(repeated)));
    }
}

TEST(Solvers, TheProductionSolverAgreesWithTheReferenceOnThousandsOfTinyRandomBatches) {
    // Prices from a tiny range, so that ties are everywhere, and every corner is met: no orders on a side,
    // one item, curves of one to four steps, caps that bind, and explicit ranks with gaps about a third of
    // the time.
    synthetic::Rng rng(20261006);
    for (int trial = 0; trial < 4000; ++trial) {
        ClearingInput in;
        in.itemCount = static_cast<std::uint32_t>(1 + rng.below(6));
        const std::size_t asks = rng.below(6);
        const std::size_t bids = rng.below(8);
        auto make = [&](OrderRef id, bool isBid) {
            ClearingOrder o;
            o.id = id;
            o.perSlotCap = static_cast<NodeUnits>(1 + rng.below(4));
            o.stepCount = static_cast<std::uint8_t>(1 + rng.below(4));
            std::vector<PriceTicks> prices;
            for (std::size_t k = 0; k < o.stepCount; ++k) prices.push_back(static_cast<PriceTicks>(rng.below(7)));
            std::sort(prices.begin(), prices.end());
            if (isBid) std::reverse(prices.begin(), prices.end());
            for (std::size_t k = 0; k < o.stepCount; ++k) {
                o.steps[k] = { static_cast<NodeUnits>(1 + rng.below(4)), prices[k] };
            }
            for (ItemIndex item = 0; item < in.itemCount; ++item) {
                if (rng.below(2) == 0) o.items.push_back(item);
            }
            if (o.items.empty()) o.items.push_back(static_cast<ItemIndex>(rng.below(in.itemCount)));
            return o;
        };
        for (std::size_t a = 0; a < asks; ++a) in.asks.push_back(make(1 + a, false));
        for (std::size_t b = 0; b < bids; ++b) in.bids.push_back(make(1000 + b, true));
        if (rng.below(3) == 0) {
            std::uint32_t rank = static_cast<std::uint32_t>(rng.below(3));
            for (std::size_t b = 0; b < bids; ++b, rank += static_cast<std::uint32_t>(1 + rng.below(3))) in.bidRank.push_back(rank);
            rank = static_cast<std::uint32_t>(rng.below(3));
            for (std::size_t a = 0; a < asks; ++a, rank += static_cast<std::uint32_t>(1 + rng.below(3))) in.askRank.push_back(rank);
        }

        const ClearingResult got = clear(in);
        const ClearingResult want = clearReference(in);
        ASSERT_EQ(got.welfare, want.welfare) << "trial " << trial;
        ASSERT_EQ(got.tradedUnits, want.tradedUnits) << "trial " << trial;
        ASSERT_EQ(got.tieCost, want.tieCost) << "trial " << trial;
        for (std::size_t i = 0; i < got.itemPrice.size(); ++i) {
            if (got.itemPrice[i] && want.itemPrice[i]) {
                ASSERT_EQ(*got.itemPrice[i], *want.itemPrice[i]) << "trial " << trial << ", item " << i;
            }
        }
        invariants::expectValid(in, got);
        if (::testing::Test::HasFailure()) FAIL() << "trial " << trial;
    }
}
