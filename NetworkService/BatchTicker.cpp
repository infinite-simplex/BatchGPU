#include "BatchTicker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <optional>
#include <utility>
#include <vector>

#include "BatchWire.hpp"

namespace {
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(20);
constexpr auto RETRY_INTERVAL = std::chrono::milliseconds(250);
} // namespace

std::uint64_t BatchTicker::systemSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

BatchTicker::BatchTicker(const NetworkConfig& config, schedule::Schedule schedule, Clock clock)
    : port_(config.ingressListenPort), schedule_(schedule), clock_(std::move(clock)) {}

void BatchTicker::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&BatchTicker::run, this);
}

void BatchTicker::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    closeConnection();
}

void BatchTicker::run() {
    std::optional<BatchId> sent;
    auto retryAt = std::chrono::steady_clock::now();
    while (running_.load(std::memory_order_relaxed)) {
        discardAcks();
        const std::optional<BatchId> due = schedule::latestClearId(schedule_, clock_());
        if (due && (!sent || *due > *sent) && std::chrono::steady_clock::now() >= retryAt) {
            if (send(*due)) {
                sent = due;
            } else {
                retryAt = std::chrono::steady_clock::now() + RETRY_INTERVAL;
            }
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
}

bool BatchTicker::connected() {
    if (socket_ != -1) return true;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr); // the gateway's own process
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(fd);
        return false;
    }
    socket_ = fd;
    return true;
}

void BatchTicker::closeConnection() {
    if (socket_ != -1) ::close(socket_);
    socket_ = -1;
}

// The gateway answers every clear on this connection, and nothing here reads the answer.
void BatchTicker::discardAcks() {
    if (socket_ == -1) return;
    char buffer[512];
    for (;;) {
        const ssize_t n = ::recv(socket_, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n > 0) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        closeConnection(); // the gateway closed it, or it failed
        return;
    }
}

bool BatchTicker::send(BatchId id) {
    if (!connected()) return false;
    const std::vector<char> frame = wire::encodeBatchClear(id);
    if (::send(socket_, frame.data(), frame.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(frame.size())) return true;
    closeConnection();
    return false;
}
