#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "BatchEvent.hpp"
#include "BatchRecord.hpp"
#include "BookLookup.hpp"
#include "Digest.hpp"
#include "SolverVersion.hpp"

namespace recorder {

// Collects the events of a batch and hands back its record when the batch ends. The terms of each order come
// from the engine's book, which still holds the batch when its last event is emitted. Anything that disagrees
// with the batch's own summary or with the book throws std::logic_error: a record that is wrong is worse than none.
class BatchRecorder {
public:
    BatchRecorder() = default;
    explicit BatchRecorder(BookLookup book) : book_(std::move(book)) {}

    void setBook(BookLookup book) { book_ = std::move(book); }

    std::optional<BatchRecord> onEvent(const BatchEvent& e) {
        switch (e.type) {
        case EventType::FILL:
            onFill(e);
            return std::nullopt;
        case EventType::ORDER_DONE:
            onOrderDone(e);
            return std::nullopt;
        case EventType::BATCH_DONE:
            return close(e, BatchStatus::CLEARED);
        case EventType::BATCH_VOIDED:
            return close(e, BatchStatus::VOIDED);
        case EventType::ACCEPTED:
        case EventType::REJECTED:
        case EventType::CANCELED:
            return std::nullopt;
        }
        return std::nullopt;
    }

private:
    struct Open {
        bool any{ false };
        RowSide side{ RowSide::BID };
        std::uint32_t rank{ 0 };
        ParticipantId participant{ 0 };
        OrderToken token{ 0 };
        std::uint64_t units{ 0 };
    };

    static RowSide rowSide(Side side) {
        if (side == Side::BID) return RowSide::BID;
        if (side == Side::ASK) return RowSide::ASK;
        throw std::logic_error("BatchRecorder: an event names a side that is neither a bid nor an ask");
    }

    static bool sameOrder(const Open& open, RowSide side, const BatchEvent& e) {
        return open.side == side && open.rank == e.rank && open.participant == e.participantId &&
               open.token == e.orderToken;
    }

    void onFill(const BatchEvent& e) {
        const RowSide side = rowSide(e.side);
        if (open_.any) {
            if (!sameOrder(open_, side, e)) throw std::logic_error("BatchRecorder: the fills of two orders are mixed");
        } else {
            open_ = Open{ true, side, e.rank, e.participantId, e.orderToken, 0 };
        }
        open_.units += e.units;
        (side == RowSide::BID ? bidUnits_ : askUnits_) += e.units;
        fills_.push_back(FillRow{ side, e.rank, e.hour, e.accelerator, e.interconnect, e.region, e.price, e.units,
                                  e.lifted });
    }

    void onOrderDone(const BatchEvent& e) {
        const RowSide side = rowSide(e.side);
        if (open_.any) {
            if (!sameOrder(open_, side, e)) throw std::logic_error("BatchRecorder: an order is done after another order's fills");
            if (open_.units != e.units) throw std::logic_error("BatchRecorder: an order's fills do not add up to its units");
        } else if (e.units != 0) {
            throw std::logic_error("BatchRecorder: an order has units but no fills");
        }
        if (side == RowSide::BID) {
            if (sawAsk_ || (lastBid_ && e.rank <= *lastBid_)) {
                throw std::logic_error("BatchRecorder: bids are not in rank order");
            }
            lastBid_ = e.rank;
            ++bidOrders_;
        } else {
            if (lastAsk_ && e.rank <= *lastAsk_) throw std::logic_error("BatchRecorder: asks are not in rank order");
            sawAsk_ = true;
            lastAsk_ = e.rank;
            ++askOrders_;
        }

        if (!book_) throw std::logic_error("BatchRecorder: there is no book to read the orders' terms from");
        const std::optional<BookedOrder> booked = book_(side, e.rank);
        if (!booked || !booked->live) throw std::logic_error("BatchRecorder: an order is done that the book does not hold as live");
        if (booked->participant != e.participantId || booked->token != e.orderToken) {
            throw std::logic_error("BatchRecorder: the book holds another order at the rank of an order that is done");
        }
        if (std::uint64_t{ booked->terms.totalUnits } != std::uint64_t{ e.units } + e.unfilledUnits) {
            throw std::logic_error("BatchRecorder: an order's quantity is not the book's");
        }
        orders_.push_back(OrderRow{ side, e.rank, e.participantId, e.orderToken, booked->terms, e.units, e.liftedUnits, e.amount });
        open_ = Open{};
    }

    std::optional<BatchRecord> close(const BatchEvent& e, BatchStatus status) {
        if (open_.any) throw std::logic_error("BatchRecorder: the batch ended in the middle of an order");
        BatchRecord record;
        BatchHeader& h = record.header;
        h.batchId = e.batchId;
        h.status = status;
        h.voidReason = static_cast<std::uint8_t>(e.voidReason);
        h.firstHour = e.hour;
        h.eventSeq = e.sequence_number;
        h.bids = e.bids;
        h.asks = e.asks;
        h.arcs = e.arcs;
        h.pivots = e.pivots;
        h.units = e.units;
        h.liftedUnits = e.liftedUnits;
        h.welfare = e.amount;
        h.rankSum = e.rankSum;
        h.fills = e.fills;
        h.certified = e.certified;
        h.solverVersion = SOLVER_VERSION;

        if (status == BatchStatus::VOIDED) {
            if (!orders_.empty() || !fills_.empty()) {
                throw std::logic_error("BatchRecorder: a voided batch came with fills or orders");
            }
        } else {
            if (fills_.size() != e.fills) throw std::logic_error("BatchRecorder: the fills do not match the batch's count");
            if (bidOrders_ != e.bids || askOrders_ != e.asks) {
                throw std::logic_error("BatchRecorder: the orders do not match the batch's counts");
            }
            if (bidUnits_ != e.units || askUnits_ != e.units) {
                throw std::logic_error("BatchRecorder: the units bought and sold do not match the batch's units");
            }
            record.orders = std::move(orders_);
            record.fills = std::move(fills_);
            record.prices = pricesOf(record.fills);
        }
        reset();
        sortCanonical(record);
        h.digest = batchDigest(record);
        return record;
    }

    // An item is one SKU in one hour. It must have been filled at one price, and bought as much as sold, before
    // its units and money are added to its accelerator's row for the hour.
    static std::vector<AcceleratorPriceRow> pricesOf(const std::vector<FillRow>& fills) {
        struct Item {
            PriceTicks price{ 0 };
            std::uint64_t bought{ 0 };
            std::uint64_t sold{ 0 };
        };
        std::map<std::tuple<HourIndex, std::uint8_t, std::uint8_t, std::uint8_t>, Item> items;
        for (const FillRow& f : fills) {
            const auto key = std::make_tuple(f.hour, f.accelerator, f.interconnect, f.region);
            auto [it, fresh] = items.try_emplace(key);
            Item& item = it->second;
            if (fresh) {
                item.price = f.price;
            } else if (item.price != f.price) {
                throw std::logic_error("BatchRecorder: one item was filled at two prices");
            }
            (f.side == RowSide::BID ? item.bought : item.sold) += f.units;
        }

        struct Model {
            AcceleratorPriceRow row;
            std::uint64_t units{ 0 };
        };
        std::map<std::pair<HourIndex, std::uint8_t>, Model> models;
        for (const auto& [key, item] : items) {
            if (item.bought != item.sold) throw std::logic_error("BatchRecorder: an item's units bought and sold differ");
            const HourIndex hour = std::get<0>(key);
            const std::uint8_t accelerator = std::get<1>(key);
            auto [it, fresh] = models.try_emplace(std::make_pair(hour, accelerator));
            Model& model = it->second;
            if (fresh) {
                model.row.hour = hour;
                model.row.accelerator = accelerator;
                model.row.priceMin = model.row.priceMax = item.price;
            }
            model.units += item.bought;
            model.row.amount += static_cast<std::int64_t>(item.bought) * static_cast<std::int64_t>(item.price);
            model.row.priceMin = std::min(model.row.priceMin, item.price);
            model.row.priceMax = std::max(model.row.priceMax, item.price);
        }

        std::vector<AcceleratorPriceRow> out;
        out.reserve(models.size());
        for (auto& [key, model] : models) {
            if (model.units > std::numeric_limits<NodeUnits>::max()) {
                throw std::logic_error("BatchRecorder: an accelerator traded more units in an hour than a row can hold");
            }
            model.row.units = static_cast<NodeUnits>(model.units);
            out.push_back(model.row);
        }
        return out;
    }

    void reset() {
        orders_.clear();
        fills_.clear();
        open_ = Open{};
        bidUnits_ = askUnits_ = 0;
        bidOrders_ = askOrders_ = 0;
        sawAsk_ = false;
        lastBid_.reset();
        lastAsk_.reset();
    }

    BookLookup book_;
    std::vector<OrderRow> orders_;
    std::vector<FillRow> fills_;
    Open open_;
    std::uint64_t bidUnits_{ 0 };
    std::uint64_t askUnits_{ 0 };
    std::uint32_t bidOrders_{ 0 };
    std::uint32_t askOrders_{ 0 };
    bool sawAsk_{ false };
    std::optional<std::uint32_t> lastBid_;
    std::optional<std::uint32_t> lastAsk_;
};

} // namespace recorder
