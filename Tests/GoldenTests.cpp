#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <vector>

#include "BatchClearing.hpp"
#include "SyntheticBatch.hpp"

// Pins what the production solver returns for fixed inputs. Welfare, volume, the rank sum and the price of
// every item that trades are properties of the input, but WHICH of several equally good allocations comes
// out is decided by the solver's pivot rule, scan order, block size and starting tree. Every replica must
// return the same one, so a change to any of those is a change to the protocol, and this test makes it
// deliberate. If it fails:
//   - welfare, volume or tieCost differ: the solver is WRONG. Fix it.
//   - only the pivot count and the fingerprint differ: the allocation among equally good ones has
//     changed. If that was intended, update the numbers below and upgrade every replica together.
//
// The inputs come from the integer-only generator, so they are the same on every platform and compiler.

namespace {

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (8 * byte)) & 0xffu;
        hash *= 1099511628211ull; // FNV-1a
    }
    return hash;
}

std::uint64_t fingerprint(const ClearingResult& r) {
    std::uint64_t h = 14695981039346656037ull;
    h = mix(h, static_cast<std::uint64_t>(r.welfare));
    h = mix(h, r.tradedUnits);
    h = mix(h, static_cast<std::uint64_t>(r.tieCost));
    h = mix(h, 0xB1Du);
    for (const Fill& f : r.bidFills) h = mix(mix(mix(h, f.order), f.item), f.units);
    h = mix(h, 0xA5Cu);
    for (const Fill& f : r.askFills) h = mix(mix(mix(h, f.order), f.item), f.units);
    h = mix(h, 0x9F1Cu);
    for (const auto& price : r.itemPrice) h = mix(h, price ? static_cast<std::uint64_t>(*price) + 1 : 0);
    return h;
}

struct Golden {
    const char* name;
    synthetic::Params params;
    std::uint64_t seed;
    bool gappedRanks;           // give the orders explicit ranks with gaps, as cancels would leave
    std::int64_t welfare;
    std::uint64_t volume;
    std::int64_t tieCost;
    std::uint32_t pivots;       // the solver's pivot count: a fingerprint of the pivot rule
    std::uint64_t hash;
};

synthetic::Params shape(std::uint32_t items, std::size_t asks, std::size_t bids, std::uint32_t offer,
                        std::uint32_t accept, PriceTicks maxPrice) {
    synthetic::Params p;
    p.items = items;
    p.asks = asks;
    p.bids = bids;
    p.offerPerAsk = offer;
    p.acceptPerBid = accept;
    p.maxPrice = maxPrice;
    return p;
}

ClearingInput build(const Golden& g) {
    ClearingInput in = synthetic::makeBatch(g.params, g.seed);
    if (g.gappedRanks) {
        for (std::size_t b = 0; b < in.bids.size(); ++b) in.bidRank.push_back(static_cast<std::uint32_t>(3 * b + b % 2));
        for (std::size_t a = 0; a < in.asks.size(); ++a) in.askRank.push_back(static_cast<std::uint32_t>(2 * a + 1));
    }
    return in;
}

} // namespace

TEST(Golden, TheProductionSolverReturnsTheSameAllocationOnFixedBatches) {
    const std::vector<Golden> cases = {
        { "tiny, many ties", shape(6, 5, 12, 2, 3, 4), 1, false, 49, 20, 159, 62, 0xd17567b2fe3be2e8ull },
        { "medium", shape(40, 30, 120, 5, 8, 60), 2, false, 6753, 228, 16011, 1164, 0x6dc626c390ed991eull },
        { "dense", shape(20, 40, 60, 15, 15, 30), 3, false, 3890, 279, 14152, 1469, 0x12b724fad4ca0620ull },
        { "large", shape(300, 400, 800, 6, 10, 2000), 4, false, 2901352, 2759, 1653926, 15093, 0x3f462317927bda72ull },
        { "big and dense", shape(100, 600, 900, 40, 40, 500), 5, false, 1033669, 4121, 3079984, 29030, 0x58a907d95afb18eull },
        { "medium, ranks with gaps", shape(40, 30, 120, 5, 8, 60), 2, true, 6753, 228, 45390, 1164, 0x1d73131666bb6ddeull },
        { "one item", shape(1, 6, 20, 1, 1, 5), 6, false, 28, 11, 74, 44, 0xf1964fc89372e5d6ull },
    };
    for (const Golden& g : cases) {
        SCOPED_TRACE(g.name);
        const ClearingResult r = clear(build(g));
        const std::uint64_t hash = fingerprint(r);
        const bool same = r.welfare == g.welfare && r.tradedUnits == g.volume && r.tieCost == g.tieCost &&
                          r.stats.augmentations == g.pivots && hash == g.hash;
        EXPECT_EQ(r.welfare, g.welfare) << "welfare differs: the solver is wrong, or the generator changed";
        EXPECT_EQ(r.tradedUnits, g.volume) << "volume differs: the solver is wrong, or the generator changed";
        EXPECT_EQ(r.tieCost, g.tieCost) << "rank sum differs: the solver is wrong, or the generator changed";
        EXPECT_EQ(r.stats.augmentations, g.pivots)
            << "the pivot count changed: the pivot rule, the arc order or a limit changed";
        EXPECT_EQ(hash, g.hash)
            << "the allocation changed. If that was intended it is a protocol change.";
        if (!same) {
            std::ostringstream actual;
            actual << "{ " << r.welfare << ", " << r.tradedUnits << ", " << r.tieCost << ", " << r.stats.augmentations
                   << ", 0x" << std::hex << hash << "ull }";
            ADD_FAILURE() << "actual values for \"" << g.name << "\", in the order of the table: " << actual.str();
        }
    }
}
