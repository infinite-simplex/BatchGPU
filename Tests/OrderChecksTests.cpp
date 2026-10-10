#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "BatchCommand.hpp"
#include "Catalog.hpp"
#include "ClearingTypes.hpp"
#include "ClearingValidation.hpp"
#include "Limits.hpp"
#include "OrderChecks.hpp"
#include "OrderFixtures.hpp"
#include "RejectReason.hpp"

using catalog::Attribute;

namespace {

constexpr std::uint64_t ONE = 1;

constexpr BatchCommand smallestBid() {
    BatchCommand c{};
    c.side = Side::BID;
    c.acceleratorMask = 1;
    c.interconnectMask = 1;
    c.regionMask = 1;
    c.slotMask = 1;
    c.perSlotCap = 1;
    c.stepCount = 1;
    c.steps[0] = { 1, 1 };
    return c;
}
static_assert(!checkOrder(smallestBid()));
static_assert(checkOrder(BatchCommand{}).reason == RejectReason::BAD_SIDE);

void setMask(BatchCommand& c, Attribute a, std::uint64_t value) {
    switch (a) {
    case Attribute::ACCELERATOR: c.acceleratorMask = static_cast<std::uint32_t>(value); break;
    case Attribute::INTERCONNECT: c.interconnectMask = static_cast<std::uint8_t>(value); break;
    case Attribute::REGION: c.regionMask = static_cast<std::uint32_t>(value); break;
    case Attribute::SLOTS: c.slotMask = value; break;
    }
}

std::size_t fieldBits(Attribute a) {
    switch (a) {
    case Attribute::ACCELERATOR: return catalog::ACCELERATOR_MASK_BITS;
    case Attribute::INTERCONNECT: return catalog::INTERCONNECT_MASK_BITS;
    case Attribute::REGION: return catalog::REGION_MASK_BITS;
    case Attribute::SLOTS: return catalog::SLOT_MASK_BITS;
    }
    return 0;
}

const Attribute ALL_ATTRIBUTES[] = { Attribute::ACCELERATOR, Attribute::INTERCONNECT, Attribute::REGION,
                                     Attribute::SLOTS };

BatchCommand withCurve(BatchCommand base, std::initializer_list<CurveStep> steps) {
    base.stepCount = static_cast<std::uint8_t>(steps.size());
    std::size_t k = 0;
    for (const CurveStep& s : steps) base.steps[k++] = s;
    return base;
}

Reject expectReject(const BatchCommand& c, RejectReason reason, std::uint16_t detail) {
    const Reject r = checkOrder(c);
    EXPECT_EQ(r.reason, reason) << "got " << toString(r.reason) << " detail " << r.detail << ", wanted "
                                << toString(reason) << " detail " << detail;
    EXPECT_EQ(r.detail, detail) << "the detail of " << toString(reason);
    return r;
}

struct Case {
    std::string what;
    BatchCommand order;
    Reject expected; // none when the order passes
};

std::vector<Case> boundaryCases() {
    std::vector<Case> cases;
    auto passes = [&](std::string what, const BatchCommand& c) { cases.push_back({ std::move(what), c, {} }); };
    auto refused = [&](std::string what, const BatchCommand& c, RejectReason reason, std::uint16_t detail) {
        cases.push_back({ std::move(what), c, { reason, detail } });
    };
    const BatchCommand bid = validBid();
    const BatchCommand ask = validAsk();
    constexpr auto MAX_UNITS = limits::MAX_ORDER_UNITS;
    constexpr auto MAX_PRICE = limits::MAX_PRICE_TICKS;
    passes("the design's bid", bid);
    passes("the design's ask", ask);

    // 'S' is the LOB's sell side, and 'b' and 'a' are the wrong case.
    for (const int byte : std::initializer_list<int>{ 0, 1, 'S', 'b', 'a', 0x43, 0xFF }) {
        BatchCommand c = bid;
        c.side = static_cast<Side>(byte);
        refused("side byte " + std::to_string(byte), c, RejectReason::BAD_SIDE, static_cast<std::uint16_t>(byte));
    }

    for (const Attribute a : ALL_ATTRIBUTES) {
        const std::string name = "attribute " + std::to_string(static_cast<int>(a));
        const std::size_t options = catalog::optionCount(a);
        BatchCommand c = bid;
        setMask(c, a, 0);
        refused(name + " empty", c, RejectReason::EMPTY_MASK, maskDetail(a, 0));
        setMask(c, a, ONE << (options - 1));
        passes(name + " last valid bit alone", c);
        setMask(c, a, catalog::validBits(options));
        passes(name + " every valid bit: a bid may name many values", c);
        for (std::size_t bad = options; bad < fieldBits(a); ++bad) {
            setMask(c, a, (ONE << bad) | 1);
            refused(name + " bit " + std::to_string(bad), c, RejectReason::MASK_BIT_OUT_OF_RANGE,
                    maskDetail(a, static_cast<unsigned>(bad)));
        }
        setMask(c, a, (ONE << (fieldBits(a) - 1)) | (ONE << options) | 1);
        refused(name + " several bad bits: the lowest is named", c, RejectReason::MASK_BIT_OUT_OF_RANGE,
                maskDetail(a, static_cast<unsigned>(options)));
    }
    {
        // The window is 47 slots, written as numbers so that this does not depend on the catalog's constant.
        BatchCommand c = bid;
        c.slotMask = ONE << 46;
        passes("slot 46 is the last hour of the window", c);
        c.slotMask = (ONE << 47) - 1;
        passes("the whole window", c);
        c.slotMask = ONE << 47;
        refused("slot 47", c, RejectReason::MASK_BIT_OUT_OF_RANGE, maskDetail(Attribute::SLOTS, 47));
        c.slotMask = ONE << 63;
        refused("slot 63", c, RejectReason::MASK_BIT_OUT_OF_RANGE, maskDetail(Attribute::SLOTS, 63));
    }

    for (const Attribute a : { Attribute::ACCELERATOR, Attribute::INTERCONNECT, Attribute::REGION }) {
        BatchCommand c = ask;
        setMask(c, a, 0b11);
        refused("ask with two values of attribute " + std::to_string(static_cast<int>(a)), c,
                RejectReason::ASK_MASK_NOT_SINGLE, maskDetail(a, 0));
    }
    for (const std::uint64_t slots : { (ONE << 47) - 1, std::uint64_t{ 0b10101 } }) {
        BatchCommand c = ask;
        c.slotMask = slots;
        passes("an ask may offer several hours", c);
    }

    for (const BatchCommand& base : { bid, ask }) {
        const std::string side = base.side == Side::BID ? "bid" : "ask";
        BatchCommand c = base;
        c.perSlotCap = 0;
        refused(side + " cap 0", c, RejectReason::PER_SLOT_CAP_OUT_OF_RANGE, 0);
        c.perSlotCap = 1;
        passes(side + " cap 1", c);
        c.perSlotCap = limits::MAX_PER_SLOT_CAP;
        passes(side + " the largest cap", c);
        c.perSlotCap = limits::MAX_PER_SLOT_CAP + 1;
        refused(side + " one over", c, RejectReason::PER_SLOT_CAP_OUT_OF_RANGE, 0);
        c.perSlotCap = 0xFFFFFFFFu;
        refused(side + " cap 2^32 - 1", c, RejectReason::PER_SLOT_CAP_OUT_OF_RANGE, 0);
    }

    for (const std::uint8_t count : { 0, 5, 6, 100, 255 }) {
        BatchCommand c = withCurve(bid, { { 1, 50 }, { 1, 40 }, { 1, 30 }, { 1, 20 } });
        c.stepCount = count;
        refused("step count " + std::to_string(count), c, RejectReason::STEP_COUNT_OUT_OF_RANGE, count);
    }
    for (std::uint8_t count = 1; count <= limits::MAX_STEPS; ++count) {
        BatchCommand c = withCurve(bid, { { 1, 50 }, { 1, 40 }, { 1, 30 }, { 1, 20 } });
        c.stepCount = count;
        passes("a curve of " + std::to_string(count) + " steps", c);
    }
    for (std::uint8_t k = 0; k < limits::MAX_STEPS; ++k) {
        BatchCommand c = withCurve(bid, { { 1, 40 }, { 1, 30 }, { 1, 20 }, { 1, 10 } });
        c.steps[k].quantity = 0;
        refused("zero quantity at step " + std::to_string(k), c, RejectReason::STEP_QUANTITY_ZERO, k);
    }
    for (const BatchCommand& base : { bid, ask }) {
        passes("the maximum price", withCurve(base, { { 1, MAX_PRICE } }));
        refused("one tick over the maximum price", withCurve(base, { { 1, MAX_PRICE + 1 } }),
                RejectReason::STEP_PRICE_OVER_MAX, 0);
    }
    refused("a price over the maximum at step 1 of an ask", withCurve(ask, { { 1, 100 }, { 1, MAX_PRICE + 5 } }),
            RejectReason::STEP_PRICE_OVER_MAX, 1);
    refused("quantity over the maximum at step 1", withCurve(bid, { { 1, 50 }, { MAX_UNITS + 1, 40 } }),
            RejectReason::STEP_QUANTITY_OVER_MAX, 1);
    refused("quantity over the maximum at step 0", withCurve(bid, { { MAX_UNITS + 1, 50 }, { 1, 40 } }),
            RejectReason::STEP_QUANTITY_OVER_MAX, 0);
    refused("quantity 2^32 - 1", withCurve(bid, { { 0xFFFFFFFFu, 50 } }), RejectReason::STEP_QUANTITY_OVER_MAX, 0);
    // A bid's prices may not rise, and an ask's may not fall; equal is allowed.
    passes("bid prices 30, 30, 20", withCurve(bid, { { 1, 30 }, { 1, 30 }, { 1, 20 } }));
    refused("bid price rises at step 1", withCurve(bid, { { 1, 30 }, { 1, 31 }, { 1, 20 } }),
            RejectReason::STEP_PRICE_ORDER, 1);
    refused("bid price rises at step 2", withCurve(bid, { { 1, 30 }, { 1, 30 }, { 1, 31 } }),
            RejectReason::STEP_PRICE_ORDER, 2);
    passes("ask prices 20, 20, 30", withCurve(ask, { { 1, 20 }, { 1, 20 }, { 1, 30 } }));
    refused("ask price falls at step 1", withCurve(ask, { { 1, 20 }, { 1, 19 }, { 1, 30 } }),
            RejectReason::STEP_PRICE_ORDER, 1);
    refused("ask price falls at step 2", withCurve(ask, { { 1, 20 }, { 1, 20 }, { 1, 19 } }),
            RejectReason::STEP_PRICE_ORDER, 2);
    // The steps past the count are not looked at, and the first step has no previous price to break.
    {
        BatchCommand c = withCurve(bid, { { 1, 50 }, { 1, 40 } });
        c.steps[2] = { 0, 0xFFFFFFFFu };
        c.steps[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
        passes("garbage past the step count", c);
    }
    passes("a bid at price zero", withCurve(bid, { { 1, 0 } }));
    passes("an ask at price zero", withCurve(ask, { { 1, 0 } }));

    passes("one step of the maximum quantity", withCurve(bid, { { MAX_UNITS, 100 } }));
    const BatchCommand quarters = withCurve(bid, { { MAX_UNITS / 4, 40 },
                                                  { MAX_UNITS / 4, 30 },
                                                  { MAX_UNITS / 4, 20 },
                                                  { MAX_UNITS - 3 * (MAX_UNITS / 4), 10 } });
    passes("four steps that add up to the maximum exactly", quarters);
    BatchCommand over = quarters;
    over.steps[3].quantity += 1;
    refused("four steps one over the maximum", over, RejectReason::TOTAL_QUANTITY_OVER_MAX, 3);
    refused("the running total first passes the maximum at step 2",
            withCurve(bid, { { MAX_UNITS / 2, 40 }, { MAX_UNITS / 2, 30 }, { 1, 20 }, { 1, 10 } }),
            RejectReason::TOTAL_QUANTITY_OVER_MAX, 2);
    return cases;
}

} // namespace

TEST(OrderChecks, EachCheckPassesAndRefusesAtItsBoundaryAndNamesTheProblem) {
    const std::vector<Case> cases = boundaryCases();
    ASSERT_GT(cases.size(), 100u);
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        const Reject r = checkOrder(c.order);
        EXPECT_EQ(r.reason, c.expected.reason) << "got " << toString(r.reason) << ", wanted " << toString(c.expected.reason);
        EXPECT_EQ(r.detail, c.expected.detail);
    }
}

TEST(OrderChecks, ProblemsAreReportedInTheOrderOfTheChecks) {
    // Between the attributes: accelerator, interconnect, region, slots.
    BatchCommand c = validBid();
    c.slotMask = 0;
    c.regionMask = 0b1000;
    c.interconnectMask = 0;
    c.acceleratorMask = 1u << 5;
    expectReject(c, RejectReason::MASK_BIT_OUT_OF_RANGE, maskDetail(Attribute::ACCELERATOR, 5));
    c.acceleratorMask = 1;
    expectReject(c, RejectReason::EMPTY_MASK, maskDetail(Attribute::INTERCONNECT, 0));
    c.interconnectMask = 1;
    expectReject(c, RejectReason::MASK_BIT_OUT_OF_RANGE, maskDetail(Attribute::REGION, 3));
    c.regionMask = 1;
    expectReject(c, RejectReason::EMPTY_MASK, maskDetail(Attribute::SLOTS, 0));
    c.slotMask = 1;
    EXPECT_FALSE(checkOrder(c));

    // An ask that names too many of every attribute: the first is the one named.
    c = validAsk();
    c.regionMask = 0b11;
    c.interconnectMask = 0b11;
    c.acceleratorMask = 0b11;
    expectReject(c, RejectReason::ASK_MASK_NOT_SINGLE, maskDetail(Attribute::ACCELERATOR, 0));
    c.acceleratorMask = 0b100;
    expectReject(c, RejectReason::ASK_MASK_NOT_SINGLE, maskDetail(Attribute::INTERCONNECT, 0));
    c.interconnectMask = 0b01;
    expectReject(c, RejectReason::ASK_MASK_NOT_SINGLE, maskDetail(Attribute::REGION, 0));

    // An empty or out-of-range mask is reported before the rule that an ask is single.
    c = validAsk();
    c.acceleratorMask = 0;
    expectReject(c, RejectReason::EMPTY_MASK, maskDetail(Attribute::ACCELERATOR, 0));
    c.acceleratorMask = 0b100000;
    expectReject(c, RejectReason::MASK_BIT_OUT_OF_RANGE, maskDetail(Attribute::ACCELERATOR, 5));

    // Between the steps: the first bad step is the one named, whatever is wrong with the later ones.
    c = withCurve(validBid(), { { 1, 40 }, { 1, 50 }, { 0, 30 }, { 1, 20 } }); // a rise at step 1, a zero at step 2
    expectReject(c, RejectReason::STEP_PRICE_ORDER, 1);
    c.steps[1].price = 30;
    expectReject(c, RejectReason::STEP_QUANTITY_ZERO, 2);

    // Within one step: zero quantity, then a price over the maximum, then a quantity over the maximum,
    // then a price that rises.
    c = withCurve(validBid(), { { 1, 100 }, { 0, limits::MAX_PRICE_TICKS + 1 } });
    expectReject(c, RejectReason::STEP_QUANTITY_ZERO, 1);
    c.steps[1] = { limits::MAX_ORDER_UNITS + 1, limits::MAX_PRICE_TICKS + 1 };
    expectReject(c, RejectReason::STEP_PRICE_OVER_MAX, 1);
    c.steps[1] = { limits::MAX_ORDER_UNITS + 1, 200 };
    expectReject(c, RejectReason::STEP_QUANTITY_OVER_MAX, 1);
    c.steps[1] = { 1, 200 };
    expectReject(c, RejectReason::STEP_PRICE_ORDER, 1);

    // A problem with a step is reported before the total.
    c = withCurve(validBid(), { { limits::MAX_ORDER_UNITS, 40 }, { limits::MAX_ORDER_UNITS, 30 }, { 0, 20 } });
    expectReject(c, RejectReason::STEP_QUANTITY_ZERO, 2);
    c.steps[2] = { 1, 20 };
    expectReject(c, RejectReason::TOTAL_QUANTITY_OVER_MAX, 1);

    // An ask with a problem at every check: fix them one at a time, and the report moves down the list
    // (side, masks, single value, cap, curve, total).
    c = withCurve(validAsk(), { { limits::MAX_ORDER_UNITS, 10 }, { 0, 20 }, { limits::MAX_ORDER_UNITS, 30 } });
    c.side = static_cast<Side>('Q');
    c.slotMask = 0;
    c.acceleratorMask = 0b11;
    c.perSlotCap = 0;
    expectReject(c, RejectReason::BAD_SIDE, 'Q');
    c.side = Side::ASK;
    expectReject(c, RejectReason::EMPTY_MASK, maskDetail(Attribute::SLOTS, 0));
    c.slotMask = 1;
    expectReject(c, RejectReason::ASK_MASK_NOT_SINGLE, maskDetail(Attribute::ACCELERATOR, 0));
    c.acceleratorMask = 0b10;
    expectReject(c, RejectReason::PER_SLOT_CAP_OUT_OF_RANGE, 0);
    c.perSlotCap = 1;
    expectReject(c, RejectReason::STEP_QUANTITY_ZERO, 1);
    c.steps[1].quantity = 1;
    expectReject(c, RejectReason::TOTAL_QUANTITY_OVER_MAX, 1); // 47,000 + 1 passes the maximum at step 1
    c.steps[0].quantity = 1;
    c.steps[2].quantity = 1;
    EXPECT_FALSE(checkOrder(c));
}

namespace {

// The rules written a second way: list every problem the order has, each with a key that says where
// it falls in the order of the checks, and report the one with the smallest key.
Reject reference(const BatchCommand& c) {
    struct Problem {
        int check;
        int sub;
        Reject reject;
    };
    std::vector<Problem> found;

    const bool bid = c.side == Side::BID;
    const bool ask = c.side == Side::ASK;
    if (!bid && !ask) {
        found.push_back({ 2, 0, { RejectReason::BAD_SIDE, static_cast<std::uint16_t>(c.side) } });
    }

    const std::uint64_t masks[4] = { c.acceleratorMask, c.interconnectMask, c.regionMask, c.slotMask };
    const unsigned options[4] = { 5, 2, 3, 47 };
    for (int a = 0; a < 4; ++a) {
        if (masks[a] == 0) {
            found.push_back({ 3, a, { RejectReason::EMPTY_MASK, static_cast<std::uint16_t>(a << 8) } });
        }
        for (unsigned bit = options[a]; bit < 64; ++bit) {
            if ((masks[a] >> bit) & 1) {
                found.push_back({ 3, a,
                                  { RejectReason::MASK_BIT_OUT_OF_RANGE, static_cast<std::uint16_t>((a << 8) | bit) } });
                break;
            }
        }
    }
    if (ask) {
        for (int a = 0; a < 3; ++a) {
            int bits = 0;
            for (unsigned bit = 0; bit < 64; ++bit) bits += static_cast<int>((masks[a] >> bit) & 1);
            if (bits != 1) {
                found.push_back({ 4, a, { RejectReason::ASK_MASK_NOT_SINGLE, static_cast<std::uint16_t>(a << 8) } });
            }
        }
    }
    if (c.perSlotCap < 1 || c.perSlotCap > 1000) {
        found.push_back({ 5, 0, { RejectReason::PER_SLOT_CAP_OUT_OF_RANGE, 0 } });
    }
    if (c.stepCount < 1 || c.stepCount > 4) {
        found.push_back({ 6, 0, { RejectReason::STEP_COUNT_OUT_OF_RANGE, c.stepCount } });
    } else {
        for (int k = 0; k < c.stepCount; ++k) {
            const CurveStep& s = c.steps[k];
            if (s.quantity == 0) found.push_back({ 6, 1 + 4 * k, { RejectReason::STEP_QUANTITY_ZERO, std::uint16_t(k) } });
            if (s.price > (1u << 20) - 1) {
                found.push_back({ 6, 2 + 4 * k, { RejectReason::STEP_PRICE_OVER_MAX, std::uint16_t(k) } });
            }
            if (s.quantity > 47'000) {
                found.push_back({ 6, 3 + 4 * k, { RejectReason::STEP_QUANTITY_OVER_MAX, std::uint16_t(k) } });
            }
            if (k > 0) {
                const std::uint32_t before = c.steps[k - 1].price;
                if ((bid && s.price > before) || (ask && s.price < before)) {
                    found.push_back({ 6, 4 + 4 * k, { RejectReason::STEP_PRICE_ORDER, std::uint16_t(k) } });
                }
            }
        }
        std::uint64_t total = 0;
        for (int k = 0; k < c.stepCount; ++k) {
            total += c.steps[k].quantity;
            if (total > 47'000) {
                found.push_back({ 7, 0, { RejectReason::TOTAL_QUANTITY_OVER_MAX, std::uint16_t(k) } });
                break;
            }
        }
    }

    if (found.empty()) return {};
    const auto first = std::min_element(found.begin(), found.end(), [](const Problem& x, const Problem& y) {
        return x.check != y.check ? x.check < y.check : x.sub < y.sub;
    });
    return first->reject;
}

// A random order, with every field aimed at its boundary some of the time.
BatchCommand randomOrder(std::mt19937_64& rng) {
    auto below = [&](std::uint64_t n) { return rng() % n; };
    auto oneOf = [&](std::initializer_list<std::uint64_t> values) {
        return *(values.begin() + below(values.size()));
    };

    BatchCommand c{};
    c.type = CommandType::ORDER;
    c.side = below(25) == 0 ? static_cast<Side>(below(256)) : (below(2) ? Side::BID : Side::ASK);

    auto mask = [&](std::size_t options, std::size_t width) -> std::uint64_t {
        const std::uint64_t valid = catalog::validBits(options);
        switch (below(9)) {
        case 0: return 0;
        case 1: return ONE << below(options);                     // one valid value
        case 2: return valid;                                      // all of them
        case 3: return rng() & valid;                              // a random valid set (may be empty)
        case 4: return ONE << (options + below(width - options));  // one value beyond the catalog
        case 5: return (rng() & catalog::validBits(width));        // anything the field holds
        case 6: return (ONE << below(options)) | (ONE << (options + below(width - options)));
        default: return ONE << below(options);                     // mostly a single valid value
        }
    };
    c.acceleratorMask = static_cast<std::uint32_t>(mask(5, 32));
    c.interconnectMask = static_cast<std::uint8_t>(mask(2, 8));
    c.regionMask = static_cast<std::uint32_t>(mask(3, 32));
    c.slotMask = mask(47, 64);

    c.perSlotCap = static_cast<NodeUnits>(oneOf({ 0, 1, 2, 999, 1000, 1001, below(3000), 0xFFFFFFFFu }));
    c.stepCount = static_cast<std::uint8_t>(oneOf({ 0, 1, 2, 3, 4, 4, 4, 5, 255, below(8) }));
    for (std::size_t k = 0; k < limits::MAX_STEPS; ++k) {
        c.steps[k].quantity = static_cast<NodeUnits>(
            oneOf({ 0, 1, 2, below(100), 46'999, 47'000, 47'001, below(60'000), 0xFFFFFFFFu, below(20'000) }));
        c.steps[k].price = static_cast<PriceTicks>(
            oneOf({ 0, 1, below(100), below(2'000), (1u << 20) - 2, (1u << 20) - 1, (1u << 20), 0xFFFFFFFFu }));
    }
    // Often make the prices run the way the side's prices should, so that later checks are reached.
    if (below(3) != 0) {
        for (std::size_t k = 1; k < limits::MAX_STEPS; ++k) {
            if (c.side == Side::BID) c.steps[k].price = std::min(c.steps[k].price, c.steps[k - 1].price);
            if (c.side == Side::ASK) c.steps[k].price = std::max(c.steps[k].price, c.steps[k - 1].price);
        }
    }
    // And keep the total near the maximum, so that the total check is reached from both sides.
    if (below(3) == 0) {
        for (std::size_t k = 0; k < limits::MAX_STEPS; ++k) c.steps[k].quantity = static_cast<NodeUnits>(below(20'000));
    }

    // Half the time, start instead from a good order and spoil up to two fields of it, so that
    // good orders and orders with exactly one problem are common.
    if (below(2) == 0) {
        c = below(2) ? validBid() : validAsk();
        const std::size_t spoils = below(3);
        for (std::size_t s = 0; s < spoils; ++s) {
            switch (below(10)) {
            case 0: c.side = static_cast<Side>(oneOf({ 0, 'B', 'A', 'S', 255 })); break;
            case 1: c.acceleratorMask = static_cast<std::uint32_t>(oneOf({ 0, 1, 0b11, 0b11111, 0b100000, 0x80000000u })); break;
            case 2: c.interconnectMask = static_cast<std::uint8_t>(oneOf({ 0, 1, 0b10, 0b11, 0b100, 0x80 })); break;
            case 3: c.regionMask = static_cast<std::uint32_t>(oneOf({ 0, 1, 0b111, 0b1000, 0x80000000u })); break;
            case 4: c.slotMask = oneOf({ 0, 1, (ONE << 46), (ONE << 47) - 1, ONE << 47, ONE << 63 }); break;
            case 5: c.perSlotCap = static_cast<NodeUnits>(oneOf({ 0, 1, 1000, 1001 })); break;
            case 6: c.stepCount = static_cast<std::uint8_t>(oneOf({ 0, 1, 2, 3, 4, 5 })); break;
            case 7: c.steps[below(4)].quantity = static_cast<NodeUnits>(oneOf({ 0, 1, 47'000, 47'001, 1000 })); break;
            case 8: c.steps[below(4)].price = static_cast<PriceTicks>(oneOf({ 0, 1, 5000, (1u << 20) - 1, 1u << 20 })); break;
            default: c.steps[below(4)] = { static_cast<NodeUnits>(below(30'000)), static_cast<PriceTicks>(below(3000)) }; break;
            }
        }
    }
    return c;
}

// The items of an order, as the engine will expand them: one for each (SKU, hour) in the box.
std::vector<ItemIndex> expandItems(const BatchCommand& c) {
    std::vector<ItemIndex> items;
    for (std::size_t a = 0; a < catalog::ACCELERATOR_COUNT; ++a) {
        if (!((c.acceleratorMask >> a) & 1)) continue;
        for (std::size_t i = 0; i < catalog::INTERCONNECT_COUNT; ++i) {
            if (!((c.interconnectMask >> i) & 1)) continue;
            for (std::size_t r = 0; r < catalog::REGION_COUNT; ++r) {
                if (!((c.regionMask >> r) & 1)) continue;
                const std::size_t sku = catalog::skuIndex(
                    { static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(r) });
                for (std::size_t s = 0; s < limits::SLOT_WINDOW; ++s) {
                    if ((c.slotMask >> s) & 1) items.push_back(static_cast<ItemIndex>(sku * limits::SLOT_WINDOW + s));
                }
            }
        }
    }
    return items;
}

ClearingOrder toClearingOrder(const BatchCommand& c) {
    ClearingOrder o;
    o.id = c.orderToken;
    o.perSlotCap = c.perSlotCap;
    o.stepCount = c.stepCount; // as sent, so a count that is out of range stays out of range
    for (std::size_t k = 0; k < std::min<std::size_t>(c.stepCount, limits::MAX_STEPS); ++k) {
        o.steps[k] = { c.steps[k].quantity, c.steps[k].price };
    }
    o.items = expandItems(c);
    return o;
}

bool clearingRefuses(const BatchCommand& c) {
    ClearingInput in;
    in.itemCount = static_cast<std::uint32_t>(catalog::ITEMS_PER_WINDOW);
    (c.side == Side::BID ? in.bids : in.asks).push_back(toClearingOrder(c));
    try {
        validateInput(in);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

TEST(OrderChecks, RandomOrdersAgreeWithAnIndependentReferenceAndWithTheClearsOwnValidation) {
    std::mt19937_64 rng(20261007);
    std::size_t passed = 0;
    std::size_t byReason[32] = {};
    constexpr std::size_t TRIALS = 400'000;
    for (std::size_t i = 0; i < TRIALS; ++i) {
        const BatchCommand c = randomOrder(rng);
        const Reject got = checkOrder(c);
        const Reject want = reference(c);
        ASSERT_EQ(got.reason, want.reason) << "order " << i << ": got " << toString(got.reason) << ", reference "
                                           << toString(want.reason);
        ASSERT_EQ(got.detail, want.detail) << "order " << i << " " << toString(got.reason);
        ++byReason[static_cast<std::size_t>(got.reason)];
        passed += !got;
    }
    // The generator must reach every check, or the test says little.
    for (const RejectReason r :
         { RejectReason::BAD_SIDE, RejectReason::EMPTY_MASK, RejectReason::MASK_BIT_OUT_OF_RANGE,
           RejectReason::ASK_MASK_NOT_SINGLE, RejectReason::PER_SLOT_CAP_OUT_OF_RANGE,
           RejectReason::STEP_COUNT_OUT_OF_RANGE, RejectReason::STEP_QUANTITY_ZERO, RejectReason::STEP_PRICE_OVER_MAX,
           RejectReason::STEP_QUANTITY_OVER_MAX, RejectReason::STEP_PRICE_ORDER, RejectReason::TOTAL_QUANTITY_OVER_MAX }) {
        EXPECT_GT(byReason[static_cast<std::size_t>(r)], 50u) << toString(r) << " is rarely reached";
    }
    EXPECT_GT(passed, 1000u) << "the generator must also make orders that pass";

    // What the checks pass, the clear accepts (the masks expand to a box of items), and the curve rules are
    // the ones the clear enforces too. Mask and side problems are about the wire form, which the clear
    // never sees.
    std::mt19937_64 rng2(77);
    std::size_t accepted = 0;
    std::size_t curveRefused = 0;
    for (std::size_t i = 0; i < 40'000; ++i) {
        const BatchCommand c = randomOrder(rng2);
        const Reject r = checkOrder(c);
        switch (r.reason) {
        case RejectReason::NONE:
            EXPECT_FALSE(clearingRefuses(c)) << "order " << i;
            ++accepted;
            break;
        case RejectReason::PER_SLOT_CAP_OUT_OF_RANGE:
        case RejectReason::STEP_COUNT_OUT_OF_RANGE:
        case RejectReason::STEP_QUANTITY_ZERO:
        case RejectReason::STEP_PRICE_OVER_MAX:
        case RejectReason::STEP_QUANTITY_OVER_MAX:
        case RejectReason::STEP_PRICE_ORDER:
        case RejectReason::TOTAL_QUANTITY_OVER_MAX:
            EXPECT_TRUE(clearingRefuses(c)) << "order " << i << ": " << toString(r.reason);
            ++curveRefused;
            break;
        default:
            break;
        }
    }
    EXPECT_GT(accepted, 500u);
    EXPECT_GT(curveRefused, 500u);
}
