#pragma once
#include <cstdint>
#include <functional>
#include <optional>

#include "BatchRecord.hpp"

namespace recorder {

// An order as the engine's book holds it, which the events do not say: what it asked for.
struct BookedOrder {
    ParticipantId participant{ 0 };
    OrderToken token{ 0 };
    bool live{ false };
    OrderTerms terms;
};

// The book keeps every order of the open batch until the batch is over, so the recorder can ask it for the
// order at a rank while the events of the batch come out. Nothing when the book has none there.
using BookLookup = std::function<std::optional<BookedOrder>(RowSide side, std::uint32_t rank)>;

} // namespace recorder
