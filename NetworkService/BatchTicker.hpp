#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "NetworkConfig.hpp" // Core
#include "Schedule.hpp"

// The exchange's clock, which the engine does not have. A thread in the gateway's process that connects to
// the gateway's own ingress port and sends BATCH_CLEAR as any client would (as the LOB's GtdCancelService
// sends its cancels), so each clear is sequenced and replicated like every other command. Run one.
//
// It counts ids from the clock: the clear due at first + n * period is sent with id n. When it starts it
// sends the latest id already due, so that a new exchange, whose engine is still on batch 0, takes its time
// from the clock at once instead of refusing every order until the next boundary. After that it sends one
// id per boundary. A clock that jumps over boundaries gets the latest id only, and an id that could not be
// sent is dropped once the next one is due. Start it only with the gateway, since a gateway restart starts a
// new log and the replicas restart with it.
class BatchTicker {
public:
    using Clock = std::function<std::uint64_t()>; // seconds since the epoch

    static std::uint64_t systemSeconds();

    explicit BatchTicker(const NetworkConfig& config, schedule::Schedule schedule = {}, Clock clock = &systemSeconds);
    ~BatchTicker() { stop(); }
    BatchTicker(const BatchTicker&) = delete;
    BatchTicker& operator=(const BatchTicker&) = delete;

    void start();
    void stop();

private:
    void run();
    bool connected();
    void closeConnection();
    void discardAcks();
    bool send(BatchId id);

    std::uint16_t port_;
    schedule::Schedule schedule_;
    Clock clock_;
    int socket_{ -1 };
    std::atomic<bool> running_{ false };
    std::thread thread_;
};
