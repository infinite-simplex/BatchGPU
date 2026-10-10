#pragma once
#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "BatchEvent.hpp"
#include "BatchRecorder.hpp"
#include "BookLookup.hpp"
#include "LiveView.hpp"
#include "ResultsStore.hpp"
#include "Schedule.hpp"
#include "Telemetry.hpp"

namespace recorder {

// The policy of an engine that writes its results to a ResultsStore instead of publishing them. A batch is
// written inline, when its last event arrives. The orders of the open batch are kept in memory as they come
// and written by flushLive(), which the program calls from another thread, so that nothing waits on a
// recorder. A failure to record stops the recording for good and shows in failed(); the engine goes on.
//
// One lock covers the store, the recorder and the live view: the engine's thread holds it for each event, and
// flushLive() for its write. The schedule must be the engine's.
class RecorderPolicy {
public:
    RecorderPolicy(ResultsStore& store, std::ostream& log, const schedule::Schedule& schedule = {})
        : store_(store), log_(log), live_(schedule) {}

    // The engine's book, which holds the terms of each order until its batch is over. Before the first event.
    void attach(BookLookup book) {
        recorder_.setBook(book);
        book_ = std::move(book);
    }

    void on_order_event(const BatchEvent& event) noexcept {
        if (failed_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            switch (event.type) {
            case EventType::ACCEPTED:
                onAccepted(event);
                break;
            case EventType::CANCELED:
                if (liveIsOpen(event.batchId)) live_.canceled(event);
                break;
            case EventType::BATCH_DONE:
            case EventType::BATCH_VOIDED:
                onBatchEnd(event);
                break;
            default:
                recorder_.onEvent(event);
                break;
            }
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) {
            fail("unknown error");
        }
    }

    void on_trace_complete(const OrderTrace&) noexcept {}

    // Writes what changed in the open batch since the last call.
    void flushLive() noexcept {
        if (failed_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            if (live_.dirty()) store_.applyLive(live_.take());
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) {
            fail("unknown error");
        }
    }

    bool failed() const { return failed_.load(std::memory_order_acquire); }
    std::string error() const { return failed() ? error_ : std::string(); }
    std::uint64_t written() const { return written_.load(std::memory_order_relaxed); }
    std::uint64_t duplicates() const { return duplicates_.load(std::memory_order_relaxed); }
    std::uint64_t mismatches() const { return mismatches_.load(std::memory_order_relaxed); }

private:
    static RowSide rowSide(Side side) {
        if (side == Side::BID) return RowSide::BID;
        if (side == Side::ASK) return RowSide::ASK;
        throw std::logic_error("RecorderPolicy: an event names a side that is neither a bid nor an ask");
    }

    // A batch that is recorded already is being replayed, as a restarted recorder does, and its orders are not
    // open any more.
    bool liveIsOpen(BatchId expected) {
        if (!checked_ || checkedId_ != expected) {
            checked_ = true;
            checkedId_ = expected;
            recorded_ = store_.batch(expected).has_value();
        }
        return !recorded_;
    }

    void onAccepted(const BatchEvent& e) {
        if (!liveIsOpen(e.batchId)) return;
        if (!book_) throw std::logic_error("RecorderPolicy: there is no book to read the orders' terms from");
        const RowSide side = rowSide(e.side);
        const std::optional<BookedOrder> booked = book_(side, e.rank);
        if (!booked || !booked->live || booked->participant != e.participantId || booked->token != e.orderToken ||
            booked->terms.totalUnits != e.units) {
            throw std::logic_error("RecorderPolicy: the book does not hold the order that was accepted");
        }
        live_.accepted(e, OpenOrderRow{ side, e.rank, e.participantId, e.orderToken, booked->terms });
    }

    void onBatchEnd(const BatchEvent& e) {
        const std::optional<BatchRecord> record = recorder_.onEvent(e);
        const LiveState next = live_.ended(e);
        checked_ = false;
        if (record) write(*record, next);
    }

    void write(const BatchRecord& record, const LiveState& next) {
        switch (store_.write(record, &next)) {
        case ResultsStore::Outcome::WRITTEN:
            written_.fetch_add(1, std::memory_order_relaxed);
            break;
        case ResultsStore::Outcome::ALREADY_RECORDED:
            duplicates_.fetch_add(1, std::memory_order_relaxed);
            break;
        case ResultsStore::Outcome::ALREADY_RECORDED_DIFFERENTLY:
            duplicates_.fetch_add(1, std::memory_order_relaxed);
            mismatches_.fetch_add(1, std::memory_order_relaxed);
            log_ << "recorder: batch " << record.header.batchId
                 << " was recorded before with different results; the first is kept\n";
            break;
        }
    }

    void fail(const char* what) noexcept {
        error_ = what;
        log_ << "recorder: stopped recording: " << what << "\n";
        failed_.store(true, std::memory_order_release);
    }

    ResultsStore& store_;
    std::ostream& log_;
    std::mutex mutex_;
    BatchRecorder recorder_;
    BookLookup book_;
    LiveView live_;
    bool checked_{ false };
    BatchId checkedId_{ 0 };
    bool recorded_{ false };
    std::atomic<bool> failed_{ false };
    std::string error_;
    std::atomic<std::uint64_t> written_{ 0 };
    std::atomic<std::uint64_t> duplicates_{ 0 };
    std::atomic<std::uint64_t> mismatches_{ 0 };
};

} // namespace recorder
