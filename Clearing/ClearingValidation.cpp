#include "ClearingValidation.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

[[noreturn]] void reject(const std::string& what) {
    throw std::invalid_argument("clear(): " + what);
}

enum class Side { Bid, Ask };

void validateOrder(const ClearingOrder& order, Side side) {
    const std::string who = (side == Side::Bid ? "bid " : "ask ") + std::to_string(order.id);
    if (order.perSlotCap < 1 || order.perSlotCap > limits::MAX_PER_SLOT_CAP) {
        reject(who + ": perSlotCap out of range");
    }
    if (order.stepCount < 1 || order.stepCount > limits::MAX_STEPS) {
        reject(who + ": step count out of range");
    }
    std::uint64_t total = 0;
    for (std::size_t k = 0; k < order.stepCount; ++k) {
        const Step& step = order.steps[k];
        if (step.quantity < 1) reject(who + ": zero step quantity");
        if (step.price > limits::MAX_PRICE_TICKS) reject(who + ": price over the maximum");
        if (k > 0) {
            const PriceTicks before = order.steps[k - 1].price;
            // Parallel step arcs fill best first, which equals the curve only if it is monotone.
            if (side == Side::Bid && step.price > before) {
                reject(who + ": bid prices rise from step " + std::to_string(k - 1) + " to " +
                       std::to_string(k));
            }
            if (side == Side::Ask && step.price < before) {
                reject(who + ": ask prices fall from step " + std::to_string(k - 1) + " to " +
                       std::to_string(k));
            }
        }
        total += step.quantity;
    }
    if (total > limits::MAX_ORDER_UNITS) reject(who + ": total quantity over the maximum");
    if (order.items.empty()) reject(who + ": no items");
    // An ask names one SKU, so it offers at most one item per slot.
    if (side == Side::Ask && order.items.size() > limits::SLOT_WINDOW) {
        reject(who + ": offers more items than the slot window holds");
    }
}

} // namespace

void validateInput(const ClearingInput& input) {
    validateInput(input, ClearingOptions{});
}

void validateInput(const ClearingInput& input, const ClearingOptions& options) {
    if (input.bids.size() > limits::MAX_ORDERS_PER_SIDE ||
        input.asks.size() > limits::MAX_ORDERS_PER_SIDE) {
        reject("too many orders for one side");
    }
    if (input.itemCount > limits::MAX_ITEMS_PER_BATCH) reject("too many items");

    // Checked first and cheaply, from list lengths, before anything is built.
    std::uint64_t arcs = 0;
    for (const ClearingOrder& order : input.bids) arcs += arcsOf(order);
    for (const ClearingOrder& order : input.asks) arcs += arcsOf(order);
    if (arcs > options.maxArcs) {
        reject("too many arcs: " + std::to_string(arcs) + " (the limit is " + std::to_string(options.maxArcs) + ")");
    }

    // `seen` is cleared only at the items an order touched, so validation costs the number of listed
    // items, not orders times itemCount.
    std::vector<char> seen(input.itemCount, 0);
    auto check = [&](const std::vector<ClearingOrder>& orders, Side side) {
        for (const ClearingOrder& order : orders) {
            validateOrder(order, side);
            std::size_t marked = 0;
            for (ItemIndex item : order.items) {
                if (item >= input.itemCount) {
                    reject("order " + std::to_string(order.id) + ": item index out of range");
                }
                if (seen[item]) {
                    reject("order " + std::to_string(order.id) + ": item listed twice");
                }
                seen[item] = 1;
                ++marked;
            }
            for (std::size_t p = 0; p < marked; ++p) seen[order.items[p]] = 0;
        }
    };
    check(input.bids, Side::Bid);
    check(input.asks, Side::Ask);

    auto checkRanks = [](const std::vector<std::uint32_t>& ranks, std::size_t orders, const char* side) {
        if (ranks.empty()) return;
        if (ranks.size() != orders) reject(std::string(side) + " ranks: one per order or none");
        for (std::size_t i = 0; i < ranks.size(); ++i) {
            if (ranks[i] > limits::MAX_RANK) reject(std::string(side) + " ranks: rank over the maximum");
            if (i > 0 && ranks[i] <= ranks[i - 1]) {
                reject(std::string(side) + " ranks: not strictly increasing at position " + std::to_string(i));
            }
        }
    };
    checkRanks(input.bidRank, input.bids.size(), "bid");
    checkRanks(input.askRank, input.asks.size(), "ask");
}
