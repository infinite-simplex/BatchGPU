#include "BatchBook.hpp"

#include <bit>
#include <stdexcept>

namespace {

template <typename F>
void forEachBit(std::uint64_t mask, F&& f) {
    for (; mask != 0; mask &= mask - 1) f(static_cast<unsigned>(std::countr_zero(mask)));
}

// Every hour of the slot mask times every SKU the three masks name, in ascending item index.
void expandItems(const StoredOrder& order, std::vector<ItemIndex>& out) {
    out.reserve(order.arcs - order.stepCount);
    forEachBit(order.slotMask, [&](unsigned slot) {
        forEachBit(order.acceleratorMask, [&](unsigned a) {
            forEachBit(order.interconnectMask, [&](unsigned i) {
                forEachBit(order.regionMask, [&](unsigned r) {
                    const catalog::Sku sku{ static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(i),
                                            static_cast<std::uint8_t>(r) };
                    out.push_back(catalog::itemIndex(sku, slot));
                });
            });
        });
    });
}

} // namespace

BatchBook::BatchBook(const schedule::Schedule& schedule, const BookLimits& bookLimits, BatchId firstBatchId)
    : schedule_(schedule), limits_(bookLimits), batchId_(firstBatchId) {
    if (schedule_.periodSeconds == 0) throw std::invalid_argument("BatchBook: the batch period must not be zero");
    if (limits_.maxOrdersPerSide > limits::MAX_ORDERS_PER_SIDE ||
        limits_.maxSubmissionsPerSide > limits::MAX_SUBMISSIONS_PER_SIDE || limits_.maxArcs > limits::MAX_ARCS_PER_BATCH) {
        throw std::invalid_argument("BatchBook: a batch limit is above the settled one, which the clear would refuse");
    }
    firstHour_ = schedule::firstSlotHour(schedule_, batchId_);
}

Reject BatchBook::checkHours(const BatchCommand& order) const {
    // The stateless checks have already refused a bit beyond the window.
    for (std::uint64_t mask = order.slotMask; mask != 0; mask &= mask - 1) {
        const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
        const std::uint64_t hour = std::uint64_t{ order.baseHour } + bit;
        if (!schedule::isSellable(firstHour_, hour)) {
            return { RejectReason::SLOT_NOT_ELIGIBLE, static_cast<std::uint16_t>(bit) };
        }
    }
    return {};
}

BatchBook::Admission BatchBook::admit(const BatchCommand& cmd) {
    Admission admission;

    // A guard: the handler has made these checks already, but an order that did not come through it
    // must not reach the clear, which would refuse the whole batch for it.
    if (const Reject problem = checkOrder(cmd)) {
        admission.reject = problem;
        return admission;
    }

    // A token in use comes first: it is most likely a client resending an accepted order, and the other
    // answers would then be misleading.
    const std::pair<ParticipantId, OrderToken> key{ cmd.participantId, cmd.orderToken };
    if (tokens_.count(key) != 0) {
        admission.reject = { RejectReason::TOKEN_IN_USE, 0 };
        return admission;
    }
    if (const Reject problem = checkHours(cmd)) {
        admission.reject = problem;
        return admission;
    }

    const std::uint64_t items = std::uint64_t{ static_cast<unsigned>(std::popcount(cmd.acceleratorMask)) } *
                                static_cast<unsigned>(std::popcount(static_cast<std::uint32_t>(cmd.interconnectMask))) *
                                static_cast<unsigned>(std::popcount(cmd.regionMask)) *
                                static_cast<unsigned>(std::popcount(cmd.slotMask));
    const std::uint64_t arcs = items + cmd.stepCount;
    const bool isBid = cmd.side == Side::BID;

    // A buyer's own quota, before the batch's room: an order inside every quota then never meets a full
    // batch unless the batch really is full of other people's orders.
    if (isBid) {
        const BuyerCounts held = buyer(cmd.participantId);
        if (held.openBids >= limits_.maxOpenBidsPerBuyer) {
            admission.reject = { RejectReason::PARTICIPANT_QUOTA, static_cast<std::uint16_t>(QuotaLimit::OPEN_ORDERS) };
            return admission;
        }
        if (held.submissions >= limits_.maxSubmissionsPerBuyer) {
            admission.reject = { RejectReason::PARTICIPANT_QUOTA, static_cast<std::uint16_t>(QuotaLimit::SUBMISSIONS) };
            return admission;
        }
        if (held.arcs + arcs > limits_.maxArcsPerBuyer) {
            admission.reject = { RejectReason::PARTICIPANT_QUOTA, static_cast<std::uint16_t>(QuotaLimit::ARCS) };
            return admission;
        }
    }

    if (openOrders(cmd.side) >= limits_.maxOrdersPerSide) {
        admission.reject = { RejectReason::BATCH_FULL, static_cast<std::uint16_t>(FullLimit::ORDERS) };
        return admission;
    }
    if (submissions(cmd.side) >= limits_.maxSubmissionsPerSide) {
        admission.reject = { RejectReason::BATCH_FULL, static_cast<std::uint16_t>(FullLimit::SUBMISSIONS) };
        return admission;
    }
    if (arcs_ + arcs > limits_.maxArcs) {
        admission.reject = { RejectReason::BATCH_FULL, static_cast<std::uint16_t>(FullLimit::ARCS) };
        return admission;
    }

    // The hours become relative to the batch's first hour; the checks above have shown them to be in
    // the window.
    std::uint64_t relative = 0;
    for (std::uint64_t mask = cmd.slotMask; mask != 0; mask &= mask - 1) {
        const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
        relative |= std::uint64_t{ 1 } << (std::uint64_t{ cmd.baseHour } + bit - firstHour_);
    }

    StoredOrder stored{};
    stored.participant = cmd.participantId;
    stored.token = cmd.orderToken;
    stored.session = cmd.sessionId;
    stored.side = cmd.side;
    stored.live = true;
    stored.interconnectMask = cmd.interconnectMask;
    stored.stepCount = cmd.stepCount;
    stored.acceleratorMask = cmd.acceleratorMask;
    stored.regionMask = cmd.regionMask;
    stored.slotMask = relative;
    stored.skus = catalog::skusNamedBy(cmd.acceleratorMask, cmd.interconnectMask, cmd.regionMask);
    stored.perSlotCap = cmd.perSlotCap;
    stored.arcs = static_cast<std::uint32_t>(arcs);
    std::uint64_t total = 0;
    for (std::size_t k = 0; k < cmd.stepCount; ++k) {
        stored.steps[k] = cmd.steps[k];
        total += cmd.steps[k].quantity;
    }
    stored.totalUnits = static_cast<NodeUnits>(total); // the stateless checks have kept it within the maximum

    std::vector<StoredOrder>& side = isBid ? bids_ : asks_;
    admission.rank = static_cast<std::uint32_t>(side.size());
    side.push_back(stored);
    ++(isBid ? openBids_ : openAsks_);
    arcs_ += arcs;
    tokens_.emplace(key, Ref{ cmd.side, admission.rank });
    if (isBid) {
        BuyerCounts& held = buyers_[cmd.participantId];
        ++held.openBids;
        ++held.submissions;
        held.arcs += arcs;
    }
    return admission;
}

std::optional<BatchBook::Ref> BatchBook::cancel(ParticipantId participant, OrderToken token) {
    const auto found = tokens_.find({ participant, token });
    if (found == tokens_.end()) return std::nullopt;
    const Ref ref = found->second;
    tokens_.erase(found);

    StoredOrder& stored = (ref.side == Side::BID ? bids_ : asks_).at(ref.rank);
    stored.live = false;
    --(ref.side == Side::BID ? openBids_ : openAsks_);
    arcs_ -= stored.arcs;
    if (ref.side == Side::BID) {
        BuyerCounts& held = buyers_.at(participant);
        --held.openBids; // its submission still counts: a submit-and-cancel loop must not be free
        held.arcs -= stored.arcs;
    }
    return ref;
}

BuyerCounts BatchBook::buyer(ParticipantId participant) const {
    const auto found = buyers_.find(participant);
    return found == buyers_.end() ? BuyerCounts{} : found->second;
}

ClearingInput BatchBook::clearingInput() const {
    ClearingInput input;
    input.itemCount = static_cast<std::uint32_t>(catalog::ITEMS_PER_WINDOW);

    auto add = [](const std::vector<StoredOrder>& from, std::vector<ClearingOrder>& orders,
                  std::vector<std::uint32_t>& ranks) {
        for (std::size_t rank = 0; rank < from.size(); ++rank) {
            const StoredOrder& stored = from[rank];
            if (!stored.live) continue;
            ClearingOrder order;
            order.id = rank;
            order.perSlotCap = stored.perSlotCap;
            order.stepCount = stored.stepCount;
            for (std::size_t k = 0; k < stored.stepCount; ++k) {
                order.steps[k] = Step{ stored.steps[k].quantity, stored.steps[k].price };
            }
            expandItems(stored, order.items);
            orders.push_back(std::move(order));
            ranks.push_back(static_cast<std::uint32_t>(rank));
        }
    };
    add(bids_, input.bids, input.bidRank);
    add(asks_, input.asks, input.askRank);
    return input;
}

void BatchBook::startBatchAfter(BatchId cleared) {
    bids_.clear();
    asks_.clear();
    openBids_ = 0;
    openAsks_ = 0;
    arcs_ = 0;
    tokens_.clear();
    buyers_.clear();
    batchId_ = schedule::nextBatchId(cleared);
    firstHour_ = schedule::firstSlotHour(schedule_, batchId_);
}
