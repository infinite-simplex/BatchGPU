#pragma once
#include <cstdint>
#include <optional>
#include <vector>

#include "BatchBook.hpp"
#include "BookLookup.hpp"

namespace recorder {

inline OrderTerms termsOf(const StoredOrder& stored) {
    OrderTerms terms;
    terms.acceleratorMask = stored.acceleratorMask;
    terms.interconnectMask = stored.interconnectMask;
    terms.regionMask = stored.regionMask;
    terms.slotMask = stored.slotMask;
    terms.perSlotCap = stored.perSlotCap;
    terms.totalUnits = stored.totalUnits;
    terms.stepCount = stored.stepCount;
    for (std::size_t k = 0; k < stored.stepCount && k < terms.stepQuantity.size(); ++k) {
        terms.stepQuantity[k] = stored.steps[k].quantity;
        terms.stepPrice[k] = stored.steps[k].price;
    }
    return terms;
}

// The lookup over an engine's book, which must outlive it. It is called on the engine's thread, from the
// policy, while the engine is in the middle of a command.
inline BookLookup bookLookupOf(const BatchBook& book) {
    return [&book](RowSide side, std::uint32_t rank) -> std::optional<BookedOrder> {
        const std::vector<StoredOrder>& orders = book.orders(side == RowSide::BID ? Side::BID : Side::ASK);
        if (rank >= orders.size()) return std::nullopt;
        const StoredOrder& stored = orders[rank];
        BookedOrder out;
        out.participant = stored.participant;
        out.token = stored.token;
        out.live = stored.live;
        out.terms = termsOf(stored);
        return out;
    };
}

} // namespace recorder
