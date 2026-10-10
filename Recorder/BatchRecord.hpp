#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>
#include <vector>

#include "GpuTypes.hpp"
#include "Limits.hpp"

namespace recorder {

using Digest = std::array<std::uint8_t, 32>;

enum class BatchStatus : std::uint8_t { CLEARED = 0, VOIDED = 1 };

enum class RowSide : std::uint8_t { BID = 0, ASK = 1 };

struct BatchHeader {
    BatchId batchId{ 0 };
    BatchStatus status{ BatchStatus::CLEARED };
    std::uint8_t voidReason{ 0 };
    HourIndex firstHour{ 0 };
    std::uint64_t eventSeq{ 0 };
    std::uint32_t bids{ 0 };
    std::uint32_t asks{ 0 };
    std::uint32_t arcs{ 0 };
    std::uint32_t pivots{ 0 };
    NodeUnits units{ 0 };
    NodeUnits liftedUnits{ 0 };
    std::int64_t welfare{ 0 };
    std::int64_t rankSum{ 0 };
    std::uint32_t fills{ 0 };
    std::uint8_t certified{ 0 };
    std::uint32_t solverVersion{ 0 };
    Digest digest{};

    friend bool operator==(const BatchHeader&, const BatchHeader&) = default;
};

// What an order asked for, as the engine's book holds it. Steps beyond stepCount are zero.
struct OrderTerms {
    std::uint32_t acceleratorMask{ 0 };
    std::uint8_t interconnectMask{ 0 };
    std::uint32_t regionMask{ 0 };
    std::uint64_t slotMask{ 0 }; // bit s is the hour firstHour + s of the batch
    NodeUnits perSlotCap{ 0 };
    NodeUnits totalUnits{ 0 };
    std::uint8_t stepCount{ 0 };
    std::array<NodeUnits, limits::MAX_STEPS> stepQuantity{};
    std::array<PriceTicks, limits::MAX_STEPS> stepPrice{};

    friend bool operator==(const OrderTerms&, const OrderTerms&) = default;
};

struct OrderRow {
    RowSide side{ RowSide::BID };
    std::uint32_t rank{ 0 };
    ParticipantId participant{ 0 };
    OrderToken token{ 0 };
    OrderTerms terms;
    NodeUnits filled{ 0 };
    NodeUnits lifted{ 0 };
    std::int64_t amount{ 0 };

    NodeUnits unfilled() const { return terms.totalUnits - filled; }

    friend bool operator==(const OrderRow&, const OrderRow&) = default;
};

struct FillRow {
    RowSide side{ RowSide::BID };
    std::uint32_t rank{ 0 };
    HourIndex hour{ 0 };
    std::uint8_t accelerator{ 0 };
    std::uint8_t interconnect{ 0 };
    std::uint8_t region{ 0 };
    PriceTicks price{ 0 };
    NodeUnits units{ 0 };
    std::uint8_t lifted{ 0 };

    friend bool operator==(const FillRow&, const FillRow&) = default;
};

// The clearing price of one accelerator model in one delivery hour: a summary of the items of the model that
// traded (its interconnects and regions), each at its own price. amount / units is the average.
struct AcceleratorPriceRow {
    HourIndex hour{ 0 };
    std::uint8_t accelerator{ 0 };
    NodeUnits units{ 0 };
    std::int64_t amount{ 0 };
    PriceTicks priceMin{ 0 };
    PriceTicks priceMax{ 0 };

    friend bool operator==(const AcceleratorPriceRow&, const AcceleratorPriceRow&) = default;
};

struct BatchRecord {
    BatchHeader header;
    std::vector<OrderRow> orders;
    std::vector<FillRow> fills;
    std::vector<AcceleratorPriceRow> prices;
};

inline auto orderKey(const OrderRow& r) { return std::make_tuple(r.side, r.rank); }
inline auto fillKey(const FillRow& r) {
    return std::make_tuple(r.side, r.rank, r.hour, r.accelerator, r.interconnect, r.region);
}
inline auto priceKey(const AcceleratorPriceRow& r) { return std::make_tuple(r.hour, r.accelerator); }

// The order of the primary keys, which is the order the digest hashes and the store reads back.
inline void sortCanonical(BatchRecord& record) {
    std::sort(record.orders.begin(), record.orders.end(),
              [](const OrderRow& a, const OrderRow& b) { return orderKey(a) < orderKey(b); });
    std::sort(record.fills.begin(), record.fills.end(),
              [](const FillRow& a, const FillRow& b) { return fillKey(a) < fillKey(b); });
    std::sort(record.prices.begin(), record.prices.end(), [](const AcceleratorPriceRow& a, const AcceleratorPriceRow& b) {
        return priceKey(a) < priceKey(b);
    });
}

inline bool isCanonical(const BatchRecord& record) {
    auto strict = [](const auto& rows, auto key) {
        for (std::size_t i = 1; i < rows.size(); ++i) {
            if (!(key(rows[i - 1]) < key(rows[i]))) return false;
        }
        return true;
    };
    return strict(record.orders, [](const OrderRow& r) { return orderKey(r); }) &&
           strict(record.fills, [](const FillRow& r) { return fillKey(r); }) &&
           strict(record.prices, [](const AcceleratorPriceRow& r) { return priceKey(r); });
}

} // namespace recorder
