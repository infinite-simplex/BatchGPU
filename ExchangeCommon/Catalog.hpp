#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "Limits.hpp"

// The position of an entry in a table is its bit in an order's mask, so reordering a table
// renumbers the bits that clients send. Append at the end instead. Every replica must run the
// same catalog: it decides which orders are valid and which items exist.
namespace catalog {

// Checks and rejects visit the attributes in this order.
enum class Attribute : std::uint8_t { ACCELERATOR = 0, INTERCONNECT = 1, REGION = 2, SLOTS = 3 };
inline constexpr std::size_t ATTRIBUTE_COUNT = 4;

enum class Vendor : std::uint8_t { NVIDIA, AMD, GOOGLE };

struct Accelerator {
    const char* name;
    Vendor vendor;
    // Position in the vendor's chain, 0 for the lowest. Models of different vendors are incomparable.
    std::uint8_t chainPosition;
};

inline constexpr Accelerator ACCELERATORS[] = {
    { "A100-80GB", Vendor::NVIDIA, 0 },
    { "H100-80GB", Vendor::NVIDIA, 1 },
    { "H200-141GB", Vendor::NVIDIA, 2 },
    { "MI300X-192GB", Vendor::AMD, 0 },
    { "TPU-v5p", Vendor::GOOGLE, 0 },
};

// One chain, in this order.
inline constexpr const char* INTERCONNECTS[] = { "PCIe", "NVLink" };

// Unordered.
inline constexpr const char* REGIONS[] = { "US_WEST", "US_EAST", "EU_WEST" };

inline constexpr std::size_t ACCELERATOR_COUNT = std::size(ACCELERATORS);
inline constexpr std::size_t INTERCONNECT_COUNT = std::size(INTERCONNECTS);
inline constexpr std::size_t REGION_COUNT = std::size(REGIONS);

// A SKU is one (accelerator, interconnect, region). An item is a SKU in one hour slot.
inline constexpr std::size_t SKU_COUNT = ACCELERATOR_COUNT * INTERCONNECT_COUNT * REGION_COUNT;
inline constexpr std::size_t ITEMS_PER_WINDOW = SKU_COUNT * limits::SLOT_WINDOW;

// Width of each mask on the wire. Only the first optionCount() bits mean anything.
inline constexpr std::size_t ACCELERATOR_MASK_BITS = 32;
inline constexpr std::size_t INTERCONNECT_MASK_BITS = 8;
inline constexpr std::size_t REGION_MASK_BITS = 32;
inline constexpr std::size_t SLOT_MASK_BITS = 64;

constexpr std::size_t optionCount(Attribute a) {
    switch (a) {
    case Attribute::ACCELERATOR: return ACCELERATOR_COUNT;
    case Attribute::INTERCONNECT: return INTERCONNECT_COUNT;
    case Attribute::REGION: return REGION_COUNT;
    case Attribute::SLOTS: return limits::SLOT_WINDOW;
    }
    return 0;
}

constexpr std::uint64_t validBits(std::size_t count) {
    return count >= 64 ? ~std::uint64_t{ 0 } : (std::uint64_t{ 1 } << count) - 1;
}

constexpr int lowestBitOutside(std::uint64_t mask, std::size_t count) {
    const std::uint64_t outside = mask & ~validBits(count);
    return outside == 0 ? -1 : std::countr_zero(outside);
}

// A SKU is one (accelerator, interconnect, region). An item is a SKU in one hour slot.
struct Sku {
    std::uint8_t accelerator;
    std::uint8_t interconnect;
    std::uint8_t region;
    friend constexpr bool operator==(Sku, Sku) = default;
};

constexpr std::size_t skuIndex(Sku s) {
    return (static_cast<std::size_t>(s.accelerator) * INTERCONNECT_COUNT + s.interconnect) * REGION_COUNT +
           s.region;
}

constexpr Sku skuFromIndex(std::size_t index) {
    return Sku{ static_cast<std::uint8_t>(index / (INTERCONNECT_COUNT * REGION_COUNT)),
                static_cast<std::uint8_t>((index / REGION_COUNT) % INTERCONNECT_COUNT),
                static_cast<std::uint8_t>(index % REGION_COUNT) };
}

// Same region, an interconnect no lower, and an accelerator at or above in the same vendor's chain.
// Only SKUs of the same hour are comparable, and the caller checks that.
constexpr bool atLeastAsGood(Sku a, Sku b) {
    if (a.region != b.region) return false;
    if (a.interconnect < b.interconnect) return false;
    const Accelerator& x = ACCELERATORS[a.accelerator];
    const Accelerator& y = ACCELERATORS[b.accelerator];
    return x.vendor == y.vendor && x.chainPosition >= y.chainPosition;
}

constexpr bool strictlyBetter(Sku a, Sku b) { return atLeastAsGood(a, b) && !(a == b); }

constexpr std::uint32_t itemIndex(Sku s, std::size_t slot) {
    return static_cast<std::uint32_t>(slot * SKU_COUNT + skuIndex(s));
}
constexpr Sku itemSku(std::uint32_t item) { return skuFromIndex(item % SKU_COUNT); }
constexpr std::size_t itemSlot(std::uint32_t item) { return item / SKU_COUNT; }

// The SKUs named by the product of an order's three masks, as a mask over SKU indices (bit k is
// SKU k). Bits beyond the catalog are ignored.
constexpr std::uint64_t skusNamedBy(std::uint64_t acceleratorMask, std::uint64_t interconnectMask,
                                    std::uint64_t regionMask) {
    std::uint64_t skus = 0;
    for (std::size_t a = 0; a < ACCELERATOR_COUNT; ++a) {
        if (((acceleratorMask >> a) & 1) == 0) continue;
        for (std::size_t i = 0; i < INTERCONNECT_COUNT; ++i) {
            if (((interconnectMask >> i) & 1) == 0) continue;
            for (std::size_t r = 0; r < REGION_COUNT; ++r) {
                if (((regionMask >> r) & 1) == 0) continue;
                const Sku s{ static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(r) };
                skus |= std::uint64_t{ 1 } << skuIndex(s);
            }
        }
    }
    return skus;
}

namespace detail {

constexpr std::array<std::uint64_t, SKU_COUNT> makeStrictlyBelow() {
    std::array<std::uint64_t, SKU_COUNT> below{};
    for (std::size_t a = 0; a < SKU_COUNT; ++a) {
        for (std::size_t b = 0; b < SKU_COUNT; ++b) {
            if (strictlyBetter(skuFromIndex(a), skuFromIndex(b))) below[a] |= std::uint64_t{ 1 } << b;
        }
    }
    return below;
}

} // namespace detail

// Bit j of entry i is set when SKU j is strictly below SKU i.
inline constexpr std::array<std::uint64_t, SKU_COUNT> STRICTLY_BELOW = detail::makeStrictlyBelow();

// A unit awarded at this SKU is lifted when the order accepts some SKU strictly below it.
// Comparing SKUs without looking at the hour is safe because an order repeats its SKUs in every hour.
constexpr bool isLifted(Sku awarded, std::uint64_t acceptedSkus) {
    return (STRICTLY_BELOW[skuIndex(awarded)] & acceptedSkus) != 0;
}

namespace detail {

constexpr bool sameName(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

constexpr bool chainsAreWellFormed() {
    for (std::size_t i = 0; i < ACCELERATOR_COUNT; ++i) {
        std::size_t sameVendor = 0;
        std::size_t samePosition = 0;
        for (std::size_t j = 0; j < ACCELERATOR_COUNT; ++j) {
            if (ACCELERATORS[j].vendor != ACCELERATORS[i].vendor) continue;
            ++sameVendor;
            if (ACCELERATORS[j].chainPosition == ACCELERATORS[i].chainPosition) ++samePosition;
        }
        if (samePosition != 1) return false;
        if (ACCELERATORS[i].chainPosition >= sameVendor) return false;
    }
    return true;
}

constexpr bool namesAreDistinct() {
    for (std::size_t i = 0; i < ACCELERATOR_COUNT; ++i)
        for (std::size_t j = i + 1; j < ACCELERATOR_COUNT; ++j)
            if (sameName(ACCELERATORS[i].name, ACCELERATORS[j].name)) return false;
    for (std::size_t i = 0; i < INTERCONNECT_COUNT; ++i)
        for (std::size_t j = i + 1; j < INTERCONNECT_COUNT; ++j)
            if (sameName(INTERCONNECTS[i], INTERCONNECTS[j])) return false;
    for (std::size_t i = 0; i < REGION_COUNT; ++i)
        for (std::size_t j = i + 1; j < REGION_COUNT; ++j)
            if (sameName(REGIONS[i], REGIONS[j])) return false;
    return true;
}

} // namespace detail

static_assert(ACCELERATOR_COUNT >= 1 && INTERCONNECT_COUNT >= 1 && REGION_COUNT >= 1,
              "every attribute needs at least one option");
static_assert(ACCELERATOR_COUNT <= ACCELERATOR_MASK_BITS, "the accelerator mask is too narrow for the catalog");
static_assert(INTERCONNECT_COUNT <= INTERCONNECT_MASK_BITS, "the interconnect mask is too narrow for the catalog");
static_assert(REGION_COUNT <= REGION_MASK_BITS, "the region mask is too narrow for the catalog");
static_assert(limits::SLOT_WINDOW <= SLOT_MASK_BITS, "the slot mask is too narrow for the window");
static_assert(detail::chainsAreWellFormed(), "each vendor's chain must number its models 0, 1, 2, ... once each");
static_assert(detail::namesAreDistinct(), "names must be distinct within an attribute");
static_assert(SKU_COUNT <= 255, "a SKU index is carried in a byte");
static_assert(SKU_COUNT <= 64, "the set of SKUs that an order names is kept in one 64-bit mask");
static_assert(ITEMS_PER_WINDOW <= UINT32_MAX, "an item index is 32 bits");

} // namespace catalog
