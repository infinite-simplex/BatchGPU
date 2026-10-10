#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "BatchClearing.hpp"
#include "ClearingFixtures.hpp"
#include "ClearingInvariants.hpp"
#include "ResultCompare.hpp"
#include "SolverRegistry.hpp"

using namespace clearfix;

namespace {

// Each expected value was worked out by hand before the code ran.
struct HandWorked {
    const char* name;
    ClearingInput in;
    NodeUnits units;
    std::int64_t welfare;
    std::vector<std::optional<PriceTicks>> prices; // one per item; none where nothing trades
    std::optional<std::vector<OrderRef>> bidFillOrders{}; // the order of each bid fill line, when the case says who
    std::vector<NodeUnits> itemUnits{};                    // units at each item, when the case says
};

std::vector<HandWorked> handWorkedCases() {
    const std::optional<PriceTicks> none;
    return {
        { .name = "a crossing bid and ask trade at the ask's price: supply is not scarce, so the minimal price is the ask's cost",
          .in = batch(1, { order(2, 5, { { 5, 20 } }, { 0 }) }, { order(1, 5, { { 5, 10 } }, { 0 }) }),
          .units = 5, .welfare = 5 * (20 - 10), .prices = { 10 }, .bidFillOrders = std::vector<OrderRef>{ 2 } },
        { .name = "no cross means no trade and no price",
          .in = batch(1, { order(2, 5, { { 5, 20 } }, { 0 }) }, { order(1, 5, { { 5, 30 } }, { 0 }) }),
          .units = 0, .welfare = 0, .prices = { none }, .bidFillOrders = std::vector<OrderRef>{} },
        { .name = "a bid that equals an ask trades: welfare is unchanged and volume rises",
          .in = batch(1, { order(2, 5, { { 5, 10 } }, { 0 }) }, { order(1, 5, { { 5, 10 } }, { 0 }) }),
          .units = 5, .welfare = 0, .prices = { 10 } },
        { .name = "scarce supply: the price is the highest losing bid",
          .in = batch(1, { order(2, 1, { { 1, 30 } }, { 0 }), order(3, 1, { { 1, 20 } }, { 0 }) },
                      { order(1, 1, { { 1, 10 } }, { 0 }) }),
          .units = 1, .welfare = 20, .prices = { 20 }, .bidFillOrders = std::vector<OrderRef>{ 2 } },
        { .name = "a per-slot cap forces a spread across items: 2 node-hours but 1 per item",
          .in = batch(2, { order(2, 1, { { 2, 20 } }, { 0, 1 }) }, { order(1, 1, { { 2, 10 } }, { 0, 1 }) }),
          .units = 2, .welfare = 20, .prices = { 10, 10 }, .itemUnits = { 1, 1 } },
        { .name = "a step curve is filled from the best price down: 2 at 40 and 2 at 30 against 4 at 25, priced where demand fits",
          .in = batch(1, { order(2, 10, { { 2, 40 }, { 3, 30 }, { 5, 20 } }, { 0 }) }, { order(1, 10, { { 4, 25 } }, { 0 }) }),
          .units = 4, .welfare = 2 * 40 + 2 * 30 - 4 * 25, .prices = { 30 } },
        { .name = "abundant supply: the price is the ask's cost",
          .in = batch(1, { order(2, 10, { { 2, 20 } }, { 0 }) }, { order(1, 10, { { 5, 10 } }, { 0 }) }),
          .units = 2, .welfare = 20, .prices = { 10 } },
        { .name = "an item that did not trade has no price",
          .in = batch(2, { order(3, 1, { { 1, 20 } }, { 0, 1 }) },
                      { order(1, 1, { { 1, 10 } }, { 0 }), order(2, 1, { { 1, 15 } }, { 1 }) }),
          .units = 1, .welfare = 10, .prices = { 10, none } },
        { .name = "each item keeps its own price",
          .in = batch(2, { order(3, 1, { { 2, 20 } }, { 0, 1 }) },
                      { order(1, 1, { { 1, 10 } }, { 0 }), order(2, 1, { { 1, 12 } }, { 1 }) }),
          .units = 2, .welfare = 18, .prices = { 10, 12 } },
        { .name = "rising ask steps stop where the bid stops: two cheap units at 10, then 30, against a bid of 3 at 20",
          .in = batch(1, { order(2, 10, { { 3, 20 } }, { 0 }) }, { order(1, 10, { { 2, 10 }, { 2, 30 } }, { 0 }) }),
          .units = 2, .welfare = 20, .prices = { 20 } },
        { .name = "an empty batch of no items", .in = batch(0, {}, {}), .units = 0, .welfare = 0, .prices = {} },
        { .name = "an empty batch of five items", .in = batch(5, {}, {}), .units = 0, .welfare = 0,
          .prices = { none, none, none, none, none } },
        { .name = "bids alone trade nothing",
          .in = batch(2, { order(1, 5, { { 5, 20 } }, { 0, 1 }), order(2, 5, { { 5, 30 } }, { 1 }) }, {}),
          .units = 0, .welfare = 0, .prices = { none, none } },
        { .name = "asks alone trade nothing", .in = batch(2, {}, { order(1, 5, { { 5, 10 } }, { 0, 1 }) }),
          .units = 0, .welfare = 0, .prices = { none, none } },
    };
}

} // namespace

TEST(Clearing, EverySolverGivesTheAnswersWorkedOutByHand) {
    const std::vector<HandWorked> cases = handWorkedCases();
    for (const SolverUnderTest& solver : allSolvers()) {
        for (const HandWorked& c : cases) {
            SCOPED_TRACE(std::string(solver.name) + ": " + c.name);
            const ClearingResult r = solver.clear(c.in);
            invariants::expectValid(c.in, r);
            EXPECT_EQ(r.tradedUnits, c.units);
            EXPECT_EQ(r.welfare, c.welfare);
            ASSERT_EQ(r.itemPrice.size(), c.in.itemCount);
            ASSERT_EQ(c.prices.size(), c.in.itemCount);
            for (std::size_t i = 0; i < c.prices.size(); ++i) EXPECT_EQ(r.itemPrice[i], c.prices[i]) << "item " << i;
            if (c.bidFillOrders) {
                std::vector<OrderRef> filled;
                for (const Fill& f : r.bidFills) filled.push_back(f.order);
                EXPECT_EQ(filled, *c.bidFillOrders);
            }
            if (!c.itemUnits.empty()) {
                EXPECT_EQ(r.itemUnits, c.itemUnits);
            }
            if (c.units == 0) {
                EXPECT_EQ(r.tieCost, 0);
                EXPECT_TRUE(r.bidFills.empty());
                EXPECT_TRUE(r.askFills.empty());
            }
        }
    }
}

TEST(Clearing, TiesGoToTheEarlierOrderAndARankGapOnlyChangesTheRankSum) {
    // Two identical bids compete for one unit; the bid listed first arrived first and wins. Price is
    // unaffected by who wins: the losing bid's 20 sets it.
    const ClearingInput twoBids = batch(1, { order(2, 1, { { 1, 20 } }, { 0 }), order(3, 1, { { 1, 20 } }, { 0 }) },
                                        { order(1, 1, { { 1, 10 } }, { 0 }) });
    // One bid wants a unit and two identical asks offer one each; the earlier ask sells.
    const ClearingInput twoAsks = batch(1, { order(3, 1, { { 1, 20 } }, { 0 }) },
                                        { order(1, 1, { { 1, 10 } }, { 0 }), order(2, 1, { { 1, 10 } }, { 0 }) });
    // Three equal bids for two units: the cap is 1 for each bid and 2 for the ask, so two different bids win.
    const ClearingInput threeBids = batch(1,
                                          { order(2, 1, { { 1, 20 } }, { 0 }), order(3, 1, { { 1, 20 } }, { 0 }),
                                            order(4, 1, { { 1, 20 } }, { 0 }) },
                                          { order(1, 2, { { 2, 10 } }, { 0 }) });
    // The first bid is too low to trade, so the second bid's rank, 1, is paid, plus the ask's rank, 0.
    const ClearingInput lowFirst = batch(1, { order(2, 1, { { 1, 5 } }, { 0 }), order(3, 1, { { 1, 20 } }, { 0 }) },
                                         { order(1, 1, { { 1, 10 } }, { 0 }) });
    for (const SolverUnderTest& solver : allSolvers()) {
        SCOPED_TRACE(solver.name);
        const ClearingInput tied = batch(1,
                                         { order(2, 1, { { 1, 20 } }, { 0 }), order(3, 1, { { 1, 20 } }, { 0 }),
                                           order(4, 1, { { 1, 20 } }, { 0 }) },
                                         { order(1, 1, { { 1, 10 } }, { 0 }) });
        const ClearingResult first = solver.clear(tied);
        for (int run = 0; run < 5; ++run) EXPECT_TRUE(bitIdentical(first, solver.clear(tied)));

        ClearingResult r = solver.clear(twoBids);
        invariants::expectValid(twoBids, r);
        ASSERT_EQ(r.bidFills.size(), 1u);
        EXPECT_EQ(r.bidFills[0].order, 2u);
        EXPECT_EQ(r.tradedUnits, 1u);
        EXPECT_EQ(r.welfare, 10);
        EXPECT_EQ(*r.itemPrice[0], 20u);
        EXPECT_EQ(r.tieCost, 0) << "ask rank 0 plus bid rank 0";
        ClearingResult swapped = solver.clear(reversed(twoBids));
        ASSERT_EQ(swapped.bidFills.size(), 1u);
        EXPECT_EQ(swapped.bidFills[0].order, 3u);
        EXPECT_EQ(swapped.welfare, 10);
        EXPECT_EQ(*swapped.itemPrice[0], 20u);

        r = solver.clear(twoAsks);
        invariants::expectValid(twoAsks, r);
        ASSERT_EQ(r.askFills.size(), 1u);
        EXPECT_EQ(r.askFills[0].order, 1u);
        EXPECT_EQ(r.tieCost, 0);
        swapped = solver.clear(reversed(twoAsks));
        ASSERT_EQ(swapped.askFills.size(), 1u);
        EXPECT_EQ(swapped.askFills[0].order, 2u);

        r = solver.clear(threeBids);
        invariants::expectValid(threeBids, r);
        std::set<OrderRef> winners;
        for (const Fill& f : r.bidFills) winners.insert(f.order);
        EXPECT_EQ(winners, (std::set<OrderRef>{ 2, 3 }));
        EXPECT_EQ(r.tieCost, 1) << "ask rank 0 twice (0), bid ranks 0 and 1 (1)";

        r = solver.clear(lowFirst);
        invariants::expectValid(lowFirst, r);
        ASSERT_EQ(r.bidFills.size(), 1u);
        EXPECT_EQ(r.bidFills[0].order, 3u);
        EXPECT_EQ(r.tieCost, 1);
    }

    // Bids 0..3 and 5..8 were submitted and cancelled, so the two live bids have ranks 4 and 9. The same
    // bid wins, and the rank sum counts the gap.
    ClearingInput gapped = batch(1, { order(11, 1, { { 1, 20 } }, { 0 }), order(12, 1, { { 1, 20 } }, { 0 }) },
                                 { order(1, 1, { { 1, 10 } }, { 0 }) });
    gapped.askRank = { 0 };
    gapped.bidRank = { 4, 9 };
    for (const auto solve : { static_cast<ClearingResult (*)(const ClearingInput&)>(&clear), &clearReference }) {
        const ClearingResult r = solve(gapped);
        ASSERT_EQ(r.bidFills.size(), 1u);
        EXPECT_EQ(r.bidFills[0].order, 11u);
        EXPECT_EQ(r.tieCost, 4);
    }
}

TEST(Clearing, RefusesInputTheHandlerShouldHaveCaughtAndInputOverItsLimits) {
    const ClearingOrder goodBid = order(2, 1, { { 1, 20 } }, { 0 });
    const ClearingOrder goodAsk = order(1, 1, { { 1, 10 } }, { 0 });
    for (const SolverUnderTest& solver : allSolvers()) {
        SCOPED_TRACE(solver.name);
        const auto refuses = [&](const ClearingInput& in, const char* what) {
            EXPECT_THROW(solver.clear(in), std::invalid_argument) << what;
        };
        refuses(batch(1, { order(2, 1, { { 1, 10 }, { 1, 20 } }, { 0 }) }, { goodAsk }), "a bid whose prices rise");
        refuses(batch(1, { goodBid }, { order(1, 1, { { 1, 20 }, { 1, 10 } }, { 0 }) }), "an ask whose prices fall");
        ClearingOrder noSteps = goodBid;
        noSteps.stepCount = 0;
        refuses(batch(1, { noSteps }, { goodAsk }), "no steps");
        refuses(batch(1, { order(2, 1, { { 0, 20 } }, { 0 }) }, { goodAsk }), "a zero quantity");
        refuses(batch(1, { order(2, 0, { { 1, 20 } }, { 0 }) }, { goodAsk }), "a zero cap");
        refuses(batch(1, { order(2, 1, { { 1, limits::MAX_PRICE_TICKS + 1 } }, { 0 }) }, { goodAsk }), "a price over the maximum");
        refuses(batch(1, { order(2, 1, { { 1, 20 } }, { 5 }) }, { goodAsk }), "an item index out of range");
        refuses(batch(1, { order(2, 1, { { 1, 20 } }, { 0, 0 }) }, { goodAsk }), "an item listed twice");
        refuses(batch(1, { order(2, 1, { { 1, 20 } }, {}) }, { goodAsk }), "no items");
        std::vector<ItemIndex> many;
        for (ItemIndex i = 0; i <= limits::SLOT_WINDOW; ++i) many.push_back(i);
        refuses(batch(static_cast<std::uint32_t>(many.size()), { goodBid }, { order(1, 1, { { 1, 10 } }, many) }),
                "an ask that offers more items than the slot window holds");
        EXPECT_NO_THROW(solver.clear(batch(1, { goodBid }, { goodAsk }))) << "the good input itself is accepted";
    }

    // The limits, on the production solver only.
    ClearingInput quantity = batch(1, { order(2, 1, { { 1, 2 } }, { 0 }) }, { order(1, 1, { { limits::MAX_ORDER_UNITS + 1, 1 } }, { 0 }) });
    EXPECT_THROW(clear(quantity), std::invalid_argument);
    quantity.asks[0].steps[0].quantity = limits::MAX_ORDER_UNITS;
    EXPECT_NO_THROW(clear(quantity));

    ClearingInput crowded;
    crowded.itemCount = 1;
    crowded.bids.assign(limits::MAX_ORDERS_PER_SIDE + 1, order(1, 1, { { 1, 1 } }, { 0 }));
    EXPECT_THROW(clear(crowded), std::invalid_argument) << "too many orders on one side";

    // Explicit ranks must be strictly increasing, one per order, and within the maximum.
    ClearingInput ranked = batch(1, { order(11, 1, { { 1, 20 } }, { 0 }), order(12, 1, { { 1, 20 } }, { 0 }) }, { goodAsk });
    ranked.bidRank = { 3, 3 };
    EXPECT_THROW(clear(ranked), std::invalid_argument);
    ranked.bidRank = { 3 };
    EXPECT_THROW(clear(ranked), std::invalid_argument);
    ranked.bidRank = { 0, limits::MAX_RANK + 1 };
    EXPECT_THROW(clear(ranked), std::invalid_argument);
    ranked.bidRank = { 0, limits::MAX_RANK };
    EXPECT_NO_THROW(clear(ranked));

    // The arc limit is checked before any work: one ask offering 3 items (1 step + 3 item arcs) and two bids
    // accepting 3 items each (3 item arcs + 1 step each) make 4 + 4 + 4 = 12 arcs.
    ClearingInput arcs = batch(3, { order(2, 2, { { 4, 30 } }, { 0, 1, 2 }), order(3, 2, { { 4, 20 } }, { 0, 1, 2 }) },
                               { order(1, 2, { { 4, 10 } }, { 0, 1, 2 }) });
    ClearingOptions exactly;
    exactly.maxArcs = 12;
    EXPECT_NO_THROW(clear(arcs, exactly));
    ClearingOptions oneShort;
    oneShort.maxArcs = 11;
    try {
        clear(arcs, oneShort);
        ADD_FAILURE() << "an input one arc over the limit must be rejected";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("too many arcs"), std::string::npos) << e.what();
    }
    // The default limit: three bids each accepting every one of 400,000 items.
    ClearingInput huge;
    huge.itemCount = 400'000;
    std::vector<ItemIndex> all(huge.itemCount);
    for (ItemIndex i = 0; i < huge.itemCount; ++i) all[i] = i;
    huge.asks = { order(1, 1, { { 1, 10 } }, { 0 }) };
    for (OrderRef id = 2; id <= 4; ++id) huge.bids.push_back(order(id, 1, { { 1, 30 } }, all));
    ASSERT_GT(3 * all.size(), limits::MAX_ARCS_PER_BATCH);
    EXPECT_THROW(clear(huge), std::invalid_argument);
}
