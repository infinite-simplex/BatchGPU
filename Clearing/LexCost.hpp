#pragma once
#include <compare>
#include <cstdint>

// The primary part is a price in ticks and the secondary part an arrival rank. Comparison is
// lexicographic, so a secondary difference never outweighs a primary one and no scaling constant is
// needed. Each part only has to fit int64_t on its own.
struct LexCost {
    std::int64_t primary{ 0 };
    std::int64_t secondary{ 0 };

    friend constexpr LexCost operator+(LexCost a, LexCost b) {
        return { a.primary + b.primary, a.secondary + b.secondary };
    }
    friend constexpr LexCost operator-(LexCost a, LexCost b) {
        return { a.primary - b.primary, a.secondary - b.secondary };
    }
    friend constexpr LexCost operator-(LexCost a) { return { -a.primary, -a.secondary }; }

    // Compares in declaration order, so the order of the members matters.
    friend constexpr auto operator<=>(const LexCost&, const LexCost&) = default;
};
