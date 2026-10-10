#pragma once
#include <gtest/gtest.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ClearingTypes.hpp"

// Reads Tests/fixtures/*.inst: an input and the expected result for each case. The expected values were
// computed independently of this code by a Python oracle that has since been removed, so the files are
// frozen and nothing regenerates or checks them.
//
// The format, one case per block, whitespace separated, blank lines and lines starting with # ignored:
//
//     case <name>
//     items <N>
//     ask <ref> <cap> <nsteps> <q1> <p1> ... <qk> <pk> <nitems> <item> ...
//     bid <ref> <cap> <nsteps> <q1> <p1> ... <qk> <pk> <nitems> <item> ...
//     expect welfare <W>
//     expect volume <V>
//     expect ranksum <R>              the least total arrival rank among allocations with that welfare
//                                     and volume. Every traded unit pays the rank of its bid plus the
//                                     rank of its ask, a rank being the order's position in its side's
//                                     list. Its value is unique even when several allocations reach it.
//     expect prices <p0> ... <pN-1>   the minimal potential of every item, or "none" if it has no lower
//                                     bound. clear() reports a price only where units traded, and must
//                                     match this value there.
//     end
namespace fixtures {

struct FixtureCase {
    std::string name;
    ClearingInput input;
    std::int64_t welfare{ 0 };
    std::uint64_t volume{ 0 };
    std::int64_t ranksum{ 0 };
    std::vector<std::optional<PriceTicks>> prices;
};

inline ClearingOrder parseOrder(std::istringstream& line) {
    ClearingOrder o;
    unsigned cap = 0;
    unsigned stepCount = 0;
    line >> o.id >> cap >> stepCount;
    o.perSlotCap = cap;
    o.stepCount = static_cast<std::uint8_t>(stepCount);
    for (unsigned k = 0; k < stepCount; ++k) {
        unsigned q = 0;
        unsigned p = 0;
        line >> q >> p;
        o.steps[k] = { q, p };
    }
    std::size_t itemCount = 0;
    line >> itemCount;
    for (std::size_t i = 0; i < itemCount; ++i) {
        ItemIndex item = 0;
        line >> item;
        o.items.push_back(item);
    }
    return o;
}

inline std::vector<FixtureCase> load(const std::string& path) {
    std::ifstream file(path);
    EXPECT_TRUE(file.is_open()) << "cannot open " << path;
    std::vector<FixtureCase> cases;
    std::string text;
    FixtureCase current;
    while (std::getline(file, text)) {
        if (text.empty() || text[0] == '#') continue;
        std::istringstream line(text);
        std::string word;
        line >> word;
        if (word == "case") {
            current = FixtureCase{};
            line >> current.name;
        } else if (word == "items") {
            line >> current.input.itemCount;
        } else if (word == "ask") {
            current.input.asks.push_back(parseOrder(line));
        } else if (word == "bid") {
            current.input.bids.push_back(parseOrder(line));
        } else if (word == "expect") {
            std::string what;
            line >> what;
            if (what == "welfare") {
                line >> current.welfare;
            } else if (what == "volume") {
                line >> current.volume;
            } else if (what == "ranksum") {
                line >> current.ranksum;
            } else if (what == "prices") {
                std::string token;
                while (line >> token) {
                    if (token == "none") {
                        current.prices.push_back(std::nullopt);
                    } else {
                        current.prices.push_back(static_cast<PriceTicks>(std::stoul(token)));
                    }
                }
            }
        } else if (word == "end") {
            cases.push_back(current);
        }
    }
    return cases;
}

// The recorded price is the minimal potential of every item, but clear() reports one only for items that
// traded, and which items trade can depend on the tie-break. So where units traded the price must match,
// and where nothing traded there must be none.
inline void expectPrices(const ClearingResult& r, const std::vector<std::optional<PriceTicks>>& expected) {
    ASSERT_EQ(r.itemPrice.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (r.itemUnits[i] > 0) {
            ASSERT_TRUE(expected[i].has_value()) << "item " << i << " traded but the oracle has no price";
            EXPECT_EQ(r.itemPrice[i], expected[i]) << "item " << i;
        } else {
            EXPECT_FALSE(r.itemPrice[i].has_value()) << "item " << i << " has a price but did not trade";
        }
    }
}

} // namespace fixtures
