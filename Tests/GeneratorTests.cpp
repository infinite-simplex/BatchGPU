#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BatchClearing.hpp"
#include "ClearingInvariants.hpp"
#include "RealisticBatch.hpp"
#include "WindowBatch.hpp"

// Each band is the range ERCOT's day-ahead order books showed over five delivery days, widened a little
// for sampling noise. If a band fails, the generator drifted from the data it claims to match.

namespace {

struct SideStats {
    double meanSteps{ 0 };
    double tieShare{ 0 };       // orders whose first-step price another order also quotes
    double distinctRatio{ 0 };  // distinct first-step prices over orders
    double sizeP50{ 0 }, sizeP90{ 0 }, sizeP99{ 0 };
    double top1{ 0 }, top5{ 0 }, top10{ 0 }; // share of orders on the busiest 1, 5, 10 percent of items
    double tradeShare{ 0 };     // orders that trade at least partly
};

double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5)];
}

SideStats measure(const std::vector<ClearingOrder>& orders, const std::vector<Fill>& fills,
                  std::uint32_t itemCount) {
    SideStats s;
    double steps = 0;
    std::vector<double> sizes;
    std::unordered_map<PriceTicks, int> priceCount;
    std::unordered_map<ItemIndex, int> itemOrders;
    for (const ClearingOrder& o : orders) {
        steps += o.stepCount;
        std::uint64_t total = 0;
        for (std::size_t k = 0; k < o.stepCount; ++k) total += o.steps[k].quantity;
        sizes.push_back(static_cast<double>(total));
        ++priceCount[o.steps[0].price];
        for (ItemIndex item : o.items) ++itemOrders[item];
    }
    const double n = static_cast<double>(orders.size());
    s.meanSteps = steps / n;
    int tied = 0;
    for (const ClearingOrder& o : orders) tied += priceCount[o.steps[0].price] > 1 ? 1 : 0;
    s.tieShare = tied / n;
    s.distinctRatio = static_cast<double>(priceCount.size()) / n;
    s.sizeP50 = quantile(sizes, 0.5);
    s.sizeP90 = quantile(sizes, 0.9);
    s.sizeP99 = quantile(sizes, 0.99);

    std::vector<int> counts;
    for (const auto& [item, c] : itemOrders) counts.push_back(c);
    std::sort(counts.begin(), counts.end(), std::greater<>());
    double total = 0;
    for (int c : counts) total += c;
    auto topShare = [&](std::uint32_t denominator) {
        const std::size_t k = std::max<std::size_t>(1, itemCount / denominator);
        double sum = 0;
        for (std::size_t i = 0; i < std::min(k, counts.size()); ++i) sum += counts[i];
        return sum / total;
    };
    s.top1 = topShare(100);
    s.top5 = topShare(20);
    s.top10 = topShare(10);

    std::unordered_set<OrderRef> traded;
    for (const Fill& f : fills) traded.insert(f.order);
    s.tradeShare = static_cast<double>(traded.size()) / n;
    return s;
}

void print(const char* side, const SideStats& s) {
    std::printf("%s: steps %.2f  tie %.3f  distinct %.3f  size p50/p90/p99 %.1f/%.1f/%.1f  "
                "top1/5/10%% items %.3f/%.3f/%.3f  trade %.3f\n",
                side, s.meanSteps, s.tieShare, s.distinctRatio, s.sizeP50, s.sizeP90, s.sizeP99,
                s.top1, s.top5, s.top10, s.tradeShare);
}

bool sameOrders(const ClearingInput& x, const ClearingInput& y) {
    for (std::size_t i = 0; i < x.bids.size(); ++i) {
        if (x.bids[i].items != y.bids[i].items || x.bids[i].steps[0].price != y.bids[i].steps[0].price) return false;
    }
    for (std::size_t i = 0; i < x.asks.size(); ++i) {
        if (x.asks[i].items != y.asks[i].items || x.asks[i].steps[0].price != y.asks[i].steps[0].price) return false;
    }
    return true;
}

window::Params smallWindowParams() {
    window::Params p;
    p.groups = 6;
    p.slots = 20;
    p.asks = 60;
    p.bids = 90;
    p.askWindow = 7;
    p.bidWindow = 4;
    p.groupsPerBid = 3;
    return p;
}

} // namespace

TEST(Generators, RealisticBatchesMatchTheErcotBandsWhenOrdersNameOneItemAndStillClearWhenTheyNameMany) {
    // ERCOT orders each name one location, so this is the like-for-like setting. One hour of ERCOT holds
    // about 1,200 to 3,100 bids on 320 to 910 locations.
    realistic::Params p;
    p.bids = 2000;
    p.items = 500;
    p.acceptPerBid = 1;
    p.offerPerAsk = 1;
    const ClearingInput in = realistic::makeBatch(p, 20261005);
    const ClearingResult r = clear(in);
    invariants::expectValid(in, r);

    const SideStats bids = measure(in.bids, r.bidFills, in.itemCount);
    const SideStats asks = measure(in.asks, r.askFills, in.itemCount);
    print("bids", bids);
    print("asks", asks);

    // Steps per curve (ERCOT mean 1.3 to 2.2 for bids, 1.4 to 2.2 for offers).
    EXPECT_GE(bids.meanSteps, 1.25); EXPECT_LE(bids.meanSteps, 2.3);
    EXPECT_GE(asks.meanSteps, 1.30); EXPECT_LE(asks.meanSteps, 2.3);
    // Price ties (ERCOT 0.51 to 0.68 for bids, 0.57 to 0.73 for offers) and distinct prices (0.39 to 0.63).
    for (const SideStats* s : { &bids, &asks }) {
        EXPECT_GE(s->tieShare, 0.45); EXPECT_LE(s->tieShare, 0.78);
        EXPECT_GE(s->distinctRatio, 0.35); EXPECT_LE(s->distinctRatio, 0.68);
    }
    // Order size, bids (ERCOT p50 3.8 to 6.0, p90 55 to 250, p99 269 to 503).
    EXPECT_GE(bids.sizeP50, 3.0); EXPECT_LE(bids.sizeP50, 7.0);
    EXPECT_GE(bids.sizeP90, 40.0); EXPECT_LE(bids.sizeP90, 260.0);
    EXPECT_GE(bids.sizeP99, 200.0); EXPECT_LE(bids.sizeP99, 520.0);
    // Crowding on a few items (ERCOT top 1 percent 12 to 18, top 5 percent 25 to 45, top 10 percent 37 to 57).
    for (const SideStats* s : { &bids, &asks }) {
        EXPECT_GE(s->top1, 0.10); EXPECT_LE(s->top1, 0.22);
        EXPECT_GE(s->top5, 0.25); EXPECT_LE(s->top5, 0.50);
        EXPECT_GE(s->top10, 0.35); EXPECT_LE(s->top10, 0.60);
    }
    // Share of orders that trade at least partly (ERCOT bids 0.34 to 0.44, offers 0.56 to 0.72).
    EXPECT_GE(bids.tradeShare, 0.30); EXPECT_LE(bids.tradeShare, 0.50);
    EXPECT_GE(asks.tradeShare, 0.50); EXPECT_LE(asks.tradeShare, 0.75);

    // The compute-specific part is not calibrated: orders that accept several items. Check only that the
    // generator produces valid batches there, and that the same seed gives the same batch.
    for (std::uint32_t accept : { 5u, 20u }) {
        realistic::Params many;
        many.bids = 400;
        many.items = 100;
        many.acceptPerBid = accept;
        many.offerPerAsk = 5;
        const ClearingInput batch = realistic::makeBatch(many, 7 + accept);
        EXPECT_NO_THROW({
            const ClearingResult cleared = clear(batch);
            invariants::expectValid(batch, cleared);
        }) << "accept " << accept;
        EXPECT_TRUE(sameOrders(batch, realistic::makeBatch(many, 7 + accept)));
    }
}

TEST(Generators, WindowBatchesHaveTheItemStructureOfTheExchange) {
    const window::Params p = smallWindowParams();
    const ClearingInput in = window::makeBatch(p, 42);
    ASSERT_EQ(in.itemCount, p.groups * p.slots);
    ASSERT_EQ(in.asks.size(), p.asks);
    ASSERT_EQ(in.bids.size(), p.bids);

    // An ask offers consecutive slots of one group.
    for (const ClearingOrder& ask : in.asks) {
        ASSERT_EQ(ask.items.size(), p.askWindow);
        const std::uint32_t group = ask.items.front() / p.slots;
        for (std::size_t i = 0; i < ask.items.size(); ++i) {
            EXPECT_EQ(ask.items[i] / p.slots, group) << "an ask names one group";
            EXPECT_EQ(ask.items[i], ask.items.front() + i) << "consecutive slots";
        }
    }
    // A bid takes a window of consecutive slots in each of several groups, at the same hours in every group.
    for (const ClearingOrder& bid : in.bids) {
        ASSERT_EQ(bid.items.size(), p.groupsPerBid * p.bidWindow);
        std::set<std::uint32_t> groups;
        std::set<std::uint32_t> starts;
        for (const ItemIndex item : bid.items) groups.insert(item / p.slots);
        EXPECT_EQ(groups.size(), p.groupsPerBid) << "distinct groups";
        for (const std::uint32_t g : groups) {
            std::vector<std::uint32_t> slots;
            for (const ItemIndex item : bid.items) {
                if (item / p.slots == g) slots.push_back(item % p.slots);
            }
            ASSERT_EQ(slots.size(), p.bidWindow);
            EXPECT_TRUE(std::is_sorted(slots.begin(), slots.end()));
            EXPECT_EQ(slots.back() - slots.front() + 1, p.bidWindow) << "a window of consecutive slots";
            starts.insert(slots.front());
        }
        EXPECT_EQ(starts.size(), 1u) << "the same hours in every group";
    }

    // The same seed gives the same batch and another seed a different one.
    EXPECT_TRUE(sameOrders(window::makeBatch(p, 7), window::makeBatch(p, 7)));
    EXPECT_FALSE(sameOrders(window::makeBatch(p, 7), window::makeBatch(p, 8)));

    // Windows longer than the slot count are capped.
    window::Params wide = smallWindowParams();
    wide.askWindow = 99;
    wide.bidWindow = 99;
    wide.groupsPerBid = 99;
    const ClearingInput capped = window::makeBatch(wide, 1);
    EXPECT_EQ(capped.asks[0].items.size(), wide.slots);
    EXPECT_EQ(capped.bids[0].items.size(), wide.groups * wide.slots);
}
