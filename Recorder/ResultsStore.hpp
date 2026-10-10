#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "BatchRecord.hpp"
#include "LiveRecord.hpp"

namespace recorder {

class StoreError : public std::runtime_error {
public:
    StoreError(const std::string& what, int code) : std::runtime_error(what), code_(code) {}
    int code() const { return code_; }

private:
    int code_;
};

// One SQLite file of results. A connection belongs to one thread, or to one thread at a time. Unsigned 64-bit
// values (batch ids, order tokens) are stored as the same 64 bits in SQLite's signed integer.
class ResultsStore {
public:
    enum class Mode { READ_WRITE, READ_ONLY };

    enum class Outcome { WRITTEN, ALREADY_RECORDED, ALREADY_RECORDED_DIFFERENTLY };

    struct Watermark {
        std::uint64_t batches{ 0 };
        std::uint64_t lastEventSeq{ 0 };
        BatchId lastBatchId{ 0 };
    };

    // READ_WRITE creates the file and its tables when they are not there, and uses WAL with a full sync, so a
    // reader in another process sees whole batches only. Either mode refuses a file of another schema version
    // or another catalog. Throws StoreError.
    explicit ResultsStore(const std::string& path, Mode mode = Mode::READ_WRITE);
    ~ResultsStore();
    ResultsStore(const ResultsStore&) = delete;
    ResultsStore& operator=(const ResultsStore&) = delete;

    // One transaction for the batch and all its rows. The first record with a batch id wins: a second one
    // changes nothing, and the outcome says whether its digest was the same. The digest in the header is
    // ignored, and computed from the rows. The rows must be in canonical order. When `live` is given, the same
    // transaction deletes the open orders and sets the state of the open batch to it, whatever the outcome.
    Outcome write(const BatchRecord& record, const LiveState* live = nullptr);

    // The open batch: one transaction that applies what changed in it, and one that forgets it.
    void applyLive(const LiveChanges& changes);
    void clearLive();

    std::optional<BatchHeader> batch(BatchId id) const;
    std::vector<OrderRow> orders(BatchId id) const;
    std::vector<FillRow> fills(BatchId id) const;
    std::vector<AcceleratorPriceRow> prices(BatchId id) const;
    Watermark watermark() const;

    // Reads in pages, for the results API. A batch's orders and fills come in the order of their keys, from
    // the `offset`th row on.
    std::vector<OrderRow> orders(BatchId id, std::size_t offset, std::size_t limit) const;
    std::vector<FillRow> fills(BatchId id, std::size_t offset, std::size_t limit) const;
    std::optional<OrderRow> order(BatchId id, RowSide side, std::uint32_t rank) const;
    std::vector<FillRow> fillsOf(BatchId id, RowSide side, std::uint32_t rank) const;

    // The batches with the largest ids first, from the id below `before` (or from the largest). Ids are
    // ordered as unsigned numbers, like everywhere else.
    std::vector<BatchHeader> batches(std::optional<BatchId> before, std::size_t limit) const;

    struct Position {
        BatchId batchId{ 0 };
        RowSide side{ RowSide::BID };
        std::uint32_t rank{ 0 };

        friend bool operator==(const Position&, const Position&) = default;
    };
    struct ParticipantOrder {
        Position position;
        HourIndex firstHour{ 0 }; // of the order's batch, which its hours are counted from
        OrderRow row;
    };
    // One participant's orders over all batches, the newest first: by batch id, then side, then rank, each
    // from the largest. `before` excludes that position and everything after it, and `token` keeps the
    // orders that carry it.
    std::vector<ParticipantOrder> ordersOf(ParticipantId participant, std::optional<Position> before,
                                           std::optional<OrderToken> token, std::size_t limit) const;

    // The price of one accelerator model in one delivery hour, batch by batch, the largest batch id first from
    // the id below `before`.
    struct PricePoint {
        BatchId batchId{ 0 };
        AcceleratorPriceRow row;

        friend bool operator==(const PricePoint&, const PricePoint&) = default;
    };
    std::vector<PricePoint> priceSeries(std::uint8_t accelerator, HourIndex hour, std::optional<BatchId> before,
                                        std::size_t limit) const;
    // The same at a fixed lead: in each batch, the hour that is `lead` hours after the first hour it sells.
    std::vector<PricePoint> priceSeriesAtLead(std::uint8_t accelerator, std::uint32_t lead,
                                              std::optional<BatchId> before, std::size_t limit) const;

    struct OpenOrders {
        LiveState state;
        std::vector<OpenOrderRow> orders;
    };
    // The open batch and a participant's orders in it, read together. Nothing until a recorder has written the
    // state of the open batch.
    std::optional<OpenOrders> openOrdersOf(ParticipantId participant) const;
    std::optional<LiveState> live() const;
    std::vector<OpenOrderRow> openOrders() const;

    // Whether the stored rows still hash to the stored digest.
    bool intact(BatchId id) const;

    std::string pragma(const std::string& name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recorder
