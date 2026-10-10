#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace api {

// The start of an hour counted from the epoch, as "2026-10-01T01:00:00Z" (UTC). A year that does not fit four
// digits gives none, as no real hour does.
inline std::optional<std::string> hourStart(std::uint64_t hour) {
    // Days since 1970-01-01 to a civil date: the proleptic Gregorian calendar, in eras of 400 years.
    const std::int64_t days = static_cast<std::int64_t>(hour / 24) + 719468;
    const std::int64_t era = days / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthFromMarch = (5 * dayOfYear + 2) / 153;
    const unsigned day = dayOfYear - (153 * monthFromMarch + 2) / 5 + 1;
    const unsigned month = monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9;
    const std::int64_t year = static_cast<std::int64_t>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0);
    if (year > 9999) return std::nullopt;

    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02u:00:00Z", static_cast<int>(year), month, day,
                  static_cast<unsigned>(hour % 24));
    return std::string(text);
}

} // namespace api
