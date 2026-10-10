#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>

#include "BatchEvent.hpp"
#include "LiveRecord.hpp"
#include "Schedule.hpp"

namespace recorder {

// The open batch as the recorder keeps it between writes: the changes since the last one, and the state they
// leave. Not thread-safe; the policy's lock covers it.
class LiveView {
public:
    explicit LiveView(const schedule::Schedule& schedule) : schedule_(schedule) {}

    void accepted(const BatchEvent& e, const OpenOrderRow& row) {
        pending_[{ row.side, row.rank }] = row;
        touch(e);
    }

    void canceled(const BatchEvent& e) {
        pending_[{ e.side == Side::BID ? RowSide::BID : RowSide::ASK, e.rank }] = std::nullopt;
        touch(e);
    }

    // The batch is over, and its open orders with it. What the batch's own write must leave in the live
    // tables: the batch after the id that cleared, with nothing open.
    LiveState ended(const BatchEvent& e) {
        pending_.clear();
        dirty_ = false;
        state_ = LiveState{ schedule::nextBatchId(e.batchId), firstHourOf(schedule::nextBatchId(e.batchId)), e.sequence_number };
        return state_;
    }

    bool dirty() const { return dirty_; }

    LiveChanges take() {
        LiveChanges changes;
        changes.state = state_;
        for (auto& [key, row] : pending_) {
            if (row) changes.upserts.push_back(*row);
            else changes.removals.push_back(key);
        }
        pending_.clear();
        dirty_ = false;
        return changes;
    }

private:
    HourIndex firstHourOf(BatchId id) const {
        return static_cast<HourIndex>(
            std::min<std::uint64_t>(schedule::firstSlotHour(schedule_, id), std::numeric_limits<HourIndex>::max()));
    }

    void touch(const BatchEvent& e) {
        state_ = LiveState{ e.batchId, firstHourOf(e.batchId), e.sequence_number };
        dirty_ = true;
    }

    schedule::Schedule schedule_;
    std::map<OrderKey, std::optional<OpenOrderRow>> pending_;
    LiveState state_;
    bool dirty_{ false };
};

} // namespace recorder
