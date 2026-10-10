#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#include "BatchEngine.hpp"
#include "ClearingValidation.hpp"
#include "EngineFixtures.hpp"
#include "MatchingService.hpp" // Core's replica host
#include "OrderFixtures.hpp"
#include "SPSCProducerPolicy.hpp"

using namespace enginefix;

// The engine end to end through submit_order, with the policy Core's tests use: the events it emits and
// the traces it completes land in vectors. These are cases worked out by hand; EngineModelTests.cpp holds
// the engine to a second implementation on random commands.

template class BatchEngine<TestingPolicy<BatchEvent>>;
static_assert(std::is_same_v<decltype(&BatchEngine<TestingPolicy<BatchEvent>>::submit_order),
                             void (BatchEngine<TestingPolicy<BatchEvent>>::*)(BatchCommand&)>,
              "the contract of MatchingService");

namespace {

// Returned by value: the list is often a temporary, `only(h.order(o))`, and a reference into it would dangle.
BatchEvent only(const std::vector<BatchEvent>& events) {
    EXPECT_EQ(events.size(), 1u);
    return events.empty() ? BatchEvent{} : events.front();
}

// The bid takes A100 or H100 over NVLink in US_WEST for 10 hours, one node at a time, at 2400, 2200 and
// 2000 ticks. The ask offers H100 over NVLink in US_WEST for 4 hours, 4 nodes at once, at 1800 and 2000.
Order designBid() {
    Order o = bid(7, 1001, (std::uint64_t{ 1 } << 10) - 1, 1, { { 2, 2400 }, { 3, 2200 }, { 5, 2000 } }, A100 | H100);
    return o;
}
Order designAsk() { return ask(9, 2002, 0b1111, 4, { { 8, 1800 }, { 8, 2000 } }, H100); }

std::int32_t counter(const OrderTrace& trace, GpuExtSlot slot) { return trace.ext_counters[static_cast<std::size_t>(slot)]; }

} // namespace

TEST(Engine, PinsTheNumbersAndSizesThatClientsSeeInEvents) {
    // A client keeps these numbers. If one fails because it was changed on purpose, update it and tell
    // every client.
    const RejectReason reasons[] = {
        RejectReason::NONE,
        RejectReason::BAD_SIDE,
        RejectReason::EMPTY_MASK,
        RejectReason::MASK_BIT_OUT_OF_RANGE,
        RejectReason::ASK_MASK_NOT_SINGLE,
        RejectReason::PER_SLOT_CAP_OUT_OF_RANGE,
        RejectReason::STEP_COUNT_OUT_OF_RANGE,
        RejectReason::STEP_QUANTITY_ZERO,
        RejectReason::STEP_PRICE_OVER_MAX,
        RejectReason::STEP_QUANTITY_OVER_MAX,
        RejectReason::STEP_PRICE_ORDER,
        RejectReason::TOTAL_QUANTITY_OVER_MAX,
        RejectReason::TOKEN_IN_USE,
        RejectReason::SLOT_NOT_ELIGIBLE,
        RejectReason::PARTICIPANT_QUOTA,
        RejectReason::BATCH_FULL,
        RejectReason::UNKNOWN_ORDER,
    };
    const char* const names[] = { "NONE",
                                  "BAD_SIDE",
                                  "EMPTY_MASK",
                                  "MASK_BIT_OUT_OF_RANGE",
                                  "ASK_MASK_NOT_SINGLE",
                                  "PER_SLOT_CAP_OUT_OF_RANGE",
                                  "STEP_COUNT_OUT_OF_RANGE",
                                  "STEP_QUANTITY_ZERO",
                                  "STEP_PRICE_OVER_MAX",
                                  "STEP_QUANTITY_OVER_MAX",
                                  "STEP_PRICE_ORDER",
                                  "TOTAL_QUANTITY_OVER_MAX",
                                  "TOKEN_IN_USE",
                                  "SLOT_NOT_ELIGIBLE",
                                  "PARTICIPANT_QUOTA",
                                  "BATCH_FULL",
                                  "UNKNOWN_ORDER" };
    for (std::size_t i = 0; i < std::size(reasons); ++i) {
        EXPECT_EQ(static_cast<unsigned>(reasons[i]), i) << names[i];
        EXPECT_STREQ(toString(reasons[i]), names[i]);
    }
    EXPECT_STREQ(toString(static_cast<RejectReason>(200)), "?");

    EXPECT_EQ(static_cast<unsigned>(FullLimit::ORDERS), 0u);
    EXPECT_EQ(static_cast<unsigned>(FullLimit::SUBMISSIONS), 1u);
    EXPECT_EQ(static_cast<unsigned>(FullLimit::ARCS), 2u);
    EXPECT_EQ(static_cast<unsigned>(QuotaLimit::OPEN_ORDERS), 0u);
    EXPECT_EQ(static_cast<unsigned>(QuotaLimit::SUBMISSIONS), 1u);
    EXPECT_EQ(static_cast<unsigned>(QuotaLimit::ARCS), 2u);

    EXPECT_EQ(static_cast<unsigned>(EventType::ACCEPTED), 0u);
    EXPECT_EQ(static_cast<unsigned>(EventType::REJECTED), 1u);
    EXPECT_EQ(static_cast<unsigned>(EventType::CANCELED), 2u);
    EXPECT_EQ(static_cast<unsigned>(EventType::FILL), 3u);
    EXPECT_EQ(static_cast<unsigned>(EventType::ORDER_DONE), 4u);
    EXPECT_EQ(static_cast<unsigned>(EventType::BATCH_DONE), 5u);
    EXPECT_EQ(static_cast<unsigned>(EventType::BATCH_VOIDED), 6u);

    EXPECT_EQ(static_cast<unsigned>(VoidReason::NONE), 0u);
    EXPECT_EQ(static_cast<unsigned>(VoidReason::PIVOT_BUDGET), 1u);
    EXPECT_EQ(static_cast<unsigned>(VoidReason::REFUSED_INPUT), 2u);
    EXPECT_EQ(static_cast<unsigned>(VoidReason::SOLVER_ERROR), 3u);

    EXPECT_EQ(static_cast<unsigned>(GpuExtSlot::LiftedUnits), 0u);
    EXPECT_EQ(static_cast<unsigned>(GpuExtSlot::BidsFilled), 1u);
    EXPECT_EQ(static_cast<unsigned>(GpuExtSlot::BidsUnfilled), 2u);
    EXPECT_EQ(static_cast<unsigned>(GpuExtSlot::SolverPivots), 3u);

    EXPECT_LE(sizeof(BatchEvent), 128u);
    EXPECT_LE(sizeof(BatchCommand), 128u);
}

TEST(Engine, AnswersEveryOrderAndCancelWithTheEventThatNamesItsOwnerAndItsReason) {
    {
        SCOPED_TRACE("an accepted order is echoed and its trace is complete");
        Harness h;
        const auto events = h.order(designBid(), 31);
        const BatchEvent& e = only(events);
        EXPECT_EQ(e.type, EventType::ACCEPTED);
        EXPECT_EQ(e.sequence_number, 1u);
        EXPECT_EQ(e.batchId, 0u);
        EXPECT_EQ(e.participantId, 7u);
        EXPECT_EQ(e.orderToken, 1001u);
        EXPECT_EQ(e.session_id, 31u);
        EXPECT_EQ(e.side, Side::BID);
        EXPECT_EQ(e.rank, 0u);
        EXPECT_EQ(e.units, 10u) << "the order's whole quantity";
        EXPECT_EQ(e.rejectReason, RejectReason::NONE);
        EXPECT_EQ(h.traces.size(), 1u) << "an order is decided as far as the telemetry goes when it is let in";
        EXPECT_EQ(h.engine.book().openOrders(Side::BID), 1u);
    }
    {
        SCOPED_TRACE("an invalid command is refused with the reason the handler found and never touches the batch");
        Harness h;
        BatchCommand c = designBid().command();
        c.type = CommandType::INVALID;
        c.invalidReason = RejectReason::STEP_PRICE_ORDER;
        c.invalidDetail = 2;
        const auto events = h.send(c, 44);
        const BatchEvent& e = only(events);
        EXPECT_EQ(e.type, EventType::REJECTED);
        EXPECT_EQ(e.rejectReason, RejectReason::STEP_PRICE_ORDER);
        EXPECT_EQ(e.rejectDetail, 2u);
        EXPECT_EQ(e.participantId, 7u);
        EXPECT_EQ(e.orderToken, 1001u);
        EXPECT_EQ(e.session_id, 44u);
        EXPECT_EQ(e.side, Side::BID);
        EXPECT_EQ(e.batchId, 0u);
        EXPECT_EQ(h.traces.size(), 1u) << "a refused order is decided now, so its trace is complete";
        EXPECT_EQ(h.engine.book().submissions(Side::BID), 0u);
        EXPECT_EQ(h.engine.book().arcs(), 0u);
    }
    {
        SCOPED_TRACE("a side that was neither bid nor ask is echoed as the client sent it");
        Harness h;
        BatchCommand c = designBid().command();
        c.type = CommandType::INVALID;
        c.side = static_cast<Side>('Q');
        c.invalidReason = RejectReason::BAD_SIDE;
        c.invalidDetail = 'Q';
        const BatchEvent e = only(h.send(c));
        EXPECT_EQ(e.type, EventType::REJECTED);
        EXPECT_EQ(static_cast<unsigned>(e.side), static_cast<unsigned>('Q'));
        EXPECT_EQ(e.rejectDetail, static_cast<unsigned>('Q'));
    }
    {
        SCOPED_TRACE("an order that reached the engine with a stateless problem is refused as the handler would");
        Harness h;
        BatchCommand c = designBid().command();
        c.regionMask = 0b1000; // a region beyond the catalog
        const BatchEvent e = only(h.send(c));
        EXPECT_EQ(e.type, EventType::REJECTED);
        EXPECT_EQ(e.rejectReason, RejectReason::MASK_BIT_OUT_OF_RANGE);
        EXPECT_EQ(e.rejectDetail, maskDetail(catalog::Attribute::REGION, 3));
        EXPECT_EQ(h.engine.book().submissions(Side::BID), 0u);
        EXPECT_EQ(h.traces.size(), 1u);
        h.order(designAsk());
        EXPECT_EQ(h.clearBatch(0).back().type, EventType::BATCH_DONE);
    }
    {
        SCOPED_TRACE("an order for an hour that cannot be sold is refused and the event names the hour");
        Harness h; // batch 0 sells hours 1001 to 1047
        Order o = designBid();
        o.baseHour = H0 - 3;
        o.slots = 0b11101; // hours 998, 1000, 1001, 1002: the lowest that cannot be sold is bit 0, hour 998
        BatchEvent e = only(h.order(o));
        EXPECT_EQ(e.type, EventType::REJECTED);
        EXPECT_EQ(e.rejectReason, RejectReason::SLOT_NOT_ELIGIBLE);
        EXPECT_EQ(e.rejectDetail, 0u);
        EXPECT_EQ(e.hour, 998u);
        o.slots = 0b11100; // bit 2 is hour 1000
        e = only(h.order(o));
        EXPECT_EQ(e.rejectDetail, 2u);
        EXPECT_EQ(e.hour, 1000u);
        o.baseHour = H0 + 45;
        o.slots = 0b1111; // 1046 and 1047 are fine, 1048 is not
        e = only(h.order(o));
        EXPECT_EQ(e.rejectDetail, 2u);
        EXPECT_EQ(e.hour, 1048u);
        EXPECT_EQ(e.batchId, 0u) << "checked against batch 0";
        EXPECT_EQ(h.engine.book().submissions(Side::BID), 0u);
    }
    {
        SCOPED_TRACE("the hour in a reject stops at what an event can hold");
        Harness h;
        Order o = designBid();
        o.baseHour = std::numeric_limits<HourIndex>::max();
        o.slots = 0b111;
        const BatchEvent e = only(h.order(o));
        EXPECT_EQ(e.rejectReason, RejectReason::SLOT_NOT_ELIGIBLE);
        EXPECT_EQ(e.rejectDetail, 0u);
        EXPECT_EQ(e.hour, std::numeric_limits<HourIndex>::max());
        // The lowest bad bit is bit 2 of this one, and baseHour + 2 does not fit 32 bits.
        Order top = designBid();
        top.token = 2;
        top.baseHour = std::numeric_limits<HourIndex>::max();
        top.slots = 0b100;
        const BatchEvent f = only(h.order(top));
        EXPECT_EQ(f.rejectDetail, 2u);
        EXPECT_EQ(f.hour, std::numeric_limits<HourIndex>::max()) << "stopped, not wrapped to 1";
    }
    {
        SCOPED_TRACE("a token in use is refused whatever the side, and free again after a cancel");
        Harness h;
        ASSERT_EQ(only(h.order(designBid())).type, EventType::ACCEPTED);
        const BatchEvent again = only(h.order(designBid()));
        EXPECT_EQ(again.type, EventType::REJECTED);
        EXPECT_EQ(again.rejectReason, RejectReason::TOKEN_IN_USE);
        EXPECT_EQ(again.rejectDetail, 0u);
        Order ask7 = designAsk();
        ask7.participant = 7;
        ask7.token = 1001;
        EXPECT_EQ(only(h.order(ask7)).rejectReason, RejectReason::TOKEN_IN_USE) << "an ask with the bid's token";
        ASSERT_EQ(only(h.cancel(7, 1001)).type, EventType::CANCELED);
        const BatchEvent third = only(h.order(designBid()));
        EXPECT_EQ(third.type, EventType::ACCEPTED);
        EXPECT_EQ(third.rank, 1u) << "a new submission";
    }
    {
        SCOPED_TRACE("a cancel removes the order and tells the order's own session");
        Harness h;
        ASSERT_EQ(only(h.order(designBid(), 31)).type, EventType::ACCEPTED);
        EXPECT_EQ(h.traces.size(), 1u);
        const BatchEvent e = only(h.cancel(7, 1001, 55)); // sent from another connection
        EXPECT_EQ(e.type, EventType::CANCELED);
        EXPECT_EQ(e.participantId, 7u);
        EXPECT_EQ(e.orderToken, 1001u);
        EXPECT_EQ(e.session_id, 31u) << "the private ack goes to the session that has the order, as the LOB's does";
        EXPECT_EQ(e.side, Side::BID);
        EXPECT_EQ(e.rank, 0u);
        EXPECT_EQ(e.units, 10u);
        EXPECT_EQ(h.engine.book().openOrders(Side::BID), 0u);
        EXPECT_EQ(h.traces.size(), 2u) << "the order's trace, completed when it was let in, and the cancel's own";
    }
    {
        SCOPED_TRACE("a cancel of something that is not there is refused and names no side");
        Harness h;
        const BatchEvent e = only(h.cancel(7, 1001, 12));
        EXPECT_EQ(e.type, EventType::REJECTED);
        EXPECT_EQ(e.rejectReason, RejectReason::UNKNOWN_ORDER);
        EXPECT_EQ(e.rejectDetail, 0u);
        EXPECT_EQ(e.participantId, 7u);
        EXPECT_EQ(e.orderToken, 1001u);
        EXPECT_EQ(e.session_id, 12u);
        EXPECT_EQ(static_cast<unsigned>(e.side), 0u) << "a cancel does not say a side";
        EXPECT_EQ(h.traces.size(), 1u);
        ASSERT_EQ(only(h.order(designBid())).type, EventType::ACCEPTED);
        EXPECT_EQ(only(h.cancel(8, 1001)).rejectReason, RejectReason::UNKNOWN_ORDER);
        ASSERT_EQ(only(h.cancel(7, 1001)).type, EventType::CANCELED);
        EXPECT_EQ(only(h.cancel(7, 1001)).rejectReason, RejectReason::UNKNOWN_ORDER);
    }
    {
        SCOPED_TRACE("a buyer's quota and the batch's room are refused with their reasons and details");
        Harness h;
        for (OrderToken t = 1; t <= 20; ++t) ASSERT_EQ(only(h.order(bid(1, t, 1, 1, { { 1, 5 } }))).type, EventType::ACCEPTED);
        const BatchEvent quota = only(h.order(bid(1, 21, 1, 1, { { 1, 5 } })));
        EXPECT_EQ(quota.type, EventType::REJECTED);
        EXPECT_EQ(quota.rejectReason, RejectReason::PARTICIPANT_QUOTA);
        EXPECT_EQ(quota.rejectDetail, static_cast<std::uint16_t>(QuotaLimit::OPEN_ORDERS));
        EngineConfig config = testConfig();
        config.limits.maxOrdersPerSide = 1;
        Harness small(config);
        ASSERT_EQ(only(small.order(ask(1, 1, 1, 1, { { 1, 5 } }))).type, EventType::ACCEPTED);
        const BatchEvent full = only(small.order(ask(2, 1, 1, 1, { { 1, 5 } })));
        EXPECT_EQ(full.rejectReason, RejectReason::BATCH_FULL);
        EXPECT_EQ(full.rejectDetail, static_cast<std::uint16_t>(FullLimit::ORDERS));
    }
}

TEST(Engine, ClearsTheDesignsExampleAtTheAskCostWithTheBidLiftedFromA100ToH100) {
    // The bid wants one node an hour, 10 hours, of A100 or H100 and the ask offers H100 for 4 hours. They
    // can trade 4 units, one in each of the 4 hours, and the bid gets H100, which is better than the A100
    // it also accepts: every unit is lifted. The bid pays at most 2400 for the first two units and 2200
    // for the next, the ask asks 1800, so the welfare is 2400 + 2400 + 2200 + 2200 - 4 * 1800 = 2000, and
    // the minimal price is the ask's 1800.
    Harness h;
    h.order(designBid(), 31);
    h.order(designAsk(), 32);
    const auto out = h.clearBatch(0, 99);

    ASSERT_EQ(out.size(), 4u + 1u + 4u + 1u + 1u) << "4 fills and a done for the bid, the same for the ask, and the batch";
    // Sequence numbers continue after the two ACCEPTED events.
    for (std::size_t i = 0; i < out.size(); ++i) EXPECT_EQ(out[i].sequence_number, 3 + i);

    for (std::uint32_t hour = 0; hour < 4; ++hour) {
        const BatchEvent& f = out[hour];
        EXPECT_EQ(f.type, EventType::FILL) << hour;
        EXPECT_EQ(f.batchId, 0u);
        EXPECT_EQ(f.participantId, 7u);
        EXPECT_EQ(f.orderToken, 1001u);
        EXPECT_EQ(f.session_id, 31u);
        EXPECT_EQ(f.side, Side::BID);
        EXPECT_EQ(f.rank, 0u);
        EXPECT_EQ(f.accelerator, 1) << "H100";
        EXPECT_EQ(f.interconnect, 1) << "NVLink";
        EXPECT_EQ(f.region, 0) << "US_WEST";
        EXPECT_EQ(f.hour, H0 + hour);
        EXPECT_EQ(f.price, 1800u);
        EXPECT_EQ(f.units, 1u);
        EXPECT_EQ(f.lifted, 1) << "H100 is above the A100 that the bid also accepts";
    }
    const BatchEvent& bidDone = out[4];
    EXPECT_EQ(bidDone.type, EventType::ORDER_DONE);
    EXPECT_EQ(bidDone.participantId, 7u);
    EXPECT_EQ(bidDone.session_id, 31u);
    EXPECT_EQ(bidDone.units, 4u);
    EXPECT_EQ(bidDone.unfilledUnits, 6u);
    EXPECT_EQ(bidDone.liftedUnits, 4u);
    EXPECT_EQ(bidDone.amount, 4 * 1800);

    for (std::uint32_t hour = 0; hour < 4; ++hour) {
        const BatchEvent& f = out[5 + hour];
        EXPECT_EQ(f.type, EventType::FILL) << hour;
        EXPECT_EQ(f.participantId, 9u);
        EXPECT_EQ(f.session_id, 32u);
        EXPECT_EQ(f.side, Side::ASK);
        EXPECT_EQ(f.hour, H0 + hour);
        EXPECT_EQ(f.price, 1800u);
        EXPECT_EQ(f.units, 1u);
        EXPECT_EQ(f.lifted, 0);
    }
    const BatchEvent& askDone = out[9];
    EXPECT_EQ(askDone.type, EventType::ORDER_DONE);
    EXPECT_EQ(askDone.units, 4u);
    EXPECT_EQ(askDone.unfilledUnits, 12u);
    EXPECT_EQ(askDone.liftedUnits, 0u);
    EXPECT_EQ(askDone.amount, 4 * 1800) << "the provider receives what the buyer pays";

    const BatchEvent& done = out[10];
    EXPECT_EQ(done.type, EventType::BATCH_DONE);
    EXPECT_EQ(done.batchId, 0u);
    EXPECT_EQ(done.session_id, 99u);
    EXPECT_EQ(done.units, 4u);
    EXPECT_EQ(done.liftedUnits, 4u);
    EXPECT_EQ(done.amount, 2000) << "the welfare";
    EXPECT_EQ(done.rankSum, 0) << "both orders arrived first on their side";
    EXPECT_EQ(done.fills, 8u);
    EXPECT_EQ(done.certified, 1);
    EXPECT_EQ(done.bids, 1u);
    EXPECT_EQ(done.asks, 1u);
    EXPECT_EQ(done.arcs, 20u + 3u + 4u + 2u) << "the bid's 20 items and 3 steps, the ask's 4 items and 2 steps";
    EXPECT_GT(done.pivots, 0u);
}

TEST(Engine, ReportsALiftedUnitPerFillAndTheEventsOfABatchInTheOrderOfTheBook) {
    {
        SCOPED_TRACE("a unit is lifted per fill and an ask is never lifted");
        // A bid for up to 6 nodes of A100, H100 or H200 over NVLink in US_WEST, 3 at a time over 2 hours, at
        // 1000. One ask sells 2 A100 in the first hour, another sells 3 H200 in the second, both at 100.
        // The prices follow from who is short. In the first hour the ask sells all it has and the bid would
        // take a third node, so the price is the bid's 1000. In the second hour both are at their per-hour
        // limit, and the price is the lowest that the ask accepts, its 100.
        Harness h;
        h.order(bid(1, 1, 0b11, 3, { { 6, 1000 } }, A100 | H100 | H200));
        h.order(ask(2, 1, 0b01, 2, { { 2, 100 } }, A100));
        h.order(ask(3, 1, 0b10, 3, { { 3, 100 } }, H200));
        const auto out = h.clearBatch(0);

        const auto fills = ofType(out, EventType::FILL);
        ASSERT_EQ(fills.size(), 4u);
        // The bid's two fills come first, in item order: the A100 in hour 0, then the H200 in hour 1.
        EXPECT_EQ(fills[0].side, Side::BID);
        EXPECT_EQ(fills[0].accelerator, 0);
        EXPECT_EQ(fills[0].hour, H0);
        EXPECT_EQ(fills[0].units, 2u);
        EXPECT_EQ(fills[0].lifted, 0) << "the A100 is the lowest model that the bid accepts";
        EXPECT_EQ(fills[0].price, 1000u);
        EXPECT_EQ(fills[1].side, Side::BID);
        EXPECT_EQ(fills[1].accelerator, 2);
        EXPECT_EQ(fills[1].hour, H0 + 1);
        EXPECT_EQ(fills[1].units, 3u);
        EXPECT_EQ(fills[1].lifted, 1) << "the H200 is above the A100 and the H100 that it accepts";
        EXPECT_EQ(fills[1].price, 100u);
        EXPECT_EQ(fills[2].side, Side::ASK);
        EXPECT_EQ(fills[2].lifted, 0);
        EXPECT_EQ(fills[3].side, Side::ASK);
        EXPECT_EQ(fills[3].lifted, 0);

        const auto dones = ofType(out, EventType::ORDER_DONE);
        ASSERT_EQ(dones.size(), 3u);
        EXPECT_EQ(dones[0].units, 5u);
        EXPECT_EQ(dones[0].unfilledUnits, 1u);
        EXPECT_EQ(dones[0].liftedUnits, 3u) << "only the H200 units";
        EXPECT_EQ(dones[0].amount, 2 * 1000 + 3 * 100) << "the bid pays each item's one price";
        EXPECT_EQ(dones[1].liftedUnits, 0u);
        EXPECT_EQ(dones[1].amount, 2 * 1000) << "the first ask receives the price of its item";
        EXPECT_EQ(dones[2].liftedUnits, 0u);
        EXPECT_EQ(dones[2].amount, 3 * 100);
        EXPECT_EQ(out.back().liftedUnits, 3u) << "the batch total";
    }
    {
        SCOPED_TRACE("every order gets a done, and the events come bids first, then asks, then the batch");
        Harness h;
        h.order(bid(1, 1, 0b1, 1, { { 1, 10 } }));  // rank 0: too cheap, gets nothing
        h.order(bid(2, 1, 0b1, 1, { { 1, 500 } })); // rank 1: trades
        h.order(ask(3, 1, 0b1, 1, { { 1, 100 } })); // rank 0: sells
        h.order(ask(4, 1, 0b1, 1, { { 1, 900 } })); // rank 1: too dear, sells nothing
        const auto out = h.clearBatch(0);
        // bid 0: ORDER_DONE; bid 1: FILL, ORDER_DONE; ask 0: FILL, ORDER_DONE; ask 1: ORDER_DONE; BATCH_DONE.
        ASSERT_EQ(out.size(), 1u + 2u + 2u + 1u + 1u);
        EXPECT_EQ(out[0].type, EventType::ORDER_DONE);
        EXPECT_EQ(out[0].participantId, 1u);
        EXPECT_EQ(out[0].units, 0u);
        EXPECT_EQ(out[0].unfilledUnits, 1u) << "a lapsed order is told how much it did not get";
        EXPECT_EQ(out[0].amount, 0);
        EXPECT_EQ(out[1].type, EventType::FILL);
        EXPECT_EQ(out[1].participantId, 2u);
        EXPECT_EQ(out[1].rank, 1u);
        EXPECT_EQ(out[2].type, EventType::ORDER_DONE);
        EXPECT_EQ(out[2].participantId, 2u);
        EXPECT_EQ(out[2].units, 1u);
        EXPECT_EQ(out[3].type, EventType::FILL);
        EXPECT_EQ(out[3].participantId, 3u);
        EXPECT_EQ(out[3].side, Side::ASK);
        EXPECT_EQ(out[4].type, EventType::ORDER_DONE);
        EXPECT_EQ(out[5].type, EventType::ORDER_DONE);
        EXPECT_EQ(out[5].participantId, 4u);
        EXPECT_EQ(out[5].units, 0u);
        EXPECT_EQ(out[6].type, EventType::BATCH_DONE);
        EXPECT_EQ(out[6].fills, 2u);
    }
    {
        SCOPED_TRACE("the price is one per item and the buyers pay exactly what the sellers receive");
        Harness h;
        h.order(bid(1, 1, 0b11, 2, { { 3, 800 }, { 3, 400 } }, A100 | H100));
        h.order(bid(2, 1, 0b11, 2, { { 4, 600 } }, A100 | H100));
        h.order(ask(3, 1, 0b11, 3, { { 2, 100 }, { 3, 300 } }, A100));
        h.order(ask(4, 1, 0b11, 3, { { 4, 200 } }, H100));
        const auto out = h.clearBatch(0);
        std::int64_t paid = 0;
        std::int64_t received = 0;
        std::map<std::tuple<int, int, int, HourIndex>, PriceTicks> price;
        for (const BatchEvent& e : ofType(out, EventType::FILL)) {
            const auto key = std::make_tuple(int(e.accelerator), int(e.interconnect), int(e.region), e.hour);
            const auto found = price.emplace(key, e.price);
            EXPECT_EQ(found.first->second, e.price) << "two prices for one item";
        }
        for (const BatchEvent& e : ofType(out, EventType::ORDER_DONE)) {
            (e.side == Side::BID ? paid : received) += e.amount;
        }
        EXPECT_GT(paid, 0);
        EXPECT_EQ(paid, received) << "the exchange takes in what it pays out";
    }
    {
        SCOPED_TRACE("an earlier order wins a tie and a cancel does not renumber the orders after it");
        // One unit is on offer. Three equal bids want it. The earliest wins; cancel it and the next earliest
        // wins, whatever has arrived since.
        Harness h;
        h.order(ask(9, 1, 0b1, 1, { { 1, 100 } }));
        h.order(bid(1, 1, 0b1, 1, { { 1, 500 } }));
        h.order(bid(2, 1, 0b1, 1, { { 1, 500 } }));
        h.order(bid(3, 1, 0b1, 1, { { 1, 500 } }));
        ASSERT_EQ(only(h.cancel(1, 1)).type, EventType::CANCELED);
        const BatchEvent late = only(h.order(bid(4, 1, 0b1, 1, { { 1, 500 } })));
        EXPECT_EQ(late.rank, 3u) << "ranks 0, 1, 2 were taken; the gap at 0 is not reused";
        const auto out = h.clearBatch(0);
        const auto fills = ofType(out, EventType::FILL);
        ASSERT_EQ(fills.size(), 2u) << "one fill for the winning bid, one for the ask";
        EXPECT_EQ(fills[0].side, Side::BID);
        EXPECT_EQ(fills[0].participantId, 2u) << "rank 1 beat ranks 2 and 3";
        EXPECT_EQ(fills[0].rank, 1u);
        EXPECT_EQ(out.back().rankSum, 1 + 0) << "the winning bid's rank plus the ask's";
    }
    {
        SCOPED_TRACE("an empty batch still clears");
        Harness h;
        const BatchEvent e = only(h.clearBatch(0));
        EXPECT_EQ(e.type, EventType::BATCH_DONE);
        EXPECT_EQ(e.batchId, 0u);
        EXPECT_EQ(e.units, 0u);
        EXPECT_EQ(e.amount, 0);
        EXPECT_EQ(e.fills, 0u);
        EXPECT_EQ(e.bids, 0u);
        EXPECT_EQ(e.asks, 0u);
        EXPECT_EQ(e.arcs, 0u);
        EXPECT_EQ(h.engine.book().batchId(), 1u);
        EXPECT_EQ(h.engine.book().firstHour(), H0 + 1);
        EXPECT_EQ(h.traces.size(), 1u);
    }
    {
        SCOPED_TRACE("a batch with only bids gives every bid its done and nothing traded");
        Harness h;
        h.order(bid(1, 1, 0b1, 1, { { 5, 500 } }));
        h.order(bid(2, 1, 0b1, 1, { { 7, 500 } }));
        const auto out = h.clearBatch(0);
        ASSERT_EQ(out.size(), 3u);
        EXPECT_EQ(out[0].type, EventType::ORDER_DONE);
        EXPECT_EQ(out[0].unfilledUnits, 5u);
        EXPECT_EQ(out[1].unfilledUnits, 7u);
        EXPECT_EQ(out[2].type, EventType::BATCH_DONE);
        EXPECT_EQ(out[2].units, 0u);
        EXPECT_EQ(out[2].bids, 2u);
        EXPECT_EQ(out[2].asks, 0u);
    }
}

TEST(Engine, EndsTheBatchAtTheClearAndOpensTheNextWhateverIdTheClearCarried) {
    {
        SCOPED_TRACE("orders lapse at the clear and the next batch sells an hour later");
        Harness h;
        h.order(bid(1, 1, 0b1, 1, { { 1, 5 } }));
        h.clearBatch(0);
        EXPECT_EQ(only(h.cancel(1, 1)).rejectReason, RejectReason::UNKNOWN_ORDER) << "the bid lapsed";
        // Hour 1001 has gone from the window, and hour 1048 has come into it.
        Order old = bid(1, 1, 0b1, 1, { { 1, 5 } });
        const BatchEvent refused = only(h.order(old));
        EXPECT_EQ(refused.rejectReason, RejectReason::SLOT_NOT_ELIGIBLE);
        EXPECT_EQ(refused.batchId, 1u) << "checked against batch 1";
        Order next = bid(1, 1, 0b1, 1, { { 1, 5 } });
        next.baseHour = H0 + 1;
        const BatchEvent ok = only(h.order(next));
        EXPECT_EQ(ok.type, EventType::ACCEPTED);
        EXPECT_EQ(ok.batchId, 1u);
        EXPECT_EQ(ok.rank, 0u) << "ranks start again, and the lapsed order's token is free";
        Order last = bid(1, 2, 0b1, 1, { { 1, 5 } });
        last.baseHour = H0 + 47;
        EXPECT_EQ(only(h.order(last)).type, EventType::ACCEPTED) << "the new last hour";
        last.token = 3;
        last.baseHour = H0 + 48;
        EXPECT_EQ(only(h.order(last)).rejectReason, RejectReason::SLOT_NOT_ELIGIBLE);
    }
    {
        SCOPED_TRACE("any batch id is accepted and the next window follows it");
        Harness h;
        EXPECT_EQ(only(h.clearBatch(7)).batchId, 7u) << "the event carries the id of the clear";
        EXPECT_EQ(h.engine.book().batchId(), 8u);
        EXPECT_EQ(h.engine.book().firstHour(), H0 + 8);
        EXPECT_EQ(only(h.clearBatch(7)).batchId, 7u);
        EXPECT_EQ(h.engine.book().batchId(), 8u);
        EXPECT_EQ(only(h.clearBatch(2)).batchId, 2u);
        EXPECT_EQ(h.engine.book().batchId(), 3u);
        EXPECT_EQ(h.engine.book().firstHour(), H0 + 3);
        Order o = bid(1, 1, 0b1, 1, { { 1, 5 } });
        o.baseHour = H0 + 3;
        EXPECT_EQ(only(h.order(o)).type, EventType::ACCEPTED);
        EXPECT_EQ(only(h.order(o)).type, EventType::REJECTED) << "token in use";
    }
    {
        SCOPED_TRACE("the largest batch id makes every order look ineligible until a smaller one comes");
        Harness h;
        const BatchId top = std::numeric_limits<BatchId>::max();
        EXPECT_EQ(only(h.clearBatch(top)).batchId, top);
        EXPECT_EQ(h.engine.book().batchId(), top) << "it stops there instead of wrapping to 0";
        for (const HourIndex hour : { HourIndex{ 0 }, H0, H0 + 8, std::numeric_limits<HourIndex>::max() }) {
            Order o = bid(1, hour + 1, 0b1, 1, { { 1, 5 } });
            o.baseHour = hour;
            const BatchEvent e = only(h.order(o));
            EXPECT_EQ(e.rejectReason, RejectReason::SLOT_NOT_ELIGIBLE) << hour;
            EXPECT_EQ(e.batchId, top);
        }
        EXPECT_EQ(only(h.clearBatch(0)).batchId, 0u);
        EXPECT_EQ(h.engine.book().batchId(), 1u);
        Order o = bid(1, 1, 0b1, 1, { { 1, 5 } });
        o.baseHour = H0 + 1;
        EXPECT_EQ(only(h.order(o)).type, EventType::ACCEPTED);
    }
    {
        SCOPED_TRACE("the first batch id is configured, and the default configuration uses the default schedule");
        EngineConfig config = testConfig();
        config.firstBatchId = 5;
        Harness h(config);
        EXPECT_EQ(h.engine.book().batchId(), 5u);
        Order o = bid(1, 1, 0b1, 1, { { 1, 5 } });
        o.baseHour = H0 + 5;
        const BatchEvent e = only(h.order(o));
        EXPECT_EQ(e.type, EventType::ACCEPTED);
        EXPECT_EQ(e.batchId, 5u);

        std::vector<BatchEvent> events;
        std::vector<OrderTrace> traces;
        TestingPolicy<BatchEvent> policy{ events, traces };
        BatchEngine<TestingPolicy<BatchEvent>> engine(policy);
        EXPECT_EQ(engine.book().batchId(), 0u);
        EXPECT_EQ(engine.book().firstHour(), schedule::firstSlotHour(schedule::Schedule{}, 0));
        EXPECT_EQ(engine.book().firstHour(), 497'449u);
    }
}

namespace {

// A batch that takes more than one pivot, with a few orders on each side.
void fillWithAMarket(Harness& h) {
    for (ParticipantId p = 1; p <= 4; ++p) {
        h.order(bid(p, 1, 0b111, 2, { { 3, 900 - 10 * p }, { 3, 500 } }, A100 | H100));
        h.order(ask(10 + p, 1, 0b111, 2, { { 3, 100 + 10 * p }, { 3, 600 } }, p % 2 ? A100 : H100));
    }
}

} // namespace

TEST(Engine, VoidsABatchTheClearCannotDoWithOneEventAndLetsEveryOrderLapse) {
    {
        SCOPED_TRACE("a batch that runs out of pivots is voided with one event and every order lapses");
        EngineConfig config = testConfig();
        config.clearing.maxPivots = 1;
        Harness h(config);
        fillWithAMarket(h);
        ASSERT_EQ(h.engine.book().openOrders(Side::BID), 4u);
        const std::uint64_t arcs = h.engine.book().arcs();

        const auto out = h.clearBatch(0);
        const BatchEvent& e = only(out);
        EXPECT_EQ(e.type, EventType::BATCH_VOIDED);
        EXPECT_EQ(e.voidReason, VoidReason::PIVOT_BUDGET);
        EXPECT_EQ(e.batchId, 0u);
        EXPECT_EQ(e.bids, 4u);
        EXPECT_EQ(e.asks, 4u);
        EXPECT_EQ(e.arcs, arcs);
        EXPECT_EQ(e.pivots, 1u) << "it stopped at the budget";
        EXPECT_EQ(e.fills, 0u);
        EXPECT_EQ(e.units, 0u);

        // No fills, one event. The orders lapsed, and the exchange goes on.
        EXPECT_EQ(h.engine.book().openOrders(Side::BID), 0u);
        EXPECT_EQ(h.engine.book().batchId(), 1u);
        EXPECT_EQ(h.traces.size(), 8u + 1u) << "the eight orders' traces and the clear's";
        const OrderTrace& trace = h.traces.back();
        EXPECT_EQ(counter(trace, GpuExtSlot::LiftedUnits), 0);
        EXPECT_EQ(counter(trace, GpuExtSlot::BidsFilled), 0);
        EXPECT_EQ(counter(trace, GpuExtSlot::BidsUnfilled), 0);
        EXPECT_EQ(counter(trace, GpuExtSlot::SolverPivots), 1);

        Order o = bid(1, 1, 0b1, 1, { { 1, 5 } });
        o.baseHour = H0 + 1;
        EXPECT_EQ(only(h.order(o)).type, EventType::ACCEPTED);
    }
    {
        SCOPED_TRACE("a zero budget voids any batch that needs a pivot");
        EngineConfig config = testConfig();
        config.clearing.maxPivots = 0;
        Harness h(config);
        h.order(bid(1, 1, 0b1, 1, { { 1, 500 } }));
        h.order(ask(2, 1, 0b1, 1, { { 1, 100 } }));
        const BatchEvent e = only(h.clearBatch(0));
        EXPECT_EQ(e.type, EventType::BATCH_VOIDED);
        EXPECT_EQ(e.voidReason, VoidReason::PIVOT_BUDGET);
        EXPECT_EQ(e.pivots, 0u);
        EXPECT_EQ(e.arcs, 4u);
    }
    {
        SCOPED_TRACE("a batch the clear refuses is voided as refused input");
        // The book's limits keep this from happening. A clear that is set to take fewer arcs than the book
        // lets in shows what the engine does if it ever did.
        EngineConfig config = testConfig();
        config.clearing.maxArcs = 3;
        Harness h(config);
        h.order(bid(1, 1, 0b1, 1, { { 1, 500 } }));
        h.order(ask(2, 1, 0b1, 1, { { 1, 100 } }));
        const BatchEvent e = only(h.clearBatch(0));
        EXPECT_EQ(e.type, EventType::BATCH_VOIDED);
        EXPECT_EQ(e.voidReason, VoidReason::REFUSED_INPUT);
        EXPECT_EQ(e.bids, 1u);
        EXPECT_EQ(e.asks, 1u);
        EXPECT_EQ(e.arcs, 4u) << "the book's own count";
        EXPECT_EQ(h.engine.book().batchId(), 1u);
        EXPECT_EQ(h.engine.book().openOrders(Side::BID), 0u);
    }
    {
        SCOPED_TRACE("a clear that fails its own check voids the batch as a solver error");
        EngineConfig config = testConfig();
        config.clearFunction = [](const ClearingInput&, const ClearingOptions&) -> ClearingResult {
            throw std::logic_error("the optimality check failed");
        };
        Harness h(config);
        h.order(bid(1, 1, 0b1, 1, { { 1, 500 } }));
        const BatchEvent e = only(h.clearBatch(0));
        EXPECT_EQ(e.type, EventType::BATCH_VOIDED);
        EXPECT_EQ(e.voidReason, VoidReason::SOLVER_ERROR);
        EXPECT_EQ(h.engine.book().batchId(), 1u);
    }
}

namespace {

ClearingInput g_captured;
ClearingOptions g_capturedOptions;
int g_calls = 0;

ClearingResult capturingClear(const ClearingInput& input, const ClearingOptions& options) {
    g_captured = input;
    g_capturedOptions = options;
    ++g_calls;
    return clear(input, options);
}

} // namespace

TEST(Engine, GivesTheClearExactlyTheLiveOrdersAndStopsOnWhatIsNotTheBatchsFault) {
    {
        SCOPED_TRACE("the clear gets every live order with its rank, curve and the items of its box, and nothing else");
        EngineConfig config = testConfig();
        config.clearFunction = &capturingClear;
        config.clearing.maxPivots = 12345;
        g_calls = 0;
        Harness h(config);
        h.order(bid(1, 1, 0b101, 2, { { 3, 700 }, { 2, 400 } }, A100 | H100, NVLINK, US_WEST));
        h.order(bid(2, 1, 0b1, 1, { { 1, 10 } }, H200, PCIE, EU_WEST));
        h.order(ask(3, 1, 0b11, 4, { { 5, 50 } }, H100, NVLINK, US_WEST));
        h.order(bid(4, 1, 0b1, 1, { { 1, 10 } }));
        h.cancel(2, 1);
        h.clearBatch(0);

        ASSERT_EQ(g_calls, 1);
        EXPECT_EQ(g_capturedOptions.maxPivots, 12345u) << "the engine passes the configured options on";
        EXPECT_EQ(g_captured.itemCount, 1410u);
        ASSERT_EQ(g_captured.bids.size(), 2u);
        ASSERT_EQ(g_captured.asks.size(), 1u);
        EXPECT_EQ(g_captured.bidRank, (std::vector<std::uint32_t>{ 0, 2 })) << "the cancelled bid left a gap";
        EXPECT_EQ(g_captured.askRank, (std::vector<std::uint32_t>{ 0 }));
        EXPECT_EQ(g_captured.bids[0].id, 0u);
        EXPECT_EQ(g_captured.bids[1].id, 2u);
        EXPECT_EQ(g_captured.bids[0].perSlotCap, 2u);
        EXPECT_EQ(g_captured.bids[0].stepCount, 2u);
        EXPECT_EQ(g_captured.bids[0].steps[0].quantity, 3u);
        EXPECT_EQ(g_captured.bids[0].steps[0].price, 700u);
        EXPECT_EQ(g_captured.bids[0].steps[1].quantity, 2u);
        EXPECT_EQ(g_captured.bids[0].steps[1].price, 400u);
        // A100 and H100 over NVLink in US_WEST, at hours 0 and 2: SKUs 3 and 9, items 3, 9, 63, 69.
        EXPECT_EQ(g_captured.bids[0].items, (std::vector<ItemIndex>{ 3, 9, 63, 69 }));
        // The ask: H100 over NVLink in US_WEST, at hours 0 and 1.
        EXPECT_EQ(g_captured.asks[0].items, (std::vector<ItemIndex>{ 9, 39 }));
        EXPECT_NO_THROW(validateInput(g_captured));
    }
    {
        SCOPED_TRACE("the clear is called only for a batch clear, and for an empty batch too");
        EngineConfig config = testConfig();
        config.clearFunction = &capturingClear;
        g_calls = 0;
        Harness h(config);
        h.order(designBid());
        h.order(designAsk());
        h.cancel(7, 1001);
        h.cancel(5, 5);
        EXPECT_EQ(g_calls, 0);
        h.clearBatch(0);
        EXPECT_EQ(g_calls, 1);
        h.clearBatch(1);
        EXPECT_EQ(g_calls, 2) << "an empty batch is cleared too, so that every replica does the same";
    }
    {
        SCOPED_TRACE("a batch is certified exactly when the clear says it proved its answer");
        // The production clear ends every clear with an optimality certificate and the reference solver does
        // not, so a batch cleared with it must not claim one.
        Harness proved;
        proved.order(designBid());
        proved.order(designAsk());
        EXPECT_EQ(proved.clearBatch(0).back().certified, 1);

        EngineConfig config = testConfig();
        config.clearFunction = [](const ClearingInput& input, const ClearingOptions&) { return clearReference(input); };
        Harness h(config);
        h.order(designBid());
        h.order(designAsk());
        const auto out = h.clearBatch(0);
        ASSERT_EQ(out.back().type, EventType::BATCH_DONE);
        EXPECT_EQ(out.back().certified, 0);
        EXPECT_EQ(out.back().amount, 2000) << "the same welfare as the production clear";
    }
    {
        SCOPED_TRACE("an error that is not about the batch is not swallowed and leaves the batch as it was");
        EngineConfig config = testConfig();
        config.clearFunction = [](const ClearingInput&, const ClearingOptions&) -> ClearingResult {
            throw std::runtime_error("something outside the batch");
        };
        Harness h(config);
        h.order(bid(1, 1, 0b1, 1, { { 1, 500 } }));
        const std::size_t before = h.events.size();
        EXPECT_THROW(h.clearBatch(0), std::runtime_error);
        EXPECT_EQ(h.events.size(), before) << "nothing was emitted";
        EXPECT_EQ(h.engine.book().batchId(), 0u);
        EXPECT_EQ(h.engine.book().openOrders(Side::BID), 1u);
    }
    {
        SCOPED_TRACE("a command of a type the handler never produces stops the engine");
        Harness h;
        BatchCommand c{};
        c.type = static_cast<CommandType>(99);
        EXPECT_THROW(h.send(c), std::logic_error);
        EXPECT_TRUE(h.events.empty());
    }
    {
        SCOPED_TRACE("a configuration that cannot work is refused at the start");
        std::vector<BatchEvent> events;
        std::vector<OrderTrace> traces;
        TestingPolicy<BatchEvent> policy{ events, traces };
        EngineConfig config = testConfig();
        config.clearFunction = nullptr;
        EXPECT_THROW((BatchEngine<TestingPolicy<BatchEvent>>(policy, config)), std::invalid_argument);
        config = testConfig();
        config.schedule.periodSeconds = 0;
        EXPECT_THROW((BatchEngine<TestingPolicy<BatchEvent>>(policy, config)), std::invalid_argument);
        config = testConfig();
        config.limits.maxArcs = limits::MAX_ARCS_PER_BATCH + 1;
        EXPECT_THROW((BatchEngine<TestingPolicy<BatchEvent>>(policy, config)), std::invalid_argument);
    }
}

TEST(Engine, CompletesEveryCommandsTraceOnceAndCarriesTheBatchStatisticsOnTheClearsTrace) {
    {
        SCOPED_TRACE("every command's trace is completed once, inside its own call, and stamped by the engine");
        Harness h;
        BatchCommand bad = designBid().command();
        bad.acceleratorMask = 0;
        h.order(designBid());                     // accepted
        h.order(designAsk());                     // accepted
        h.order(bid(1, 1, 0b1, 1, { { 1, 5 } })); // accepted
        h.send(bad);                              // refused
        h.cancel(1, 1);                           // a cancel
        h.cancel(1, 1);                           // unknown
        h.clearBatch(0);
        ASSERT_EQ(h.commands, 7u);
        ASSERT_EQ(h.traces.size(), 7u);
        EXPECT_EQ(h.completedTraceIds(), (std::vector<std::uint64_t>{ 1, 2, 3, 4, 5, 6, 7 }));
        for (const OrderTrace& trace : h.traces) {
            EXPECT_NE(trace.match_done_tai_ns, 0u) << "Core stamps it only after the trace has been copied out";
            EXPECT_GE(trace.match_done_tai_ns, trace.engine_pop_tai_ns);
        }
        for (std::size_t i = 0; i + 1 < h.traces.size(); ++i) {
            for (const GpuExtSlot slot : { GpuExtSlot::LiftedUnits, GpuExtSlot::BidsFilled, GpuExtSlot::BidsUnfilled, GpuExtSlot::SolverPivots }) {
                EXPECT_EQ(counter(h.traces[i], slot), 0) << "command " << i << " is not a clear, and the slot held something else";
            }
        }
    }
    {
        SCOPED_TRACE("the batch statistics ride on the trace of the clear");
        Harness h;
        h.order(bid(1, 1, 0b11, 3, { { 6, 1000 } }, A100 | H100 | H200));
        h.order(bid(2, 1, 0b01, 1, { { 1, 1 } }, A100)); // too cheap: unfilled
        h.order(ask(3, 1, 0b01, 2, { { 2, 100 } }, A100));
        h.order(ask(4, 1, 0b10, 3, { { 3, 100 } }, H200));
        // A third bid that trades with a third ask in the third hour, so that two bids fill and one does not:
        // the two counts differ, and a swap of them shows.
        h.order(bid(5, 1, 0b100, 2, { { 2, 900 } }, A100));
        h.order(ask(6, 1, 0b100, 2, { { 2, 100 } }, A100));
        h.clearBatch(0);
        const OrderTrace& trace = h.traces.back();
        EXPECT_EQ(counter(trace, GpuExtSlot::LiftedUnits), 3);
        EXPECT_EQ(counter(trace, GpuExtSlot::BidsFilled), 2);
        EXPECT_EQ(counter(trace, GpuExtSlot::BidsUnfilled), 1);
        EXPECT_EQ(counter(trace, GpuExtSlot::SolverPivots), static_cast<std::int32_t>(h.events.back().pivots));
        EXPECT_GT(h.events.back().pivots, 0u);
        // A second clear writes its own numbers, not the last ones added to.
        h.clearBatch(1);
        EXPECT_EQ(counter(h.traces.back(), GpuExtSlot::LiftedUnits), 0);
        EXPECT_EQ(counter(h.traces.back(), GpuExtSlot::BidsFilled), 0);
    }
}

TEST(Engine, IsDeterministicAndGivesTheSameEventsInCoresReplicaHostAsInADirectRun) {
    // Two engines that see the same commands emit the same events, with sequence numbers from one and no gap.
    auto run = [](Harness& h) {
        h.order(designBid(), 31);
        h.order(designAsk(), 32);
        h.order(bid(3, 1, 0b111, 2, { { 4, 3000 }, { 2, 100 } }, A100 | H200), 33);
        h.cancel(3, 1, 34);
        h.order(bid(3, 2, 0b111, 2, { { 4, 3000 }, { 2, 100 } }, A100 | H200), 33);
        h.order(ask(5, 1, 0b111, 2, { { 6, 700 } }, H200), 35);
        h.clearBatch(0);
        h.order(designAsk());
        h.clearBatch(1);
    };
    Harness a;
    Harness b;
    run(a);
    run(b);
    EXPECT_TRUE(sameEvents(a.events, b.events));
    ASSERT_GT(a.events.size(), 20u);
    for (std::size_t i = 0; i < a.events.size(); ++i) EXPECT_EQ(a.events[i].sequence_number, i + 1);

    std::vector<BatchCommand> commands;
    auto add = [&](BatchCommand c, SessionId session) {
        c.trace_id = static_cast<TraceId>(commands.size() + 1);
        c.sessionId = session;
        commands.push_back(c);
    };
    add(designBid().command(), 31);
    add(designAsk().command(), 32);
    add(bid(3, 1, 0b111, 2, { { 4, 3000 }, { 2, 100 } }, A100 | H200).command(), 33);
    {
        BatchCommand cancel{};
        cancel.type = CommandType::CANCEL;
        cancel.participantId = 3;
        cancel.orderToken = 1;
        add(cancel, 34);
    }
    {
        BatchCommand clearCmd{};
        clearCmd.type = CommandType::BATCH_CLEAR;
        clearCmd.batchId = 0;
        add(clearCmd, 99);
    }

    // The same commands straight into an engine.
    std::vector<BatchEvent> expected;
    {
        std::vector<OrderTrace> traces;
        TestingPolicy<BatchEvent> policy{ expected, traces };
        BatchEngine<TestingPolicy<BatchEvent>> engine(policy, testConfig());
        for (BatchCommand c : commands) engine.submit_order(c);
    }

    // Core's queues and its MatchingService thread.
    auto inbound = std::make_unique<SPSCQueue<BatchCommand>>();
    auto eventQueue = std::make_unique<SPSCQueue<BatchEvent, 16384>>();
    auto traceQueue = std::make_unique<SPSCQueue<OrderTrace, 16384>>();
    SPSCProducerPolicy<BatchEvent> policy{ *eventQueue, *traceQueue };
    BatchEngine<SPSCProducerPolicy<BatchEvent>> engine(policy, testConfig());
    {
        MatchingService<BatchCommand, BatchEngine<SPSCProducerPolicy<BatchEvent>>> matcher(*inbound, engine);
        matcher.start();
        for (const BatchCommand& c : commands) {
            while (!inbound->try_push(c)) std::this_thread::yield();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (matcher.lastAppliedSeq() != commands.back().trace_id && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        ASSERT_EQ(matcher.lastAppliedSeq(), commands.back().trace_id) << "the host applied every command";
        matcher.stop();
    }
    std::vector<BatchEvent> actual;
    BatchEvent e;
    while (eventQueue->try_pop(e)) actual.push_back(e);
    EXPECT_TRUE(sameEvents(expected, actual));
    EXPECT_GT(actual.size(), 10u);
}

// A batch at the arc limit takes seconds to clear in an optimized build and minutes in a sanitizer build,
// where it tests nothing that the smaller ones do not, so it runs only when the compiler optimizes.
#ifdef __OPTIMIZE__
TEST(Engine, ClearsABatchAtTheArcLimitAndEveryCountAddsUp) {
    EngineConfig config = testConfig();
    Harness h(config);
    std::uint64_t seed = 12345;
    auto next = [&seed]() {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(seed >> 33);
    };

    // 500 buyers, each with the whole of their quotas: two bids using 904 and 76 arcs.
    for (ParticipantId buyer = 0; buyer < 500; ++buyer) {
        Order wide = bid(buyer, 1, (std::uint64_t{ 1 } << 30) - 1, 2, { { 3, 1200 + next() % 800 }, { 3, 900 }, { 3, 600 }, { 3, 300 } },
                         0b11111, 0b11, 0b111);
        ASSERT_EQ(only(h.order(wide)).type, EventType::ACCEPTED);
        Order narrow = bid(buyer, 2, (std::uint64_t{ 1 } << 12) - 1, 2, { { 3, 1200 + next() % 800 }, { 3, 900 }, { 3, 600 }, { 3, 300 } },
                           0b00001, 0b11, 0b111);
        ASSERT_EQ(only(h.order(narrow)).type, EventType::ACCEPTED);
    }
    // 10,000 sellers, each offering 47 hours of one SKU, four steps: 51 arcs.
    for (std::uint32_t seller = 0; seller < 10'000; ++seller) {
        const std::uint32_t accel = 1u << (seller % 5);
        Order o = ask(100'000 + seller, 1, (std::uint64_t{ 1 } << 47) - 1, 1 + next() % 3,
                      { { 1, 100 + next() % 500 }, { 1, 700 }, { 1, 900 }, { 1, 1100 } }, accel,
                      static_cast<std::uint8_t>(1u << ((seller / 5) % 2)), 1u << ((seller / 10) % 3));
        ASSERT_EQ(only(h.order(o)).type, EventType::ACCEPTED) << seller;
    }
    ASSERT_EQ(h.engine.book().arcs(), limits::MAX_ARCS_PER_BATCH);

    const auto out = h.clearBatch(0);
    const BatchEvent& done = out.back();
    ASSERT_EQ(done.type, EventType::BATCH_DONE) << describe(done);
    EXPECT_EQ(done.arcs, limits::MAX_ARCS_PER_BATCH);
    EXPECT_EQ(done.bids, 1000u);
    EXPECT_EQ(done.asks, 10'000u);
    EXPECT_EQ(done.certified, 1);
    EXPECT_GT(done.units, 0u);

    std::uint64_t bidUnits = 0;
    std::uint64_t askUnits = 0;
    std::int64_t paid = 0;
    std::int64_t received = 0;
    std::uint64_t fills = 0;
    std::uint64_t dones = 0;
    for (const BatchEvent& e : out) {
        if (e.type == EventType::FILL) {
            ++fills;
            (e.side == Side::BID ? bidUnits : askUnits) += e.units;
        } else if (e.type == EventType::ORDER_DONE) {
            ++dones;
            (e.side == Side::BID ? paid : received) += e.amount;
        }
    }
    EXPECT_EQ(dones, 11'000u) << "every order gets a done";
    EXPECT_EQ(fills, done.fills);
    EXPECT_EQ(bidUnits, done.units);
    EXPECT_EQ(askUnits, done.units);
    EXPECT_EQ(paid, received) << "the exchange takes in what it pays out";
}
#endif
