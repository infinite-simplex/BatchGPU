#pragma once
#include <cstdint>
#include <limits>
#include <optional>

#include "GpuTypes.hpp"
#include "Limits.hpp"

// The engine has no clock. The clear with id n is nominally at `first + n * period` seconds since
// the epoch, so every replica derives the same eligible hours from the log alone. A batch sells the
// SLOT_WINDOW hours that start at the first hour mark after that time: for a clear at 12:30, 13:00
// through 11:00 two days later.
//
// Ids are not checked, so every 64-bit value must give an answer, with no wrap and no undefined
// behavior. The arithmetic here saturates instead.
namespace schedule {

inline constexpr std::uint64_t SECONDS_PER_HOUR = 3600;

inline constexpr std::uint64_t PERIOD_SECONDS = SECONDS_PER_HOUR;

// The exchange's fixed epoch: the nominal time of the clear with id 0, 2026-10-01 00:30:00 UTC. The
// ticker and every replica must use the same value, because it decides which orders are eligible.
inline constexpr std::uint64_t FIRST_CLEAR_SECONDS = 1'790'814'600;

struct Schedule {
    std::uint64_t firstClearSeconds{ FIRST_CLEAR_SECONDS };
    std::uint64_t periodSeconds{ PERIOD_SECONDS };
};

constexpr std::uint64_t clearSeconds(const Schedule& s, BatchId id) {
    if (s.periodSeconds != 0 && id > (std::numeric_limits<std::uint64_t>::max() - s.firstClearSeconds) / s.periodSeconds) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return s.firstClearSeconds + id * s.periodSeconds;
}

// The id the ticker sends at `nowSeconds`: that of the latest clear due by then, and none before the first.
constexpr std::optional<BatchId> latestClearId(const Schedule& s, std::uint64_t nowSeconds) {
    if (s.periodSeconds == 0 || nowSeconds < s.firstClearSeconds) return std::nullopt;
    return (nowSeconds - s.firstClearSeconds) / s.periodSeconds;
}

// Wider than HourIndex: a very large id gives an hour that does not fit it, and no order can ask for
// such an hour.
constexpr std::uint64_t firstSlotHour(const Schedule& s, BatchId id) {
    return clearSeconds(s, id) / SECONDS_PER_HOUR + 1;
}

constexpr bool isSellable(std::uint64_t firstHour, std::uint64_t hour) {
    return hour >= firstHour && hour - firstHour < limits::SLOT_WINDOW &&
           hour <= std::numeric_limits<HourIndex>::max();
}

// Saturates rather than wrapping to 0, which would send the exchange back to its first hours.
constexpr BatchId nextBatchId(BatchId cleared) {
    return cleared == std::numeric_limits<BatchId>::max() ? cleared : cleared + 1;
}

} // namespace schedule
