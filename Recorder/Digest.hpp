#pragma once
#include <cstdint>

#include "BatchRecord.hpp"
#include "Sha256.hpp"

namespace recorder {

namespace digest_detail {

class Feed {
public:
    explicit Feed(Sha256& hash) : hash_(hash) {}

    void u8(std::uint8_t v) { hash_.update(&v, 1); }
    void u32(std::uint32_t v) { little(v, 4); }
    void u64(std::uint64_t v) { little(v, 8); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }

private:
    void little(std::uint64_t v, int bytes) {
        std::uint8_t b[8];
        for (int i = 0; i < bytes; ++i) b[i] = static_cast<std::uint8_t>(v >> (8 * i));
        hash_.update(b, static_cast<std::size_t>(bytes));
    }

    Sha256& hash_;
};

} // namespace digest_detail

// Hashes what a batch's rows say and nothing about how it was recorded: not the event sequence number, not
// the solver version, not the digest itself. The rows must be in canonical order.
inline Digest batchDigest(const BatchRecord& record) {
    Sha256 hash;
    digest_detail::Feed feed(hash);
    static constexpr char TAG[] = "BGPU-BATCH-2";
    hash.update(TAG, sizeof(TAG));

    const BatchHeader& h = record.header;
    feed.u64(h.batchId);
    feed.u8(static_cast<std::uint8_t>(h.status));
    feed.u8(h.voidReason);
    feed.u32(h.firstHour);
    feed.u32(h.bids);
    feed.u32(h.asks);
    feed.u32(h.arcs);
    feed.u32(h.pivots);
    feed.u32(h.units);
    feed.u32(h.liftedUnits);
    feed.i64(h.welfare);
    feed.i64(h.rankSum);
    feed.u32(h.fills);
    feed.u8(h.certified);

    feed.u32(static_cast<std::uint32_t>(record.orders.size()));
    for (const OrderRow& r : record.orders) {
        feed.u8(static_cast<std::uint8_t>(r.side));
        feed.u32(r.rank);
        feed.u32(r.participant);
        feed.u64(r.token);
        const OrderTerms& t = r.terms;
        feed.u32(t.acceleratorMask);
        feed.u8(t.interconnectMask);
        feed.u32(t.regionMask);
        feed.u64(t.slotMask);
        feed.u32(t.perSlotCap);
        feed.u32(t.totalUnits);
        feed.u8(t.stepCount);
        for (std::size_t k = 0; k < t.stepQuantity.size(); ++k) {
            feed.u32(t.stepQuantity[k]);
            feed.u32(t.stepPrice[k]);
        }
        feed.u32(r.filled);
        feed.u32(r.lifted);
        feed.i64(r.amount);
    }
    feed.u32(static_cast<std::uint32_t>(record.fills.size()));
    for (const FillRow& r : record.fills) {
        feed.u8(static_cast<std::uint8_t>(r.side));
        feed.u32(r.rank);
        feed.u32(r.hour);
        feed.u8(r.accelerator);
        feed.u8(r.interconnect);
        feed.u8(r.region);
        feed.u32(r.price);
        feed.u32(r.units);
        feed.u8(r.lifted);
    }
    feed.u32(static_cast<std::uint32_t>(record.prices.size()));
    for (const AcceleratorPriceRow& r : record.prices) {
        feed.u32(r.hour);
        feed.u8(r.accelerator);
        feed.u32(r.units);
        feed.i64(r.amount);
        feed.u32(r.priceMin);
        feed.u32(r.priceMax);
    }
    return hash.finish();
}

} // namespace recorder
