#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Alias.hpp" // SessionId (Core)
#include "BatchCommand.hpp"
#include "Catalog.hpp"
#include "ClearingTypes.hpp"
#include "GpuTypes.hpp"
#include "Limits.hpp"
#include "OrderChecks.hpp"
#include "RejectReason.hpp"
#include "Schedule.hpp"

// The state of the open batch and the rules that decide which orders enter it. It is a deterministic
// state machine: its only input is the commands, in log order, and it reads no clock, so every replica
// that applies the same commands holds the same batch. It emits nothing; the engine turns what the
// book decides into events.

// The defaults are the limits in Limits.hpp, and every replica must run the same ones. A test may lower
// them, to reach a limit without a batch of a million arcs.
struct BookLimits {
    std::size_t maxOrdersPerSide{ limits::MAX_ORDERS_PER_SIDE };
    std::size_t maxSubmissionsPerSide{ limits::MAX_SUBMISSIONS_PER_SIDE };
    std::uint64_t maxArcs{ limits::MAX_ARCS_PER_BATCH };
    // A buyer's quotas. Sellers have none.
    std::size_t maxOpenBidsPerBuyer{ limits::MAX_OPEN_BIDS_PER_BUYER };
    std::size_t maxSubmissionsPerBuyer{ limits::MAX_SUBMISSIONS_PER_BUYER };
    std::uint64_t maxArcsPerBuyer{ limits::MAX_ARCS_PER_BUYER };
};

// A cancelled order stays in its place, marked not live, because its arrival rank is its position: a
// cancel leaves a gap in the ranks and does not renumber the orders after it.
struct StoredOrder {
    ParticipantId participant;
    OrderToken token;
    SessionId session;
    Side side;
    bool live;
    std::uint8_t interconnectMask;
    std::uint8_t stepCount;
    std::uint32_t acceleratorMask;
    std::uint32_t regionMask;
    // The hours of the order, relative to the first hour of the batch: bit s is hour firstHour + s.
    std::uint64_t slotMask;
    // The SKUs that the three masks name together: bit k is SKU k.
    std::uint64_t skus;
    NodeUnits perSlotCap;
    NodeUnits totalUnits;
    std::uint32_t arcs;
    CurveStep steps[limits::MAX_STEPS];
};

struct BuyerCounts {
    std::uint32_t openBids{ 0 };
    std::uint32_t submissions{ 0 };
    std::uint64_t arcs{ 0 }; // over the buyer's live bids
};

class BatchBook {
public:
    struct Ref {
        Side side;
        std::uint32_t rank;
    };

    struct Admission {
        Reject reject;
        std::uint32_t rank{ 0 }; // when accepted: the arrival rank the order got
    };

    // Throws std::invalid_argument for a zero period, or for a limit above the settled one, which the
    // clear would refuse.
    explicit BatchBook(const schedule::Schedule& schedule = {}, const BookLimits& bookLimits = {},
                       BatchId firstBatchId = 0);

    // Orders are collected for the clear expected to carry this id, and checked against the hours that
    // clear sells.
    BatchId batchId() const noexcept { return batchId_; }
    std::uint64_t firstHour() const noexcept { return firstHour_; }

    // Repeats the stateless checks as a guard, then checks, in this order and naming the first problem:
    // a live order of this participant with the same token, the hours, a buyer's own quota, and then
    // the batch's room (orders on the side, submissions on the side, arcs). An order that passes gets
    // the next arrival rank on its side.
    Admission admit(const BatchCommand& order);

    // The order stays readable through order() until the batch is over, marked not live. Its submission
    // still counts.
    std::optional<Ref> cancel(ParticipantId participant, OrderToken token);

    const StoredOrder& order(Ref ref) const { return orders(ref.side).at(ref.rank); }

    // In arrival order, live or cancelled. The position is the rank.
    const std::vector<StoredOrder>& orders(Side side) const noexcept { return side == Side::BID ? bids_ : asks_; }

    std::size_t openOrders(Side side) const noexcept { return side == Side::BID ? openBids_ : openAsks_; }
    std::size_t submissions(Side side) const noexcept { return orders(side).size(); }
    std::uint64_t arcs() const noexcept { return arcs_; }
    BuyerCounts buyer(ParticipantId participant) const;

    // Every live order, in arrival order, each with the explicit rank it had, and every item an order
    // accepts (not only those that somebody offers, so that the arcs of the graph are exactly arcs()).
    // An order's `id` is its rank.
    ClearingInput clearingInput() const;

    // Every order lapses, every count starts again, and the next batch is the one after `cleared`.
    void startBatchAfter(BatchId cleared);

private:
    Reject checkHours(const BatchCommand& order) const;

    schedule::Schedule schedule_;
    BookLimits limits_;

    BatchId batchId_;
    std::uint64_t firstHour_;

    std::vector<StoredOrder> bids_;
    std::vector<StoredOrder> asks_;
    std::size_t openBids_{ 0 };
    std::size_t openAsks_{ 0 };
    std::uint64_t arcs_{ 0 };

    // Only ever looked up, never walked, so nothing that comes out of the engine depends on its order.
    std::map<std::pair<ParticipantId, OrderToken>, Ref> tokens_;
    std::unordered_map<ParticipantId, BuyerCounts> buyers_;
};
