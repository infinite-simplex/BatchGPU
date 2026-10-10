#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <random>
#include <set>
#include <string>

#include "Catalog.hpp"

using namespace catalog;

// These pin the catalog, which is part of the protocol: if one fails because the catalog was changed on
// purpose, update the expectation and ship the change to every replica together.

namespace {

std::size_t accelerator(const char* name) {
    for (std::size_t i = 0; i < ACCELERATOR_COUNT; ++i) {
        if (std::string(ACCELERATORS[i].name) == name) return i;
    }
    ADD_FAILURE() << "no accelerator called " << name;
    return 0;
}

Sku sku(const char* name, std::uint8_t interconnect, std::uint8_t region) {
    return Sku{ static_cast<std::uint8_t>(accelerator(name)), interconnect, region };
}

constexpr std::uint8_t PCIE = 0;
constexpr std::uint8_t NVLINK = 1;
constexpr std::uint8_t US_WEST = 0;
constexpr std::uint8_t US_EAST = 1;

constexpr std::uint64_t bit(std::size_t index) { return std::uint64_t{ 1 } << index; }

} // namespace

TEST(Catalog, PinsTheToyCatalogAndTheMeaningOfEveryIndexAndMask) {
    ASSERT_EQ(ACCELERATOR_COUNT, 5u);
    EXPECT_STREQ(ACCELERATORS[0].name, "A100-80GB");
    EXPECT_STREQ(ACCELERATORS[1].name, "H100-80GB");
    EXPECT_STREQ(ACCELERATORS[2].name, "H200-141GB");
    EXPECT_STREQ(ACCELERATORS[3].name, "MI300X-192GB");
    EXPECT_STREQ(ACCELERATORS[4].name, "TPU-v5p");
    ASSERT_EQ(INTERCONNECT_COUNT, 2u);
    EXPECT_STREQ(INTERCONNECTS[0], "PCIe");
    EXPECT_STREQ(INTERCONNECTS[1], "NVLink");
    ASSERT_EQ(REGION_COUNT, 3u);
    EXPECT_STREQ(REGIONS[0], "US_WEST");
    EXPECT_STREQ(REGIONS[1], "US_EAST");
    EXPECT_STREQ(REGIONS[2], "EU_WEST");
    EXPECT_EQ(SKU_COUNT, 30u);
    EXPECT_EQ(ITEMS_PER_WINDOW, 1410u) << "30 SKUs in a window of 47 slots";

    const Vendor vendors[] = { Vendor::NVIDIA, Vendor::NVIDIA, Vendor::NVIDIA, Vendor::AMD, Vendor::GOOGLE };
    const int chain[] = { 0, 1, 2, 0, 0 };
    for (std::size_t a = 0; a < ACCELERATOR_COUNT; ++a) {
        EXPECT_EQ(ACCELERATORS[a].vendor, vendors[a]) << ACCELERATORS[a].name;
        EXPECT_EQ(ACCELERATORS[a].chainPosition, chain[a]) << ACCELERATORS[a].name;
    }

    std::set<std::size_t> seen;
    for (std::size_t i = 0; i < SKU_COUNT; ++i) {
        const Sku s = skuFromIndex(i);
        EXPECT_LT(s.accelerator, ACCELERATOR_COUNT);
        EXPECT_LT(s.interconnect, INTERCONNECT_COUNT);
        EXPECT_LT(s.region, REGION_COUNT);
        EXPECT_EQ(skuIndex(s), i);
        seen.insert(skuIndex(s));
    }
    EXPECT_EQ(seen.size(), SKU_COUNT) << "every SKU has its own index";

    EXPECT_EQ(optionCount(Attribute::ACCELERATOR), 5u);
    EXPECT_EQ(optionCount(Attribute::INTERCONNECT), 2u);
    EXPECT_EQ(optionCount(Attribute::REGION), 3u);
    EXPECT_EQ(optionCount(Attribute::SLOTS), 47u);

    EXPECT_EQ(validBits(0), 0u);
    EXPECT_EQ(validBits(1), 1u);
    EXPECT_EQ(validBits(5), 0x1Fu);
    EXPECT_EQ(validBits(47), bit(47) - 1);
    EXPECT_EQ(validBits(64), ~std::uint64_t{ 0 });

    EXPECT_EQ(lowestBitOutside(0, 5), -1);
    EXPECT_EQ(lowestBitOutside(0b11111, 5), -1) << "all five options";
    EXPECT_EQ(lowestBitOutside(0b100000, 5), 5) << "the first bit beyond the catalog";
    EXPECT_EQ(lowestBitOutside(0b1100001, 5), 5) << "the lowest of several";
    EXPECT_EQ(lowestBitOutside(bit(63), 47), 63);
    EXPECT_EQ(lowestBitOutside(bit(46), 47), -1) << "the last slot of the window is valid";
    EXPECT_EQ(lowestBitOutside(bit(47), 47), 47) << "one past the window is not";
}

TEST(Catalog, TheOrderIsAPartialOrderWithTheShapeTheDesignDescribes) {
    for (std::size_t i = 0; i < SKU_COUNT; ++i) {
        const Sku a = skuFromIndex(i);
        EXPECT_TRUE(atLeastAsGood(a, a)) << "reflexive";
        EXPECT_FALSE(strictlyBetter(a, a));
        for (std::size_t j = 0; j < SKU_COUNT; ++j) {
            const Sku b = skuFromIndex(j);
            if (atLeastAsGood(a, b) && atLeastAsGood(b, a)) {
                EXPECT_EQ(a, b) << "antisymmetric";
            }
            for (std::size_t k = 0; k < SKU_COUNT; ++k) {
                const Sku c = skuFromIndex(k);
                if (atLeastAsGood(a, b) && atLeastAsGood(b, c)) {
                    EXPECT_TRUE(atLeastAsGood(a, c)) << "transitive: " << i << " " << j << " " << k;
                }
            }
        }
    }

    const Sku a100 = sku("A100-80GB", PCIE, US_WEST);
    const Sku h100 = sku("H100-80GB", PCIE, US_WEST);
    const Sku h200 = sku("H200-141GB", PCIE, US_WEST);
    EXPECT_TRUE(strictlyBetter(h100, a100));
    EXPECT_TRUE(strictlyBetter(h200, h100));
    EXPECT_TRUE(strictlyBetter(h200, a100));
    EXPECT_FALSE(atLeastAsGood(a100, h100));
    EXPECT_FALSE(atLeastAsGood(h100, h200));

    EXPECT_TRUE(strictlyBetter(sku("H100-80GB", NVLINK, US_WEST), h100));
    EXPECT_FALSE(atLeastAsGood(h100, sku("H100-80GB", NVLINK, US_WEST)));

    const char* models[] = { "A100-80GB", "H100-80GB", "H200-141GB", "MI300X-192GB", "TPU-v5p" };
    for (const char* x : models) {
        for (const char* y : models) {
            const Sku a = sku(x, NVLINK, US_WEST);
            const Sku b = sku(y, NVLINK, US_WEST);
            if (ACCELERATORS[a.accelerator].vendor != ACCELERATORS[b.accelerator].vendor) {
                EXPECT_FALSE(atLeastAsGood(a, b)) << x << " vs " << y;
                EXPECT_FALSE(atLeastAsGood(b, a)) << x << " vs " << y;
            }
        }
    }

    // (H100, PCIe) and (A100, NVLink) are incomparable although each attribute is a chain.
    EXPECT_FALSE(atLeastAsGood(h100, sku("A100-80GB", NVLINK, US_WEST)));
    EXPECT_FALSE(atLeastAsGood(sku("A100-80GB", NVLINK, US_WEST), h100));

    // A region is comparable only with itself, even when the west SKU is better in every other way.
    const Sku west = sku("H200-141GB", NVLINK, US_WEST);
    const Sku east = sku("A100-80GB", PCIE, US_EAST);
    EXPECT_FALSE(atLeastAsGood(west, east));
    EXPECT_FALSE(atLeastAsGood(east, west));
}

TEST(Catalog, ItemsAreNumberedHourByHourAndTheMasksNameTheirBoxOfSkus) {
    std::set<std::uint32_t> seen;
    for (std::size_t slot = 0; slot < limits::SLOT_WINDOW; ++slot) {
        for (std::size_t k = 0; k < SKU_COUNT; ++k) {
            const Sku s = skuFromIndex(k);
            const std::uint32_t item = itemIndex(s, slot);
            EXPECT_LT(item, ITEMS_PER_WINDOW);
            EXPECT_EQ(itemSku(item), s) << "item " << item;
            EXPECT_EQ(itemSlot(item), slot) << "item " << item;
            EXPECT_TRUE(seen.insert(item).second) << "item " << item << " is used twice";
        }
    }
    EXPECT_EQ(seen.size(), ITEMS_PER_WINDOW);
    EXPECT_EQ(*seen.begin(), 0u);
    EXPECT_EQ(*seen.rbegin(), ITEMS_PER_WINDOW - 1);

    for (std::size_t k = 0; k < SKU_COUNT; ++k) {
        EXPECT_EQ(itemIndex(skuFromIndex(k), 0), k) << "the first hour is items 0 to 29";
        EXPECT_EQ(itemIndex(skuFromIndex(k), 3), 3 * SKU_COUNT + k);
        EXPECT_EQ(itemIndex(skuFromIndex(k), 46), 46 * SKU_COUNT + k) << "the last hour";
    }

    EXPECT_EQ(skusNamedBy(0b00010, 0b10, 0b001), bit(skuIndex(sku("H100-80GB", NVLINK, US_WEST))));
    // A100 or H100, NVLink, US_WEST.
    const std::uint64_t example = skusNamedBy(0b00011, 0b10, 0b001);
    EXPECT_EQ(std::popcount(example), 2);
    EXPECT_NE((example >> skuIndex(sku("A100-80GB", NVLINK, US_WEST))) & 1, 0u);
    EXPECT_NE((example >> skuIndex(sku("H100-80GB", NVLINK, US_WEST))) & 1, 0u);
    // Every option of every attribute is every SKU, and an empty mask in any attribute names nothing.
    EXPECT_EQ(skusNamedBy(validBits(ACCELERATOR_COUNT), validBits(INTERCONNECT_COUNT), validBits(REGION_COUNT)),
              validBits(SKU_COUNT));
    EXPECT_EQ(skusNamedBy(0, 0b11, 0b111), 0u);
    EXPECT_EQ(skusNamedBy(0b11111, 0, 0b111), 0u);
    EXPECT_EQ(skusNamedBy(0b11111, 0b11, 0), 0u);

    // Random masks, found by brute force, with bits beyond the catalog that must be ignored.
    std::mt19937_64 rng(2026);
    for (int n = 0; n < 20'000; ++n) {
        const std::uint64_t a = rng();
        const std::uint64_t i = rng();
        const std::uint64_t r = rng();
        std::uint64_t expected = 0;
        for (std::size_t k = 0; k < SKU_COUNT; ++k) {
            const Sku s = skuFromIndex(k);
            if (((a >> s.accelerator) & 1) && ((i >> s.interconnect) & 1) && ((r >> s.region) & 1)) {
                expected |= bit(k);
            }
        }
        ASSERT_EQ(skusNamedBy(a, i, r), expected) << std::hex << a << " " << i << " " << r;
    }
}

TEST(Catalog, AUnitIsLiftedWhenTheOrderAcceptsASkuStrictlyBelowTheOneItGot) {
    for (std::size_t a = 0; a < SKU_COUNT; ++a) {
        EXPECT_EQ(STRICTLY_BELOW[a] >> SKU_COUNT, 0u) << "no bit beyond the catalog";
        for (std::size_t b = 0; b < SKU_COUNT; ++b) {
            const bool inTable = ((STRICTLY_BELOW[a] >> b) & 1) != 0;
            EXPECT_EQ(inTable, strictlyBetter(skuFromIndex(a), skuFromIndex(b))) << a << " vs " << b;
        }
        EXPECT_EQ((STRICTLY_BELOW[a] >> a) & 1, 0u) << "a SKU is not below itself";
    }

    const auto below = [](const Sku& s) { return std::popcount(STRICTLY_BELOW[skuIndex(s)]); };
    EXPECT_EQ(below(sku("H200-141GB", NVLINK, US_WEST)), 5) << "the other five of {A100, H100, H200} x {PCIe, NVLink}";
    EXPECT_EQ(below(sku("H100-80GB", NVLINK, US_WEST)), 3) << "A100 on both links, and H100 on PCIe";
    EXPECT_EQ(below(sku("H100-80GB", PCIE, US_WEST)), 1) << "A100 on PCIe";
    EXPECT_EQ(below(sku("A100-80GB", PCIE, US_WEST)), 0) << "the bottom of the order";
    EXPECT_EQ(below(sku("A100-80GB", NVLINK, US_WEST)), 1) << "A100 on PCIe";
    EXPECT_EQ(below(sku("MI300X-192GB", NVLINK, US_EAST)), 1) << "a vendor with one model has its link chain only";
    EXPECT_EQ(below(sku("TPU-v5p", PCIE, US_EAST)), 0);
    const std::uint64_t belowH200 = STRICTLY_BELOW[skuIndex(sku("H200-141GB", NVLINK, US_WEST))];
    for (std::size_t b = 0; b < SKU_COUNT; ++b) {
        if (((belowH200 >> b) & 1) != 0) {
            EXPECT_EQ(skuFromIndex(b).region, US_WEST) << "the SKUs below are in the same region";
        }
    }

    // A bid that accepts A100 and H100 and gets H100 counts as lifted.
    const std::uint64_t a100OrH100 = skusNamedBy(0b00011, 0b10, 0b001);
    EXPECT_TRUE(isLifted(sku("H100-80GB", NVLINK, US_WEST), a100OrH100));
    EXPECT_FALSE(isLifted(sku("A100-80GB", NVLINK, US_WEST), a100OrH100)) << "it got the lowest it accepts";
    // An order that accepts only the SKU it got was not lifted.
    EXPECT_FALSE(isLifted(sku("H100-80GB", NVLINK, US_WEST), skusNamedBy(0b00010, 0b10, 0b001)));
    // Lifted needs an accepted SKU below, not merely a lower SKU in the catalog.
    EXPECT_FALSE(isLifted(sku("H200-141GB", NVLINK, US_WEST), skusNamedBy(0b00100, 0b10, 0b001)));
    // A different vendor is incomparable: an order for A100 or MI300X that gets MI300X was not lifted.
    EXPECT_FALSE(isLifted(sku("MI300X-192GB", NVLINK, US_WEST), skusNamedBy(0b01001, 0b10, 0b001)));
    // The interconnect lifts too: accepting PCIe and NVLink and getting NVLink.
    EXPECT_TRUE(isLifted(sku("H100-80GB", NVLINK, US_WEST), skusNamedBy(0b00010, 0b11, 0b001)));
    EXPECT_FALSE(isLifted(sku("H100-80GB", PCIE, US_WEST), skusNamedBy(0b00010, 0b11, 0b001)));
    // An order naming both regions accepts A100 in US_EAST as well (its box repeats every SKU in every
    // region it names), so H100 in US_EAST is lifted for it. An order for H100 alone has nothing below.
    EXPECT_TRUE(isLifted(sku("H100-80GB", NVLINK, US_EAST), skusNamedBy(0b00011, 0b10, 0b011)));
    EXPECT_FALSE(isLifted(sku("H100-80GB", NVLINK, US_EAST), skusNamedBy(0b00010, 0b10, 0b011)));
    // A region is not an order: A100 in US_WEST is not below H100 in US_EAST. No box names this set, so
    // it is built from the bits.
    const std::uint64_t mixed = bit(skuIndex(sku("A100-80GB", NVLINK, US_WEST))) |
                                bit(skuIndex(sku("H100-80GB", NVLINK, US_EAST)));
    EXPECT_FALSE(isLifted(sku("H100-80GB", NVLINK, US_EAST), mixed));
    // Two steps down count the same as one.
    EXPECT_TRUE(isLifted(sku("H200-141GB", NVLINK, US_WEST), skusNamedBy(0b00001, 0b10, 0b001)));
}
