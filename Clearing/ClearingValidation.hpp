#pragma once
#include <cstdint>

#include "ClearingOptions.hpp"
#include "ClearingTypes.hpp"

// One arc per item the order lists and one per curve step. The engine keeps this count as it
// accepts orders so that the clear never sees too many arcs, and validateInput counts the same way.
inline std::uint64_t arcsOf(const ClearingOrder& order) {
    return static_cast<std::uint64_t>(order.items.size()) + order.stepCount;
}

// Throws std::invalid_argument for anything the handler should already have refused. Nothing is
// repaired, because that would hide a handler bug.
void validateInput(const ClearingInput& input, const ClearingOptions& options);
void validateInput(const ClearingInput& input);
