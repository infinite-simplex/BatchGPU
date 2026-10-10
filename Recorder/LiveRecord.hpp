#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "BatchRecord.hpp"

namespace recorder {

// An order of the open batch, which has not cleared: what it asked for, and nothing about a result.
struct OpenOrderRow {
    RowSide side{ RowSide::BID };
    std::uint32_t rank{ 0 };
    ParticipantId participant{ 0 };
    OrderToken token{ 0 };
    OrderTerms terms;

    friend bool operator==(const OpenOrderRow&, const OpenOrderRow&) = default;
};

// The open batch as of an event: the id it is expected to carry, the first hour it sells, and the last event
// that the open orders reflect.
struct LiveState {
    BatchId batchId{ 0 };
    HourIndex firstHour{ 0 };
    std::uint64_t eventSeq{ 0 };

    friend bool operator==(const LiveState&, const LiveState&) = default;
};

using OrderKey = std::pair<RowSide, std::uint32_t>;

// What changed in the open batch since the last time it was written.
struct LiveChanges {
    LiveState state;
    std::vector<OpenOrderRow> upserts;
    std::vector<OrderKey> removals;
};

} // namespace recorder
