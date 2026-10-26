#pragma once
#include <cstddef>
#include <cstdint>
#include "GpuTypes.hpp"

// The only place these numbers are written. Every replica must use the same values, because they
// decide which orders are accepted. The static_asserts at the bottom prove that every sum the
// clearing module forms fits in int64_t for the largest batch the limits allow, so a change that
// breaks that does not compile.
namespace limits {

inline constexpr std::size_t MAX_STEPS = 4;
inline constexpr std::size_t SLOT_WINDOW = 47;

// 2^20 - 1 ticks, which is $10,485.75 per node-hour at $0.01 a tick.
inline constexpr PriceTicks MAX_PRICE_TICKS = (1u << 20) - 1;

// Nodes one order may take or offer in a single slot.
inline constexpr NodeUnits MAX_PER_SLOT_CAP = 1'000;

inline constexpr NodeUnits MAX_ORDER_UNITS =
    static_cast<NodeUnits>(SLOT_WINDOW) * MAX_PER_SLOT_CAP;

inline constexpr std::size_t MAX_ORDERS_PER_SIDE = 10'000;

// Cancelled orders count, so a submit-and-cancel loop cannot grow ranks without bound. An
// order's rank is the number of orders its side accepted before it, so the largest is one less.
inline constexpr std::size_t MAX_SUBMISSIONS_PER_SIDE = 100'000;
inline constexpr std::uint32_t MAX_RANK = MAX_SUBMISSIONS_PER_SIDE - 1;

// An ask names one GPU, interconnect and region, so it offers at most SLOT_WINDOW items.
inline constexpr std::uint64_t MAX_ITEMS_PER_BATCH = MAX_ORDERS_PER_SIDE * SLOT_WINDOW;

// Source, sink, one per ask, one per item, one per bid.
inline constexpr std::uint64_t MAX_NODES =
    2 + MAX_ORDERS_PER_SIDE + MAX_ITEMS_PER_BATCH + MAX_ORDERS_PER_SIDE;

// An arc is one item an order lists, or one step of its curve. The order cap does not bound the
// clear, because a bid may accept any number of items: 10,000 bids can reach millions of arcs.
inline constexpr std::uint64_t MAX_ARCS_PER_BATCH = 1'000'000;

// Network simplex terminates but has no polynomial bound. The count depends only on the input, so
// every replica stops at the same pivot. It bounds work, not time.
inline constexpr std::uint64_t MAX_CLEAR_PIVOTS = 4'000'000;

// Per-buyer quotas, enforced on bids by the book. They are sized so that the quotas of all the
// buyers a batch can hold add up to no more than the batch limits, so a buyer inside their own
// quota never meets a full batch. An ask has at most SLOT_WINDOW item arcs and MAX_STEPS step
// arcs, so sellers need no quota: their arcs are reserved in full and the buyers share the rest.
inline constexpr std::size_t MAX_BUYERS_PER_BATCH = 500;
inline constexpr std::size_t MAX_OPEN_BIDS_PER_BUYER = MAX_ORDERS_PER_SIDE / MAX_BUYERS_PER_BATCH;
inline constexpr std::size_t MAX_SUBMISSIONS_PER_BUYER =
    MAX_SUBMISSIONS_PER_SIDE / MAX_BUYERS_PER_BATCH;
inline constexpr std::uint64_t MAX_ARCS_PER_ASK = SLOT_WINDOW + MAX_STEPS;
inline constexpr std::uint64_t MAX_ASK_ARCS = MAX_ORDERS_PER_SIDE * MAX_ARCS_PER_ASK;
inline constexpr std::uint64_t MAX_ARCS_PER_BUYER =
    (MAX_ARCS_PER_BATCH - MAX_ASK_ARCS) / MAX_BUYERS_PER_BATCH;

// The return arc from the sink to the source costs minus one more than the sum of the ranks on
// every arc, so a unit through the market earns more than any difference in rank: volume
// outranks the tie-break.
inline constexpr std::uint64_t MAX_RETURN_ARC_COST =
    MAX_ARCS_PER_BATCH * static_cast<std::uint64_t>(MAX_RANK) + 1;

// A potential is a sum of arc costs along a simple path in the tree. Such a path meets the source
// and the sink once each, so it holds at most four step arcs, and the return arc at most once.
inline constexpr std::uint64_t MAX_POTENTIAL =
    4 * (static_cast<std::uint64_t>(MAX_PRICE_TICKS) + MAX_RANK) + MAX_RETURN_ARC_COST;

// A shortest path never repeats a node, so it passes at most one step arc per order. The primary
// cost is a price and the secondary cost is a rank, and both sit on step arcs only.
inline constexpr std::uint64_t MAX_PATH_PRIMARY =
    2 * MAX_ORDERS_PER_SIDE * static_cast<std::uint64_t>(MAX_PRICE_TICKS);

inline constexpr std::uint64_t MAX_PATH_SECONDARY =
    2 * MAX_ORDERS_PER_SIDE * static_cast<std::uint64_t>(MAX_RANK);

// Every unit traded pays the rank of its bid and of its ask.
inline constexpr std::uint64_t MAX_TIE_COST =
    2 * MAX_ORDERS_PER_SIDE * static_cast<std::uint64_t>(MAX_ORDER_UNITS) * MAX_RANK;

// Welfare is bid value minus ask cost, so the bid value bounds it.
inline constexpr std::uint64_t MAX_BID_VALUE =
    MAX_ORDERS_PER_SIDE * static_cast<std::uint64_t>(MAX_ORDER_UNITS) * MAX_PRICE_TICKS;

inline constexpr std::uint64_t INT64_HEADROOM = 1ull << 62;

static_assert(MAX_STEPS >= 1, "a curve needs at least one step");
static_assert(MAX_SUBMISSIONS_PER_SIDE >= MAX_ORDERS_PER_SIDE,
              "an order that is live was also submitted, so the live cap cannot exceed the submission cap");
static_assert(MAX_PER_SLOT_CAP >= 1 && MAX_PRICE_TICKS >= 1, "limits must allow a trade");
static_assert(MAX_ORDERS_PER_SIDE <= UINT32_MAX / SLOT_WINDOW,
              "item and node indices are 32-bit");
static_assert(MAX_NODES < UINT32_MAX, "node ids are 32-bit");
static_assert(MAX_ORDERS_PER_SIDE * static_cast<std::uint64_t>(MAX_ORDER_UNITS) <=
                  INT64_HEADROOM / MAX_PRICE_TICKS,
              "total bid value must fit in int64_t");
static_assert(MAX_PATH_PRIMARY < INT64_HEADROOM, "a path's primary cost must fit in int64_t");
static_assert(MAX_PATH_SECONDARY < INT64_HEADROOM,
              "a path's secondary cost must fit in int64_t");
static_assert(MAX_TIE_COST < INT64_HEADROOM, "the tie-break objective must fit in int64_t");
static_assert(MAX_BID_VALUE < INT64_HEADROOM, "welfare must fit in int64_t");
static_assert(MAX_ARCS_PER_BATCH >= 2 * MAX_ORDERS_PER_SIDE,
              "every order has an item arc and a step arc, so the arc cap cannot be below the order cap");
static_assert(MAX_ARCS_PER_BATCH + MAX_NODES < (1ull << 31),
              "arc and node ids are 32-bit, and the price extraction packs an arc id and a direction into 32 bits");
static_assert(MAX_RETURN_ARC_COST < INT64_HEADROOM, "the return arc's cost must fit in int64_t");
static_assert(MAX_POTENTIAL < INT64_HEADROOM, "a potential must fit in int64_t");

static_assert(MAX_BUYERS_PER_BATCH * MAX_OPEN_BIDS_PER_BUYER <= MAX_ORDERS_PER_SIDE,
              "the buyers' open-bid quotas must fit in the order cap");
static_assert(MAX_BUYERS_PER_BATCH * MAX_SUBMISSIONS_PER_BUYER <= MAX_SUBMISSIONS_PER_SIDE,
              "the buyers' submission quotas must fit in the submission cap");
static_assert(MAX_ASK_ARCS + MAX_BUYERS_PER_BATCH * MAX_ARCS_PER_BUYER <= MAX_ARCS_PER_BATCH,
              "the asks' arcs and the buyers' arc quotas must fit in the arc cap");
static_assert(MAX_OPEN_BIDS_PER_BUYER >= 1 && MAX_SUBMISSIONS_PER_BUYER >= MAX_OPEN_BIDS_PER_BUYER,
              "a buyer must be able to hold an open bid, and to submit at least as many as are open");
static_assert(MAX_ARCS_PER_BUYER >= 2 * MAX_OPEN_BIDS_PER_BUYER,
              "every bid needs an item arc and a step arc, so a buyer's arc quota must cover its open bids");

} // namespace limits
