#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "BatchBook.hpp"
#include "ClearingValidation.hpp"
#include "EngineFixtures.hpp"

using namespace enginefix;

// These drive the book directly, with no events, on cases worked out by hand with the limits at their
// settled numbers. The random streams in EngineModelTests.cpp hold it to a second implementation.

namespace {

BatchBook::Admission admit(BatchBook& book, const Order& o) { return book.admit(o.command()); }

::testing::AssertionResult accepted(const BatchBook::Admission& a) {
    if (a.reject) {
        return ::testing::AssertionFailure() << "refused: " << toString(a.reject.reason) << " detail " << a.reject.detail;
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult refusedWith(const BatchBook::Admission& a, RejectReason reason, std::uint16_t detail) {
    if (!a.reject) return ::testing::AssertionFailure() << "accepted, expected " << toString(reason);
    if (a.reject.reason != reason || a.reject.detail != detail) {
        return ::testing::AssertionFailure() << "refused with " << toString(a.reject.reason) << " detail "
                                             << a.reject.detail << ", expected " << toString(reason) << " detail " << detail;
    }
    return ::testing::AssertionSuccess();
}

BatchBook newBook(const BookLimits& limits = {}, BatchId first = 0) {
    return BatchBook(testSchedule(), limits, first);
}

// Its arcs are one per item (models x links x regions x hours) and one per step.
Order wideBid(ParticipantId participant, OrderToken token, std::uint32_t accelMask, std::uint8_t interMask,
              std::uint32_t regionMask, unsigned slotsCount, std::size_t steps) {
    Order o;
    o.participant = participant;
    o.token = token;
    o.accel = accelMask;
    o.inter = interMask;
    o.region = regionMask;
    o.slots = (std::uint64_t{ 1 } << slotsCount) - 1;
    o.steps.clear();
    for (std::size_t k = 0; k < steps; ++k) o.steps.push_back({ 1, static_cast<PriceTicks>(100 - k) });
    return o;
}

constexpr auto OPEN_ORDERS = static_cast<std::uint16_t>(QuotaLimit::OPEN_ORDERS);
constexpr auto SUBMISSIONS = static_cast<std::uint16_t>(QuotaLimit::SUBMISSIONS);
constexpr auto QUOTA_ARCS = static_cast<std::uint16_t>(QuotaLimit::ARCS);
constexpr auto FULL_ORDERS = static_cast<std::uint16_t>(FullLimit::ORDERS);
constexpr auto FULL_SUBMISSIONS = static_cast<std::uint16_t>(FullLimit::SUBMISSIONS);
constexpr auto FULL_ARCS = static_cast<std::uint16_t>(FullLimit::ARCS);

} // namespace

TEST(BatchBook, NumbersEachSidesOrdersFromZeroKeepsWhatTheClearNeedsAndLeavesAGapWhenOneIsCancelled) {
    {
        SCOPED_TRACE("a new book is open for the first batch and empty");
        const BatchBook book = newBook();
        EXPECT_EQ(book.batchId(), 0u);
        EXPECT_EQ(book.firstHour(), 1001u);
        EXPECT_EQ(book.openOrders(Side::BID), 0u);
        EXPECT_EQ(book.openOrders(Side::ASK), 0u);
        EXPECT_EQ(book.submissions(Side::BID), 0u);
        EXPECT_EQ(book.submissions(Side::ASK), 0u);
        EXPECT_EQ(book.arcs(), 0u);
        const ClearingInput input = book.clearingInput();
        EXPECT_EQ(input.itemCount, 1410u);
        EXPECT_TRUE(input.bids.empty());
        EXPECT_TRUE(input.asks.empty());
        EXPECT_TRUE(input.bidRank.empty());
        EXPECT_TRUE(input.askRank.empty());
    }
    {
        SCOPED_TRACE("the first batch id sets the hours the book sells");
        EXPECT_EQ(newBook({}, 0).firstHour(), 1001u);
        EXPECT_EQ(newBook({}, 1).firstHour(), 1002u);
        EXPECT_EQ(newBook({}, 5).firstHour(), 1006u);
        EXPECT_EQ(newBook({}, 5).batchId(), 5u);
        EXPECT_EQ(BatchBook().firstHour(), 497'449u) << "with the default schedule, as an exchange runs it";
    }
    {
        SCOPED_TRACE("each side numbers its orders from zero in arrival order");
        BatchBook book = newBook();
        for (std::uint32_t k = 0; k < 4; ++k) {
            const auto a = admit(book, bid(10 + k, 1, 1, 1, { { 1, 50 } }));
            ASSERT_TRUE(accepted(a));
            EXPECT_EQ(a.rank, k);
        }
        for (std::uint32_t k = 0; k < 3; ++k) {
            const auto a = admit(book, ask(20 + k, 1, 1, 1, { { 1, 50 } }));
            ASSERT_TRUE(accepted(a));
            EXPECT_EQ(a.rank, k) << "the asks have their own ranks";
        }
        EXPECT_EQ(book.submissions(Side::BID), 4u);
        EXPECT_EQ(book.submissions(Side::ASK), 3u);
        EXPECT_EQ(book.openOrders(Side::BID), 4u);
        EXPECT_EQ(book.openOrders(Side::ASK), 3u);
    }
    {
        SCOPED_TRACE("a refused order takes no rank and counts for nothing");
        BatchBook book = newBook();
        ASSERT_TRUE(accepted(admit(book, bid(1, 1, 1, 1, { { 1, 50 } }))));
        Order late = bid(1, 2, 1, 1, { { 1, 50 } });
        late.baseHour = H0 - 5; // hour 996, which is past
        ASSERT_TRUE(refusedWith(admit(book, late), RejectReason::SLOT_NOT_ELIGIBLE, 0));
        EXPECT_EQ(book.submissions(Side::BID), 1u);
        EXPECT_EQ(book.openOrders(Side::BID), 1u);
        EXPECT_EQ(book.buyer(1).submissions, 1u) << "the buyer's submissions count accepted orders only";
        const auto next = admit(book, bid(1, 3, 1, 1, { { 1, 50 } }));
        EXPECT_EQ(next.rank, 1u) << "the refused order left no gap in the ranks";
    }
    {
        SCOPED_TRACE("the book keeps what the clear and the events need of an order");
        BatchBook book = newBook();
        Order o = bid(7, 99, 0b101, 3, { { 2, 300 }, { 3, 200 } }, A100 | H200, PCIE | NVLINK, US_WEST | EU_WEST);
        o.cap = 4;
        o.slots = 0b101;
        BatchCommand command = o.command();
        command.sessionId = 42;
        const auto a = book.admit(command);
        ASSERT_TRUE(accepted(a));
        const StoredOrder& s = book.order({ Side::BID, a.rank });
        EXPECT_EQ(s.participant, 7u);
        EXPECT_EQ(s.token, 99u);
        EXPECT_EQ(s.session, 42u);
        EXPECT_EQ(s.side, Side::BID);
        EXPECT_TRUE(s.live);
        EXPECT_EQ(s.acceleratorMask, A100 | H200);
        EXPECT_EQ(s.interconnectMask, PCIE | NVLINK);
        EXPECT_EQ(s.regionMask, US_WEST | EU_WEST);
        EXPECT_EQ(s.slotMask, 0b101u) << "hours 0 and 2 of the window";
        EXPECT_EQ(s.perSlotCap, 4u);
        EXPECT_EQ(s.stepCount, 2u);
        EXPECT_EQ(s.steps[0].quantity, 2u);
        EXPECT_EQ(s.steps[0].price, 300u);
        EXPECT_EQ(s.steps[1].quantity, 3u);
        EXPECT_EQ(s.steps[1].price, 200u);
        EXPECT_EQ(s.totalUnits, 5u);
        // 2 accelerators x 2 interconnects x 2 regions x 2 hours = 16 items, and 2 steps.
        EXPECT_EQ(s.arcs, 18u);
        EXPECT_EQ(book.arcs(), 18u);
        EXPECT_EQ(s.skus, catalog::skusNamedBy(A100 | H200, PCIE | NVLINK, US_WEST | EU_WEST));
    }
    {
        SCOPED_TRACE("hours are kept relative to the first hour of the batch");
        BatchBook book = newBook(); // sells 1001 to 1047
        Order early = bid(1, 1, 0, 1, { { 1, 1 } });
        early.baseHour = H0 - 2;
        early.slots = 0b1100; // bits 2 and 3 are hours 1001 and 1002 (bits 0 and 1 would be past)
        auto a = admit(book, early);
        ASSERT_TRUE(accepted(a));
        EXPECT_EQ(book.order({ Side::BID, a.rank }).slotMask, 0b11u);
        Order inside = bid(1, 2, 0, 1, { { 1, 1 } });
        inside.baseHour = H0 + 10;
        inside.slots = 0b1001; // hours 1011 and 1014
        a = admit(book, inside);
        ASSERT_TRUE(accepted(a));
        EXPECT_EQ(book.order({ Side::BID, a.rank }).slotMask, (1u << 10) | (1u << 13));
        Order last = bid(1, 3, 0, 1, { { 1, 1 } });
        last.baseHour = H0 + 40;
        last.slots = 0b1111111; // hours 1041 to 1047
        a = admit(book, last);
        ASSERT_TRUE(accepted(a));
        EXPECT_EQ(book.order({ Side::BID, a.rank }).slotMask, 0x7Full << 40);
    }
    {
        SCOPED_TRACE("a cancel removes a live order and leaves it readable but not live");
        BatchBook book = newBook();
        ASSERT_TRUE(accepted(admit(book, bid(1, 5, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(accepted(admit(book, ask(2, 5, 1, 1, { { 1, 50 } }))));
        EXPECT_EQ(book.arcs(), 4u);
        const std::optional<BatchBook::Ref> ref = book.cancel(1, 5);
        ASSERT_TRUE(ref.has_value());
        EXPECT_EQ(ref->side, Side::BID);
        EXPECT_EQ(ref->rank, 0u);
        EXPECT_FALSE(book.order(*ref).live);
        EXPECT_EQ(book.order(*ref).token, 5u) << "still readable";
        EXPECT_EQ(book.openOrders(Side::BID), 0u);
        EXPECT_EQ(book.submissions(Side::BID), 1u) << "the submission still counts";
        EXPECT_EQ(book.openOrders(Side::ASK), 1u);
        EXPECT_EQ(book.arcs(), 2u);
        EXPECT_EQ(book.buyer(1).openBids, 0u);
        EXPECT_EQ(book.buyer(1).submissions, 1u);
        EXPECT_EQ(book.buyer(1).arcs, 0u);
    }
    {
        SCOPED_TRACE("a cancel of an order that is not live finds nothing");
        BatchBook book = newBook();
        EXPECT_FALSE(book.cancel(1, 5).has_value()) << "no such order";
        ASSERT_TRUE(accepted(admit(book, bid(1, 5, 1, 1, { { 1, 50 } }))));
        EXPECT_FALSE(book.cancel(2, 5).has_value()) << "another participant's token";
        EXPECT_FALSE(book.cancel(1, 6).has_value()) << "another token";
        ASSERT_TRUE(book.cancel(1, 5).has_value());
        EXPECT_FALSE(book.cancel(1, 5).has_value()) << "already cancelled";
        EXPECT_EQ(book.openOrders(Side::BID), 0u);
        EXPECT_EQ(book.arcs(), 0u) << "a second cancel took nothing off again";
    }
    {
        SCOPED_TRACE("a cancel leaves a gap in the ranks and the next order takes the next number");
        BatchBook book = newBook();
        for (OrderToken t = 1; t <= 3; ++t) ASSERT_TRUE(accepted(admit(book, bid(t, 1, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(book.cancel(2, 1).has_value());
        const auto next = admit(book, bid(4, 1, 1, 1, { { 1, 50 } }));
        EXPECT_EQ(next.rank, 3u) << "not 1, and not 2";
        const ClearingInput input = book.clearingInput();
        ASSERT_EQ(input.bids.size(), 3u);
        EXPECT_EQ(input.bidRank, (std::vector<std::uint32_t>{ 0, 2, 3 })) << "the order that was cancelled left a gap";
        EXPECT_EQ(input.bids[0].id, 0u);
        EXPECT_EQ(input.bids[1].id, 2u);
        EXPECT_EQ(input.bids[2].id, 3u);
    }
}

TEST(BatchBook, ChecksTheStatelessRulesThenTheTokenThenTheHoursThenTheQuotaThenTheRoom) {
    {
        SCOPED_TRACE("an order that fails a stateless check is refused with the check's reason and changes nothing");
        BatchBook book = newBook();
        struct Case {
            const char* what;
            BatchCommand cmd;
        };
        std::vector<Case> cases;
        BatchCommand c = bid(1, 1, 1, 1, { { 1, 50 } }).command();
        c.side = static_cast<Side>('X');
        cases.push_back({ "side", c });
        c = bid(1, 1, 1, 1, { { 1, 50 } }).command();
        c.acceleratorMask = 0;
        cases.push_back({ "empty mask", c });
        c = bid(1, 1, 1, 1, { { 1, 50 } }).command();
        c.regionMask = 1u << 3;
        cases.push_back({ "bit beyond the catalog", c });
        cases.push_back({ "ask with two models", ask(1, 1, 1, 1, { { 1, 50 } }, A100 | H100).command() });
        c = bid(1, 1, 1, 1, { { 1, 50 } }).command();
        c.perSlotCap = 0;
        cases.push_back({ "cap", c });
        c = bid(1, 1, 1, 1, { { 1, 50 } }).command();
        c.stepCount = 0;
        cases.push_back({ "no steps", c });
        cases.push_back({ "bid price rises", bid(1, 1, 1, 1, { { 1, 50 }, { 1, 60 } }).command() });
        c = bid(1, 1, 1, 1, { { 1, 50 } }).command();
        c.steps[0].quantity = limits::MAX_ORDER_UNITS + 1;
        cases.push_back({ "quantity", c });
        for (const Case& k : cases) {
            const Reject expected = checkOrder(k.cmd);
            ASSERT_TRUE(expected) << k.what << " should fail the stateless checks";
            EXPECT_TRUE(refusedWith(book.admit(k.cmd), expected.reason, expected.detail)) << k.what;
        }
        EXPECT_EQ(book.submissions(Side::BID), 0u);
        EXPECT_EQ(book.submissions(Side::ASK), 0u);
        EXPECT_EQ(book.arcs(), 0u);
        EXPECT_EQ(book.buyer(1).submissions, 0u);
    }
    {
        SCOPED_TRACE("a stateless problem is named before any problem that needs the batch");
        BatchBook book = newBook();
        ASSERT_TRUE(accepted(admit(book, bid(1, 1, 1, 1, { { 1, 50 } }))));
        // This order has a token in use, hours that cannot be sold and a bad mask. The mask comes first.
        Order o = bid(1, 1, 1, 1, { { 1, 50 } });
        o.baseHour = 5;
        BatchCommand c = o.command();
        c.regionMask = 0;
        EXPECT_TRUE(refusedWith(book.admit(c), RejectReason::EMPTY_MASK, maskDetail(catalog::Attribute::REGION, 0)));
    }
    {
        SCOPED_TRACE("a token in use is refused, for the same participant only, whatever the side");
        BatchBook book = newBook();
        ASSERT_TRUE(accepted(admit(book, bid(1, 5, 1, 1, { { 1, 50 } }))));
        EXPECT_TRUE(refusedWith(admit(book, bid(1, 5, 1, 1, { { 1, 50 } })), RejectReason::TOKEN_IN_USE, 0));
        EXPECT_TRUE(accepted(admit(book, bid(2, 5, 1, 1, { { 1, 50 } })))) << "another participant may use the same token";
        EXPECT_TRUE(accepted(admit(book, bid(1, 6, 1, 1, { { 1, 50 } })))) << "and the same participant another token";
        EXPECT_EQ(book.submissions(Side::BID), 3u);
        // A cancel does not say the side, so a token is one name across both.
        EXPECT_TRUE(refusedWith(admit(book, ask(1, 5, 1, 1, { { 1, 50 } })), RejectReason::TOKEN_IN_USE, 0));
        EXPECT_EQ(book.submissions(Side::ASK), 0u);
    }
    {
        SCOPED_TRACE("a token is free again after a cancel");
        BatchBook book = newBook();
        ASSERT_TRUE(accepted(admit(book, bid(1, 5, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(book.cancel(1, 5).has_value());
        const auto again = admit(book, bid(1, 5, 1, 1, { { 1, 50 } }));
        ASSERT_TRUE(accepted(again));
        EXPECT_EQ(again.rank, 1u) << "a resubmission is a new submission";
    }
    {
        SCOPED_TRACE("a token in use is named before the hours, the quotas and the room");
        BookLimits tiny;
        tiny.maxOrdersPerSide = 1;
        tiny.maxOpenBidsPerBuyer = 1;
        BatchBook book = newBook(tiny);
        ASSERT_TRUE(accepted(admit(book, bid(1, 5, 1, 1, { { 1, 50 } }))));
        Order again = bid(1, 5, 1, 1, { { 1, 50 } });
        again.baseHour = 5; // past hours as well, and the buyer's quota and the batch are full
        EXPECT_TRUE(refusedWith(admit(book, again), RejectReason::TOKEN_IN_USE, 0));
    }
    {
        SCOPED_TRACE("the buyer's quotas are checked in the order open, submissions, arcs");
        BookLimits tiny;
        tiny.maxOpenBidsPerBuyer = 1;
        tiny.maxSubmissionsPerBuyer = 2;
        tiny.maxArcsPerBuyer = 3;
        BatchBook book = newBook(tiny);
        ASSERT_TRUE(accepted(admit(book, bid(1, 1, 1, 1, { { 1, 50 } })))); // 2 arcs, 1 open, 1 submission
        // A second bid breaks all three: open (2 > 1), submissions fine (2 <= 2) and arcs (4 > 3). Open is first.
        EXPECT_TRUE(refusedWith(admit(book, bid(1, 2, 1, 1, { { 1, 50 } })), RejectReason::PARTICIPANT_QUOTA, OPEN_ORDERS));
        ASSERT_TRUE(book.cancel(1, 1).has_value());
        ASSERT_TRUE(accepted(admit(book, bid(1, 2, 1, 1, { { 1, 50 } })))); // submission 2
        ASSERT_TRUE(book.cancel(1, 2).has_value());
        // Now submissions are used up (2 of 2), and this order has 5 arcs, over the 3 as well. Submissions first.
        EXPECT_TRUE(refusedWith(admit(book, wideBid(1, 3, 0b11, 0b01, 0b001, 2, 1)), RejectReason::PARTICIPANT_QUOTA,
                                SUBMISSIONS));
        // Another buyer with the open and submission quotas free but an order of 5 arcs: arcs.
        EXPECT_TRUE(refusedWith(admit(book, wideBid(2, 1, 0b11, 0b01, 0b001, 2, 1)), RejectReason::PARTICIPANT_QUOTA,
                                QUOTA_ARCS));
    }
    {
        SCOPED_TRACE("an order over the buyer's quota and in a full batch gets the quota reason");
        BookLimits tiny;
        tiny.maxOrdersPerSide = 3;
        tiny.maxOpenBidsPerBuyer = 2;
        BatchBook book = newBook(tiny);
        ASSERT_TRUE(accepted(admit(book, bid(1, 1, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(accepted(admit(book, bid(1, 2, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(accepted(admit(book, bid(2, 1, 1, 1, { { 1, 50 } }))));
        // The batch is full of bids (3 of 3) and buyer 1 is at their quota (2 of 2).
        EXPECT_TRUE(refusedWith(admit(book, bid(1, 3, 1, 1, { { 1, 50 } })), RejectReason::PARTICIPANT_QUOTA, OPEN_ORDERS));
        // A buyer inside their own quota meets the full batch.
        EXPECT_TRUE(refusedWith(admit(book, bid(3, 1, 1, 1, { { 1, 50 } })), RejectReason::BATCH_FULL, FULL_ORDERS));
    }
    {
        SCOPED_TRACE("the batch limits are checked in the order orders, submissions, arcs");
        BookLimits tiny;
        tiny.maxOrdersPerSide = 2;
        tiny.maxSubmissionsPerSide = 3;
        tiny.maxArcs = 5;
        BatchBook book = newBook(tiny);
        ASSERT_TRUE(accepted(admit(book, ask(1, 1, 1, 1, { { 1, 50 } })))); // 2 arcs
        ASSERT_TRUE(accepted(admit(book, ask(1, 2, 1, 1, { { 1, 50 } })))); // 4 arcs; 2 live, 2 submissions
        // A third breaks the orders (3 > 2), the submissions (3 <= 3 is fine) and the arcs (6 > 5): orders first.
        EXPECT_TRUE(refusedWith(admit(book, ask(1, 3, 1, 1, { { 1, 50 } })), RejectReason::BATCH_FULL, FULL_ORDERS));
        ASSERT_TRUE(book.cancel(1, 1).has_value());
        ASSERT_TRUE(accepted(admit(book, ask(1, 3, 1, 1, { { 1, 50 } })))); // 4 arcs; 2 live, 3 submissions
        ASSERT_TRUE(book.cancel(1, 3).has_value());
        // Orders have room (1 live), submissions are used up (3 of 3), and the arcs (2 + 2 = 4 <= 5) are fine.
        EXPECT_TRUE(refusedWith(admit(book, ask(1, 4, 1, 1, { { 1, 50 } })), RejectReason::BATCH_FULL, FULL_SUBMISSIONS));
        // The bids have their own counts, and here only the arcs are short: 2 live arcs + 4 > 5.
        EXPECT_TRUE(refusedWith(admit(book, wideBid(9, 1, 0b11, 0b01, 0b001, 1, 2)), RejectReason::BATCH_FULL, FULL_ARCS));
    }
    {
        SCOPED_TRACE("the limits are the settled ones by default, and one above them is refused at the start");
        const BookLimits defaults;
        EXPECT_EQ(defaults.maxOrdersPerSide, 10'000u);
        EXPECT_EQ(defaults.maxSubmissionsPerSide, 100'000u);
        EXPECT_EQ(defaults.maxArcs, 1'000'000u);
        EXPECT_EQ(defaults.maxOpenBidsPerBuyer, 20u);
        EXPECT_EQ(defaults.maxSubmissionsPerBuyer, 200u);
        EXPECT_EQ(defaults.maxArcsPerBuyer, 980u);
        EXPECT_EQ(defaults.maxOrdersPerSide, limits::MAX_ORDERS_PER_SIDE);
        EXPECT_EQ(defaults.maxSubmissionsPerSide, limits::MAX_SUBMISSIONS_PER_SIDE);
        EXPECT_EQ(defaults.maxArcs, limits::MAX_ARCS_PER_BATCH);
        EXPECT_EQ(defaults.maxOpenBidsPerBuyer, limits::MAX_OPEN_BIDS_PER_BUYER);
        EXPECT_EQ(defaults.maxSubmissionsPerBuyer, limits::MAX_SUBMISSIONS_PER_BUYER);
        EXPECT_EQ(defaults.maxArcsPerBuyer, limits::MAX_ARCS_PER_BUYER);

        // A limit above the settled one would let in a batch the clear refuses.
        BookLimits over;
        over.maxOrdersPerSide = limits::MAX_ORDERS_PER_SIDE + 1;
        EXPECT_THROW(newBook(over), std::invalid_argument);
        over = {};
        over.maxSubmissionsPerSide = limits::MAX_SUBMISSIONS_PER_SIDE + 1;
        EXPECT_THROW(newBook(over), std::invalid_argument);
        over = {};
        over.maxArcs = limits::MAX_ARCS_PER_BATCH + 1;
        EXPECT_THROW(newBook(over), std::invalid_argument);
        EXPECT_NO_THROW(newBook({}));
        over = {};
        over.maxArcs = 10;
        EXPECT_NO_THROW(newBook(over));
        EXPECT_THROW(BatchBook(schedule::Schedule{ 1800, 0 }), std::invalid_argument);
    }
}

TEST(BatchBook, SellsTheFortySevenHoursFromTheFirstHourOfTheBatchAndNoOthers) {
    {
        SCOPED_TRACE("the window is the 47 hours from the first hour");
        BatchBook book = newBook(); // 1001 to 1047
        auto at = [&](HourIndex hour, OrderToken token) {
            Order o = bid(1, token, 1, 1, { { 1, 50 } });
            o.baseHour = hour;
            return admit(book, o);
        };
        EXPECT_TRUE(refusedWith(at(H0 - 1, 1), RejectReason::SLOT_NOT_ELIGIBLE, 0)) << "hour 1000 has started";
        EXPECT_TRUE(accepted(at(H0, 2))) << "hour 1001 is the first";
        EXPECT_TRUE(accepted(at(H0 + 46, 3))) << "hour 1047 is the last";
        EXPECT_TRUE(refusedWith(at(H0 + 47, 4), RejectReason::SLOT_NOT_ELIGIBLE, 0)) << "hour 1048 is too far";
        EXPECT_TRUE(refusedWith(at(0, 5), RejectReason::SLOT_NOT_ELIGIBLE, 0));
        EXPECT_TRUE(refusedWith(at(std::numeric_limits<HourIndex>::max(), 6), RejectReason::SLOT_NOT_ELIGIBLE, 0));
    }
    {
        SCOPED_TRACE("the reject names the lowest bit of an hour that cannot be sold");
        BatchBook book = newBook(); // 1001 to 1047
        Order o = bid(1, 1, 0, 1, { { 1, 50 } });
        // Hours 998 (bit 0), 1000 (bit 2) are past, 1001 (bit 3) is fine: the lowest bad bit is 0.
        o.baseHour = H0 - 3;
        o.slots = 0b1101;
        EXPECT_TRUE(refusedWith(admit(book, o), RejectReason::SLOT_NOT_ELIGIBLE, 0));
        // Bit 0 off: now the lowest bad one is bit 2.
        o.slots = 0b1100;
        EXPECT_TRUE(refusedWith(admit(book, o), RejectReason::SLOT_NOT_ELIGIBLE, 2));
        // Only bit 3, which is hour 1001: fine.
        o.slots = 0b1000;
        EXPECT_TRUE(accepted(admit(book, o)));
        // At the far end: hours 1046, 1047 are fine, 1048 and 1049 are not. The lowest bad bit is 2.
        Order far = bid(1, 2, 0, 1, { { 1, 50 } });
        far.baseHour = H0 + 45;
        far.slots = 0b1111;
        EXPECT_TRUE(refusedWith(admit(book, far), RejectReason::SLOT_NOT_ELIGIBLE, 2));
        // The highest bit of the mask: bit 46 with a base hour of H0 + 1 is hour 1048.
        Order top = bid(1, 3, 0, 1, { { 1, 50 } });
        top.baseHour = H0 + 1;
        top.slots = std::uint64_t{ 1 } << 46;
        EXPECT_TRUE(refusedWith(admit(book, top), RejectReason::SLOT_NOT_ELIGIBLE, 46));
        top.baseHour = H0;
        top.token = 4;
        EXPECT_TRUE(accepted(admit(book, top))) << "bit 46 from the first hour is hour 1047";
    }
    {
        SCOPED_TRACE("a whole mask of 47 hours fits only at the first hour");
        BatchBook book = newBook();
        Order all = bid(1, 1, 0, 1, { { 1, 50 } });
        all.slots = (std::uint64_t{ 1 } << 47) - 1;
        all.baseHour = H0;
        EXPECT_TRUE(accepted(admit(book, all)));
        all.token = 2;
        all.baseHour = H0 + 1;
        EXPECT_TRUE(refusedWith(admit(book, all), RejectReason::SLOT_NOT_ELIGIBLE, 46));
        all.baseHour = H0 - 1;
        EXPECT_TRUE(refusedWith(admit(book, all), RejectReason::SLOT_NOT_ELIGIBLE, 0));
    }
    {
        SCOPED_TRACE("the window follows the batch id");
        BatchBook book = newBook({}, 3); // sells 1004 to 1050
        Order o = bid(1, 1, 1, 1, { { 1, 50 } });
        o.baseHour = H0 + 2; // hour 1003
        EXPECT_TRUE(refusedWith(admit(book, o), RejectReason::SLOT_NOT_ELIGIBLE, 0));
        o.baseHour = H0 + 3;
        EXPECT_TRUE(accepted(admit(book, o)));
        Order top = bid(1, 2, 1, 1, { { 1, 50 } });
        top.baseHour = H0 + 49; // hour 1050
        EXPECT_TRUE(accepted(admit(book, top)));
        top.token = 3;
        top.baseHour = H0 + 50;
        EXPECT_TRUE(refusedWith(admit(book, top), RejectReason::SLOT_NOT_ELIGIBLE, 0));
    }
    {
        SCOPED_TRACE("an hour beyond what an event can carry is never sold");
        // A window at the very top of the 32-bit hours: 4,294,967,292 to 4,294,967,295 is the end of it.
        const std::uint64_t top = std::numeric_limits<HourIndex>::max();
        const schedule::Schedule s{ (top - 4) * 3600 + 1800, 3600 }; // sells from top - 3
        BatchBook book(s, {}, 0);
        ASSERT_EQ(book.firstHour(), top - 3);
        Order o = bid(1, 1, 0, 1, { { 1, 50 } });
        o.baseHour = static_cast<HourIndex>(top - 3);
        o.slots = 0b1111; // top-3 .. top
        EXPECT_TRUE(accepted(admit(book, o)));
        o.token = 2;
        o.slots = 0b11111; // one more: hour 2^32, which a 32-bit hour cannot name and must not wrap to 0
        EXPECT_TRUE(refusedWith(admit(book, o), RejectReason::SLOT_NOT_ELIGIBLE, 4));
    }
}

TEST(BatchBook, EnforcesTheSettledQuotasAndBatchLimitsExactlyAtTheirBoundaries) {
    {
        SCOPED_TRACE("a buyer may hold twenty open bids and not twenty-one");
        BatchBook book = newBook();
        for (OrderToken t = 1; t <= 20; ++t) ASSERT_TRUE(accepted(admit(book, bid(1, t, 1, 1, { { 1, 50 } })))) << t;
        EXPECT_EQ(book.buyer(1).openBids, 20u);
        EXPECT_TRUE(refusedWith(admit(book, bid(1, 21, 1, 1, { { 1, 50 } })), RejectReason::PARTICIPANT_QUOTA, OPEN_ORDERS));
        EXPECT_TRUE(accepted(admit(book, bid(2, 1, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(book.cancel(1, 7).has_value());
        EXPECT_EQ(book.buyer(1).openBids, 19u);
        EXPECT_TRUE(accepted(admit(book, bid(1, 21, 1, 1, { { 1, 50 } }))));
    }
    {
        SCOPED_TRACE("a buyer may make two hundred submissions and cancelling does not give them back");
        BatchBook book = newBook();
        for (OrderToken t = 1; t <= 200; ++t) {
            ASSERT_TRUE(accepted(admit(book, bid(1, t, 1, 1, { { 1, 50 } })))) << t;
            ASSERT_TRUE(book.cancel(1, t).has_value());
        }
        EXPECT_EQ(book.buyer(1).openBids, 0u);
        EXPECT_EQ(book.buyer(1).submissions, 200u);
        EXPECT_TRUE(refusedWith(admit(book, bid(1, 201, 1, 1, { { 1, 50 } })), RejectReason::PARTICIPANT_QUOTA, SUBMISSIONS));
        EXPECT_TRUE(accepted(admit(book, bid(2, 1, 1, 1, { { 1, 50 } })))) << "another buyer has their own";
    }
    {
        SCOPED_TRACE("a buyer's arcs are capped at 980 over their live bids, exactly");
        BatchBook book = newBook();
        // 5 models x 2 links x 3 regions x 30 hours = 900 items, and 4 steps: 904 arcs.
        ASSERT_TRUE(accepted(admit(book, wideBid(1, 1, 0b11111, 0b11, 0b111, 30, 4))));
        EXPECT_EQ(book.buyer(1).arcs, 904u);
        // 5 x 1 x 3 x 5 = 75 items and 2 steps: 77 arcs, which is one over.
        EXPECT_TRUE(refusedWith(admit(book, wideBid(1, 2, 0b11111, 0b01, 0b111, 5, 2)), RejectReason::PARTICIPANT_QUOTA,
                                QUOTA_ARCS));
        // 1 x 2 x 3 x 12 = 72 items and 4 steps: 76 arcs, which makes exactly 980.
        ASSERT_TRUE(accepted(admit(book, wideBid(1, 3, 0b00001, 0b11, 0b111, 12, 4))));
        EXPECT_EQ(book.buyer(1).arcs, 980u);
        // Nothing more fits: the smallest bid is one item and one step.
        EXPECT_TRUE(refusedWith(admit(book, wideBid(1, 4, 0b00001, 0b01, 0b001, 1, 1)), RejectReason::PARTICIPANT_QUOTA,
                                QUOTA_ARCS));
        ASSERT_TRUE(book.cancel(1, 1).has_value());
        EXPECT_EQ(book.buyer(1).arcs, 76u);
        EXPECT_TRUE(accepted(admit(book, wideBid(1, 5, 0b11111, 0b01, 0b111, 5, 2))));
        EXPECT_TRUE(accepted(admit(book, wideBid(2, 1, 0b11111, 0b11, 0b111, 30, 4))));
    }
    {
        SCOPED_TRACE("a single bid larger than the buyer's whole quota is never accepted");
        BatchBook book = newBook();
        // Every model, link, region and hour: 1,410 items, more than 980 arcs on its own.
        EXPECT_TRUE(refusedWith(admit(book, wideBid(1, 1, 0b11111, 0b11, 0b111, 47, 1)), RejectReason::PARTICIPANT_QUOTA,
                                QUOTA_ARCS));
        EXPECT_EQ(book.submissions(Side::BID), 0u);
    }
    {
        SCOPED_TRACE("a quota is a buyer's, and a seller has none");
        BatchBook book = newBook();
        // One seller posts 40 live asks, twice a buyer's 20 open bids, each with the largest ask (47 hours).
        for (OrderToken t = 1; t <= 40; ++t) {
            Order o = ask(5, t, (std::uint64_t{ 1 } << 47) - 1, 1, { { 1, 10 }, { 1, 20 }, { 1, 30 }, { 1, 40 } });
            ASSERT_TRUE(accepted(admit(book, o))) << t;
        }
        EXPECT_EQ(book.openOrders(Side::ASK), 40u);
        EXPECT_EQ(book.arcs(), 40u * 51u) << "an ask is at most 51 arcs: 47 items and 4 steps";
        EXPECT_EQ(book.buyer(5).openBids, 0u) << "the same participant as a buyer has the quota of a buyer";
    }
    {
        SCOPED_TRACE("a side holds ten thousand live orders and the next is refused");
        BatchBook book = newBook();
        // 500 buyers with 20 bids each: the most that the quotas let into a batch, and exactly the side's room.
        for (ParticipantId buyer = 0; buyer < 500; ++buyer) {
            for (OrderToken t = 1; t <= 20; ++t) {
                ASSERT_TRUE(accepted(admit(book, bid(buyer, t, 1, 1, { { 1, 50 } })))) << buyer << "/" << t;
            }
        }
        EXPECT_EQ(book.openOrders(Side::BID), 10'000u);
        // A buyer who has nothing yet meets the full batch.
        EXPECT_TRUE(refusedWith(admit(book, bid(1000, 1, 1, 1, { { 1, 50 } })), RejectReason::BATCH_FULL, FULL_ORDERS));
        // The other side has its own room. (A seller with an id of their own: a token is one name for a
        // participant whatever the side, and buyer 1 holds the token 1.)
        EXPECT_TRUE(accepted(admit(book, ask(5000, 1, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(book.cancel(3, 4).has_value());
        EXPECT_TRUE(accepted(admit(book, bid(1000, 1, 1, 1, { { 1, 50 } }))));
    }
    {
        SCOPED_TRACE("a side takes one hundred thousand submissions and cancelling does not make room");
        {
            // Bids: 500 buyers, 200 submissions each, one live at a time.
            BatchBook book = newBook();
            for (ParticipantId buyer = 0; buyer < 500; ++buyer) {
                for (OrderToken t = 1; t <= 200; ++t) {
                    ASSERT_TRUE(accepted(admit(book, bid(buyer, t, 1, 1, { { 1, 50 } })))) << buyer << "/" << t;
                    ASSERT_TRUE(book.cancel(buyer, t).has_value());
                }
            }
            EXPECT_EQ(book.submissions(Side::BID), 100'000u);
            EXPECT_EQ(book.openOrders(Side::BID), 0u);
            EXPECT_TRUE(refusedWith(admit(book, bid(1000, 1, 1, 1, { { 1, 50 } })), RejectReason::BATCH_FULL,
                                    FULL_SUBMISSIONS));
            EXPECT_TRUE(accepted(admit(book, ask(5000, 1, 1, 1, { { 1, 50 } })))) << "the asks have their own";
        }
        {
            // Asks: one seller, no quota.
            BatchBook book = newBook();
            for (OrderToken t = 1; t <= 100'000; ++t) {
                ASSERT_TRUE(accepted(admit(book, ask(1, t, 1, 1, { { 1, 50 } })))) << t;
                ASSERT_TRUE(book.cancel(1, t).has_value());
            }
            EXPECT_TRUE(refusedWith(admit(book, ask(1, 100'001, 1, 1, { { 1, 50 } })), RejectReason::BATCH_FULL,
                                    FULL_SUBMISSIONS));
        }
    }
    {
        SCOPED_TRACE("a full batch holds exactly one million arcs and not one more");
        // The batch the quotas are sized for: 500 buyers using all 980 arcs each, and 10,000 asks of 51 arcs each.
        BatchBook book = newBook();
        for (ParticipantId buyer = 0; buyer < 500; ++buyer) {
            ASSERT_TRUE(accepted(admit(book, wideBid(buyer, 1, 0b11111, 0b11, 0b111, 30, 4)))); // 904 arcs
            ASSERT_TRUE(accepted(admit(book, wideBid(buyer, 2, 0b00001, 0b11, 0b111, 12, 4)))); // 76 arcs
            EXPECT_EQ(book.buyer(buyer).arcs, 980u);
        }
        EXPECT_EQ(book.arcs(), 490'000u);
        for (std::uint32_t seller = 0; seller < 10'000; ++seller) {
            // Their own ids, so that no token is in use by a buyer with the same number.
            Order o = ask(100'000 + seller, 1, (std::uint64_t{ 1 } << 47) - 1, 1,
                          { { 1, 10 }, { 1, 20 }, { 1, 30 }, { 1, 40 } });
            ASSERT_TRUE(accepted(admit(book, o))) << seller;
        }
        EXPECT_EQ(book.arcs(), limits::MAX_ARCS_PER_BATCH) << "exactly one million";
        EXPECT_EQ(book.openOrders(Side::ASK), 10'000u);
        // A 501st buyer, who is not meant to exist, meets the full batch with the smallest bid.
        EXPECT_TRUE(refusedWith(admit(book, wideBid(5000, 1, 1, 1, 1, 1, 1)), RejectReason::BATCH_FULL, FULL_ARCS));
        ASSERT_TRUE(book.cancel(0, 2).has_value());
        EXPECT_EQ(book.arcs(), limits::MAX_ARCS_PER_BATCH - 76);
        EXPECT_TRUE(accepted(admit(book, wideBid(5000, 1, 0b00001, 0b11, 0b111, 12, 4)))) << "76 arcs fit exactly";
        EXPECT_TRUE(refusedWith(admit(book, wideBid(5000, 2, 1, 1, 1, 1, 1)), RejectReason::BATCH_FULL, FULL_ARCS));
    }
}

TEST(BatchBook, HandsTheClearTheFullBoxOfEveryLiveOrderAndStartsTheNextBatchWhateverTheIdWas) {
    {
        SCOPED_TRACE("the items of an order are its box in ascending order");
        BatchBook book = newBook();
        // A100 or H100 over NVLink in US_WEST, in the first and the third hour of the window.
        Order o = bid(1, 1, 0b101, 1, { { 3, 50 } }, A100 | H100, NVLINK, US_WEST);
        o.cap = 2;
        ASSERT_TRUE(accepted(admit(book, o)));
        const ClearingInput input = book.clearingInput();
        ASSERT_EQ(input.bids.size(), 1u);
        const ClearingOrder& c = input.bids[0];
        // SKU index = (accelerator * 2 + interconnect) * 3 + region: A100/NVLink/West is 3, H100/NVLink/West is 9.
        // Item = hour * 30 + SKU.
        EXPECT_EQ(c.items, (std::vector<ItemIndex>{ 3, 9, 63, 69 }));
        EXPECT_EQ(c.perSlotCap, 2u);
        EXPECT_EQ(c.stepCount, 1u);
        EXPECT_EQ(c.steps[0].quantity, 3u);
        EXPECT_EQ(c.steps[0].price, 50u);
    }
    {
        SCOPED_TRACE("an ask offers one item for each hour");
        BatchBook book = newBook();
        // H200 over PCIe in EU_WEST for hours 0, 4 and 46 of the window.
        Order o = ask(1, 1, 0b1 | (1u << 4) | (std::uint64_t{ 1 } << 46), 5, { { 8, 10 }, { 8, 20 } }, H200, PCIE, EU_WEST);
        ASSERT_TRUE(accepted(admit(book, o)));
        const ClearingInput input = book.clearingInput();
        ASSERT_EQ(input.asks.size(), 1u);
        // H200/PCIe/EU_WEST is SKU (2 * 2 + 0) * 3 + 2 = 14.
        EXPECT_EQ(input.asks[0].items, (std::vector<ItemIndex>{ 14, 4 * 30 + 14, 46 * 30 + 14 }));
        EXPECT_EQ(input.asks[0].perSlotCap, 5u);
        EXPECT_EQ(input.asks[0].stepCount, 2u);
        EXPECT_EQ(input.askRank, (std::vector<std::uint32_t>{ 0 }));
    }
    {
        SCOPED_TRACE("the clear gets every item an order accepts, offered or not, and exactly the arcs the book counts");
        BatchBook book = newBook();
        ASSERT_TRUE(accepted(admit(book, wideBid(1, 1, 0b11111, 0b11, 0b111, 30, 4))));
        ASSERT_TRUE(accepted(admit(book, wideBid(2, 1, 0b00110, 0b10, 0b011, 7, 3))));
        ASSERT_TRUE(accepted(admit(book, ask(3, 1, 0b1111, 4, { { 8, 10 }, { 8, 20 } }, H100, NVLINK, US_WEST))));
        ASSERT_TRUE(accepted(admit(book, ask(4, 1, 0b11, 1, { { 8, 10 } }, MI300X, PCIE, EU_WEST))));
        ASSERT_TRUE(book.cancel(2, 1).has_value());
        const ClearingInput input = book.clearingInput();
        std::uint64_t arcs = 0;
        for (const ClearingOrder& o : input.bids) arcs += arcsOf(o);
        for (const ClearingOrder& o : input.asks) arcs += arcsOf(o);
        EXPECT_EQ(arcs, book.arcs()) << "the admission count is the size of the graph";
        EXPECT_EQ(arcs, 904u + 6u + 3u);
        EXPECT_NO_THROW(validateInput(input)) << "the book only builds batches that the clear accepts";
        ASSERT_EQ(input.bids.size(), 1u);
        EXPECT_EQ(input.bids[0].items.size(), 900u);
        for (std::size_t k = 1; k < input.bids[0].items.size(); ++k) {
            EXPECT_LT(input.bids[0].items[k - 1], input.bids[0].items[k]) << "ascending, so without repeats";
        }
        EXPECT_LT(input.bids[0].items.back(), 1410u);
    }
    {
        SCOPED_TRACE("a batch with only one side still makes an input the clear accepts");
        BatchBook book = newBook();
        ASSERT_TRUE(accepted(admit(book, bid(1, 1, 1, 1, { { 1, 50 } }))));
        const ClearingInput input = book.clearingInput();
        EXPECT_EQ(input.bids.size(), 1u);
        EXPECT_TRUE(input.asks.empty());
        EXPECT_TRUE(input.askRank.empty());
        EXPECT_NO_THROW(validateInput(input));
        EXPECT_NO_THROW(clear(input));
    }
    {
        SCOPED_TRACE("when a batch is over every order lapses, every count starts again and the next batch is one later");
        BatchBook book = newBook();
        ASSERT_TRUE(accepted(admit(book, bid(1, 1, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(accepted(admit(book, ask(2, 1, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(accepted(admit(book, bid(1, 2, 1, 1, { { 1, 50 } }))));
        ASSERT_TRUE(book.cancel(1, 2).has_value());

        book.startBatchAfter(0);
        EXPECT_EQ(book.batchId(), 1u);
        EXPECT_EQ(book.firstHour(), 1002u) << "an hour later";
        EXPECT_EQ(book.openOrders(Side::BID), 0u);
        EXPECT_EQ(book.openOrders(Side::ASK), 0u);
        EXPECT_EQ(book.submissions(Side::BID), 0u);
        EXPECT_EQ(book.submissions(Side::ASK), 0u);
        EXPECT_EQ(book.arcs(), 0u);
        EXPECT_EQ(book.buyer(1).submissions, 0u);
        EXPECT_EQ(book.buyer(1).openBids, 0u);
        EXPECT_TRUE(book.orders(Side::BID).empty());
        EXPECT_FALSE(book.cancel(1, 1).has_value()) << "the order lapsed";

        // The window has moved on by an hour.
        Order next = bid(1, 1, 1, 1, { { 1, 50 } });
        next.baseHour = H0 + 1;
        const auto again = admit(book, next);
        ASSERT_TRUE(accepted(again)) << "the token of a lapsed order may be used again";
        EXPECT_EQ(again.rank, 0u);
    }
    {
        SCOPED_TRACE("the next batch follows the one that was cleared, whatever its id was");
        BatchBook book = newBook();
        book.startBatchAfter(7); // the clear carried id 7 although batch 0 was open
        EXPECT_EQ(book.batchId(), 8u);
        EXPECT_EQ(book.firstHour(), 1009u);
        book.startBatchAfter(2); // a rewound id
        EXPECT_EQ(book.batchId(), 3u);
        EXPECT_EQ(book.firstHour(), 1004u);
        book.startBatchAfter(2); // the same id again
        EXPECT_EQ(book.batchId(), 3u);
    }
    {
        SCOPED_TRACE("an id at the top stops there and sells nothing until a smaller one comes");
        const BatchId top = std::numeric_limits<BatchId>::max();
        BatchBook book = newBook();
        book.startBatchAfter(top);
        EXPECT_EQ(book.batchId(), top) << "not 0";
        EXPECT_GT(book.firstHour(), std::uint64_t{ std::numeric_limits<HourIndex>::max() } + 64);
        EXPECT_TRUE(refusedWith(admit(book, bid(1, 1, 1, 1, { { 1, 50 } })), RejectReason::SLOT_NOT_ELIGIBLE, 0));
        Order wide = bid(1, 2, 1, 1, { { 1, 50 } });
        wide.baseHour = std::numeric_limits<HourIndex>::max();
        wide.slots = 0b111111;
        EXPECT_TRUE(refusedWith(admit(book, wide), RejectReason::SLOT_NOT_ELIGIBLE, 0));
        book.startBatchAfter(5);
        EXPECT_EQ(book.firstHour(), 1007u);
        Order later = bid(1, 3, 1, 1, { { 1, 50 } });
        later.baseHour = 1007;
        EXPECT_TRUE(accepted(admit(book, later)));
    }
}
