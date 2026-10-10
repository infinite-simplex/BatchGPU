// Benchmark for the clearing solvers. Run it in a Release build:
//
//     ./build/ClearingBench --solver ns --maxprice 60 --one 500 200 1000 5 20
//     Bench/run_sweep.sh               the full sweep, each cell killed after 30 s
//
// ns is the production solver (clear()), ssp the slow independent reference (clearReference()).
// --max-arcs and --max-pivots override the production limits, so a batch the engine would refuse can
// still be timed.
//
// It prints CSV. Every batch is synthetic, so read the numbers as how a solver's cost grows, not as a
// forecast. The generators:
//   legacy     Support/SyntheticBatch.hpp: uniform random orders, a price range you choose.
//   realistic  Support/RealisticBatch.hpp: calibrated to ERCOT's day-ahead order books.
//   window     Support/WindowBatch.hpp: the item structure of this exchange.
//   hot, ties, chain, dense   batches built to be hard, below.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "BatchClearing.hpp"
#include "RealisticBatch.hpp"
#include "SyntheticBatch.hpp"
#include "WindowBatch.hpp"

namespace {

using ClearFn = ClearingResult (*)(const ClearingInput&);

struct SolverEntry {
    const char* name;
    ClearFn clear;
};

ClearingOptions g_options;

// Exit codes for a batch the production solver refuses. Bench/run_sweep.sh reads them.
constexpr int EXIT_REJECTED = 3; // validation: too many arcs, or any other bad input
constexpr int EXIT_OVER_PIVOT_BUDGET = 4;

// 0 keeps each generator's own fixed seed. --seed N draws a different batch of the same shape.
std::uint64_t g_seed = 0;

// --gen window: --one ITEMS counts groups of 47 hourly slots, so groups = ITEMS / 47.
bool g_window = false;
std::uint32_t g_bidWindow = 6;

ClearingResult clearProduction(const ClearingInput& input) {
    return clear(input, g_options);
}

const SolverEntry kSolvers[] = {
    { "ssp", &clearReference }, // the independent reference: successive shortest paths
    { "ns", &clearProduction }, // production: network simplex
};

struct Scenario {
    std::uint32_t items;
    std::size_t asks;
    std::size_t bids;
    std::uint32_t offerPerAsk;
    std::uint32_t acceptPerBid;
};

// --gen hot|ties|chain|dense: batches built to be hard for a solver, each meant to fill the
// order cap on both sides. The structure, not the realism, is the point:
//   hot    a handful of items that everybody wants, tiny orders, random prices. One
//          giant, tightly coupled market.
//   ties   every price equal, so every trade is a zero-surplus trade and only the
//          arrival ranks decide. The most degenerate input for a simplex method.
//   chain  bid i accepts items i and i+1, ask j offers item j, prices ramp. Long
//          alternating paths, the classic bad case for successive shortest paths.
//   dense  few items and wide accept sets: a large number of arcs.
// --one ITEMS ASKS BIDS OFFER_PER_ASK ACCEPT_PER_BID sizes the batch. Hot and chain fix their own
// items and items per order, so they use only ASKS and BIDS.
std::string g_hostile; // empty, or the shape above

ClearingInput makeHostile(const std::string& shape, const Scenario& s, std::uint64_t seed) {
    synthetic::Rng rng(seed);
    ClearingInput in;
    auto make = [&](OrderRef id, std::vector<ItemIndex> items, NodeUnits qty, PriceTicks price, NodeUnits cap) {
        ClearingOrder o;
        o.id = id;
        o.perSlotCap = cap;
        o.stepCount = 1;
        o.steps[0] = { qty, price };
        o.items = std::move(items);
        return o;
    };
    if (shape == "chain") {
        in.itemCount = static_cast<std::uint32_t>(std::max(s.bids, s.asks));
        for (std::size_t a = 0; a < s.asks; ++a) {
            in.asks.push_back(make(1 + a, { static_cast<ItemIndex>(a) }, 1, static_cast<PriceTicks>(a / 8), 1));
        }
        for (std::size_t b = 0; b < s.bids; ++b) {
            in.bids.push_back(make(1'000'000 + b,
                                   { static_cast<ItemIndex>(b), static_cast<ItemIndex>((b + 1) % in.itemCount) },
                                   1, static_cast<PriceTicks>(2000 - std::min<std::size_t>(b / 8, 1990)), 1));
        }
        return in;
    }
    const bool hot = shape == "hot";
    const bool ties = shape == "ties";
    in.itemCount = hot ? 4u : s.items;
    std::vector<char> used(in.itemCount, 0);
    auto price = [&](bool bid) -> PriceTicks {
        if (ties) return 1000;
        return static_cast<PriceTicks>(bid ? 1 + rng.below(2000) : rng.below(2000));
    };
    for (std::size_t a = 0; a < s.asks; ++a) {
        const std::uint32_t k = hot ? static_cast<std::uint32_t>(1 + rng.below(3)) : std::min<std::uint32_t>(s.offerPerAsk, limits::SLOT_WINDOW);
        const NodeUnits qty = hot ? static_cast<NodeUnits>(1 + rng.below(3)) : static_cast<NodeUnits>(1 + rng.below(ties ? 5 : 20));
        in.asks.push_back(make(1 + a, synthetic::pickItems(rng, in.itemCount, k, used), qty, price(false), qty));
    }
    for (std::size_t b = 0; b < s.bids; ++b) {
        const std::uint32_t k = hot ? static_cast<std::uint32_t>(1 + rng.below(3)) : s.acceptPerBid;
        const NodeUnits qty = hot ? static_cast<NodeUnits>(1 + rng.below(3)) : static_cast<NodeUnits>(1 + rng.below(ties ? 5 : 20));
        in.bids.push_back(make(1'000'000 + b, synthetic::pickItems(rng, in.itemCount, k, used), qty, price(true), qty));
    }
    return in;
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    const std::size_t i = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    return v[i];
}

ClearingInput makeInput(const Scenario& s, PriceTicks maxPrice, bool useRealistic) {
    if (!g_hostile.empty()) return makeHostile(g_hostile, s, g_seed ? g_seed : 20261005);
    if (g_window) {
        window::Params p;
        p.groups = std::max<std::uint32_t>(1, s.items / p.slots);
        p.asks = s.asks;
        p.bids = s.bids;
        p.askWindow = s.offerPerAsk;
        p.groupsPerBid = s.acceptPerBid;
        p.bidWindow = g_bidWindow;
        p.maxPrice = maxPrice;
        return window::makeBatch(p, g_seed ? g_seed : 20261006);
    }
    if (useRealistic) {
        realistic::Params p;
        p.items = s.items;
        p.bids = s.bids;
        p.asksPerBid = static_cast<double>(s.asks) / static_cast<double>(s.bids);
        p.offerPerAsk = s.offerPerAsk;
        p.acceptPerBid = s.acceptPerBid;
        return realistic::makeBatch(p, g_seed ? g_seed : 20261005);
    }
    synthetic::Params p;
    p.items = s.items;
    p.asks = s.asks;
    p.bids = s.bids;
    p.offerPerAsk = s.offerPerAsk;
    p.acceptPerBid = s.acceptPerBid;
    p.maxPrice = maxPrice;
    return synthetic::makeBatch(p, g_seed ? g_seed : 20261004);
}

void runScenario(const SolverEntry& solver, const Scenario& s, PriceTicks maxPrice, int runs,
                 bool useRealistic) {
    const ClearingInput input = makeInput(s, maxPrice, useRealistic);

    std::vector<double> total;
    std::vector<double> solve;
    std::vector<double> price;
    std::vector<double> build;
    ClearingResult last;
    for (int r = 0; r < runs; ++r) {
        const auto start = std::chrono::steady_clock::now();
        last = solver.clear(input);
        const auto stop = std::chrono::steady_clock::now();
        total.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        build.push_back(static_cast<double>(last.stats.buildMicros) / 1000.0);
        solve.push_back(static_cast<double>(last.stats.solveMicros) / 1000.0);
        price.push_back(static_cast<double>(last.stats.priceMicros) / 1000.0);
    }
    const std::string priceColumn = !g_hostile.empty() ? g_hostile : g_window ? "window"
                                  : useRealistic ? "realistic" : std::to_string(maxPrice);
    std::printf("%s,%s,%u,%zu,%zu,%u,%u,%llu,%llu,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f\n", solver.name,
                priceColumn.c_str(), s.items, s.asks, s.bids, s.offerPerAsk,
                s.acceptPerBid, static_cast<unsigned long long>(last.stats.nodes),
                static_cast<unsigned long long>(last.stats.arcs), last.stats.augmentations,
                last.tradedUnits, percentile(total, 0.5), percentile(total, 0.95),
                percentile(build, 0.5), percentile(solve, 0.5), percentile(price, 0.5));
    std::fflush(stdout);
}

void usage() {
    std::puts("usage: ClearingBench [--solver NAME] [--gen legacy|realistic|window|hot|ties|chain|dense]\n"
              "                     [--maxprice TICKS]\n"
              "                     [--window SLOTS]\n"
              "                     [--max-arcs N] [--max-pivots N] [--seed N] [--runs N] [--no-header]\n"
              "                     --one ITEMS ASKS BIDS OFFER_PER_ASK ACCEPT_PER_BID ...\n"
              "solvers:");
    for (const SolverEntry& s : kSolvers) std::printf("  %s\n", s.name);
}

} // namespace

int main(int argc, char** argv) {
    int runs = 3;
    bool header = true;
    PriceTicks maxPrice = 60;
    bool useRealistic = false;
    const SolverEntry* solver = &kSolvers[0];
    std::vector<Scenario> scenarios;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            usage();
            return 0;
        } else if (arg == "--runs" && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        } else if (arg == "--maxprice" && i + 1 < argc) {
            maxPrice = static_cast<PriceTicks>(std::atoi(argv[++i]));
        } else if (arg == "--window" && i + 1 < argc) {
            g_bidWindow = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--seed" && i + 1 < argc) {
            g_seed = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg == "--max-arcs" && i + 1 < argc) {
            g_options.maxArcs = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg == "--max-pivots" && i + 1 < argc) {
            g_options.maxPivots = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (arg == "--no-header") {
            header = false;
        } else if (arg == "--gen" && i + 1 < argc) {
            const std::string name = argv[++i];
            const bool hostile = name == "hot" || name == "ties" || name == "chain" || name == "dense";
            if (name != "legacy" && name != "realistic" && name != "window" && !hostile) {
                std::fprintf(stderr, "unknown generator: %s\n", name.c_str());
                return 1;
            }
            useRealistic = (name == "realistic");
            g_window = (name == "window");
            g_hostile = hostile ? name : "";
        } else if (arg == "--solver" && i + 1 < argc) {
            const std::string name = argv[++i];
            solver = nullptr;
            for (const SolverEntry& s : kSolvers) {
                if (name == s.name) solver = &s;
            }
            if (!solver) {
                std::fprintf(stderr, "unknown solver: %s\n", name.c_str());
                usage();
                return 1;
            }
        } else if (arg == "--one" && i + 5 < argc) {
            scenarios.push_back({ static_cast<std::uint32_t>(std::atoi(argv[i + 1])),
                                  static_cast<std::size_t>(std::atoi(argv[i + 2])),
                                  static_cast<std::size_t>(std::atoi(argv[i + 3])),
                                  static_cast<std::uint32_t>(std::atoi(argv[i + 4])),
                                  static_cast<std::uint32_t>(std::atoi(argv[i + 5])) });
            i += 5;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            usage();
            return 1;
        }
    }
    if (scenarios.empty()) {
        std::fprintf(stderr, "give at least one --one ITEMS ASKS BIDS OFFER ACCEPT\n");
        return 1;
    }
    if (header) {
        std::puts("solver,max_price,items,asks,bids,offer_per_ask,accept_per_bid,nodes,arcs,"
                  "steps,traded_units,clear_ms_median,clear_ms_p95,build_ms,solve_ms,price_ms");
    }
    // A sweep over hostile shapes should report a refused batch, not die on an uncaught exception.
    try {
        for (const Scenario& s : scenarios) runScenario(*solver, s, maxPrice, runs, useRealistic);
    } catch (const ClearingBudgetExceeded& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return EXIT_OVER_PIVOT_BUDGET;
    } catch (const std::invalid_argument& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return EXIT_REJECTED;
    }
    return 0;
}
