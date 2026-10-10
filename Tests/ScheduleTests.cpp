#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "Schedule.hpp"

using namespace schedule;

// These pin the schedule's numbers, which decide which hours a batch sells. The engine accepts any batch
// id, so the arithmetic must give an answer for every 64-bit value.

namespace {
constexpr std::uint64_t MAX64 = std::numeric_limits<std::uint64_t>::max();
}

static_assert(firstSlotHour(Schedule{ 1000 * 3600 + 1800, 3600 }, 0) == 1001);
static_assert(firstSlotHour(Schedule{ 1000 * 3600 + 1800, 3600 }, 1) == 1002);
static_assert(isSellable(1001, 1047) && !isSellable(1001, 1048));
static_assert(nextBatchId(std::numeric_limits<BatchId>::max()) == std::numeric_limits<BatchId>::max());

TEST(Schedule, ClearsAtHalfPastEachHourAndSellsFortySevenHoursFromTheNextHourMark) {
    // 2026-10-01 00:30:00 UTC, on the half hour so that the lead is 30 minutes.
    EXPECT_EQ(FIRST_CLEAR_SECONDS, 1'790'814'600u);
    EXPECT_EQ(FIRST_CLEAR_SECONDS % 3600, 1800u);
    EXPECT_EQ(PERIOD_SECONDS, 3600u);
    const Schedule s;
    EXPECT_EQ(s.firstClearSeconds, FIRST_CLEAR_SECONDS);
    EXPECT_EQ(s.periodSeconds, PERIOD_SECONDS);
    // That clear sells from 01:00 the same day, which is hour 497,449 since the epoch.
    EXPECT_EQ(firstSlotHour(s, 0), 497'449u);
    EXPECT_EQ(firstSlotHour(s, 0) * 3600, 1'790'816'400u);

    // The ticker counts the other way, from a time to the latest clear due by it.
    EXPECT_FALSE(latestClearId(s, FIRST_CLEAR_SECONDS - 1));
    for (BatchId id = 0; id < 5000; ++id) {
        EXPECT_EQ(clearSeconds(s, id), FIRST_CLEAR_SECONDS + id * 3600) << "id " << id;
        EXPECT_EQ(firstSlotHour(s, id + 1), firstSlotHour(s, id) + 1) << "id " << id;
        EXPECT_EQ(latestClearId(s, clearSeconds(s, id)), id) << "at the clear itself";
        EXPECT_EQ(latestClearId(s, clearSeconds(s, id + 1) - 1), id) << "until the next one";
    }

    // At 12:30 the window starts at 13:00 and the last slot starts at 11:00 two days later.
    const Schedule halfPast{ 1'791'376'200, 3600 }; // 2026-10-07 12:30:00 UTC, as batch 0
    const std::uint64_t first = firstSlotHour(halfPast, 0);
    EXPECT_EQ(first * 3600, 1'791'378'000u) << "2026-10-07 13:00 UTC";
    EXPECT_EQ((first + limits::SLOT_WINDOW - 1) * 3600, 1'791'543'600u) << "2026-10-09 11:00 UTC";
    // A clear at 13:00 sells from 14:00, and a clear one second before 13:00 still sells from 13:00.
    EXPECT_EQ(firstSlotHour(Schedule{ 1'791'378'000, 3600 }, 0) * 3600, 1'791'381'600u);
    EXPECT_EQ(firstSlotHour(Schedule{ 1'791'378'000 - 1, 3600 }, 0) * 3600, 1'791'378'000u);

    const std::uint64_t window = 1000;
    EXPECT_FALSE(isSellable(window, window - 1));
    EXPECT_TRUE(isSellable(window, window));
    EXPECT_TRUE(isSellable(window, window + 1));
    EXPECT_TRUE(isSellable(window, window + 46));
    EXPECT_FALSE(isSellable(window, window + 47));
    EXPECT_FALSE(isSellable(window, window + 1000));
    EXPECT_FALSE(isSellable(window, 0));
    EXPECT_FALSE(isSellable(window, MAX64));
    for (std::uint64_t hour = 900; hour < 1200; ++hour) {
        EXPECT_EQ(isSellable(window, hour), hour >= 1000 && hour <= 1046) << "hour " << hour;
    }
}

TEST(Schedule, GivesAnAnswerForEvery64BitIdAndNeverSellsAnHourAnEventCannotCarry) {
    const Schedule s;
    // The largest id whose time does not overflow gives the exact time, and the next saturates.
    const BatchId largest = (MAX64 - s.firstClearSeconds) / s.periodSeconds;
    EXPECT_EQ(largest, 5'124'095'575'532'982u);
    EXPECT_EQ(clearSeconds(s, largest), s.firstClearSeconds + largest * s.periodSeconds);
    EXPECT_EQ(clearSeconds(s, largest + 1), MAX64);
    EXPECT_EQ(clearSeconds(s, MAX64), MAX64);
    EXPECT_EQ(clearSeconds(s, MAX64 - 1), MAX64);
    // Time never runs backwards as the id grows, which is what a wrap would do.
    EXPECT_GE(clearSeconds(s, largest + 1), clearSeconds(s, largest));
    EXPECT_GE(clearSeconds(s, MAX64), clearSeconds(s, MAX64 - 1));
    EXPECT_EQ(latestClearId(s, MAX64), largest) << "the latest id at the end of time";

    // No such hour can be asked for: an hour in an order is a 32-bit number plus a bit.
    EXPECT_EQ(firstSlotHour(s, MAX64), MAX64 / 3600 + 1);
    EXPECT_EQ(firstSlotHour(s, MAX64), 5'124'095'576'030'432u);
    EXPECT_GT(firstSlotHour(s, MAX64), std::numeric_limits<HourIndex>::max() + std::uint64_t{ 64 });

    // A zero period must not divide by zero, and a first clear near the top must not overflow.
    const Schedule frozen{ 7200, 0 };
    EXPECT_EQ(clearSeconds(frozen, 0), 7200u);
    EXPECT_EQ(clearSeconds(frozen, MAX64), 7200u);
    EXPECT_FALSE(latestClearId(frozen, MAX64)) << "with no period there is no counting";
    EXPECT_EQ(firstSlotHour(frozen, 12345), 3u);
    const Schedule late{ MAX64 - 10, 3600 };
    EXPECT_EQ(clearSeconds(late, 0), MAX64 - 10);
    EXPECT_EQ(clearSeconds(late, 1), MAX64) << "10 seconds of room, so one period saturates";
    EXPECT_EQ(firstSlotHour(late, 5), MAX64 / 3600 + 1);

    constexpr std::uint64_t top = std::numeric_limits<HourIndex>::max();
    EXPECT_TRUE(isSellable(top - 10, top));
    EXPECT_TRUE(isSellable(top - 10, top - 10));
    EXPECT_FALSE(isSellable(top - 10, top + 1)) << "inside the window, but 33 bits";
    EXPECT_FALSE(isSellable(top - 10, top + 20));
    // A window that starts beyond it sells nothing, and one that would run past 64 bits does not wrap
    // round to small hours.
    EXPECT_FALSE(isSellable(top + 1, top + 1));
    EXPECT_FALSE(isSellable(MAX64, MAX64));
    EXPECT_FALSE(isSellable(MAX64 - 5, 3));
    EXPECT_FALSE(isSellable(MAX64 - 5, MAX64 - 5));

    EXPECT_EQ(nextBatchId(0), 1u);
    EXPECT_EQ(nextBatchId(41), 42u);
    EXPECT_EQ(nextBatchId(MAX64 - 1), MAX64);
    EXPECT_EQ(nextBatchId(MAX64), MAX64);
}
