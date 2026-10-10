#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "BatchCommand.hpp"
#include "BatchEngine.hpp"
#include "BatchEvent.hpp"
#include "BatchEventFrame.hpp"
#include "BatchProtocolHandler.hpp"
#include "BatchTicker.hpp"
#include "BatchWire.hpp"
#include "EngineFixtures.hpp"
#include "ExchangeFixtures.hpp"
#include "HttpClient.hpp"
#include "MiniJson.hpp"
#include "NetworkConfig.hpp"
#include "ResultsStore.hpp"
#include "Schedule.hpp"
#include "TelemetryReport.hpp" // Core

using namespace enginefix;
using namespace exfix;
using namespace std::chrono_literals;

namespace {

// Stands in for the gateway: takes the ticker's connection and reads the clears it sends, parsed by the real
// handler.
class FakeGateway {
public:
    explicit FakeGateway(std::uint16_t port) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int on = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        EXPECT_EQ(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        EXPECT_EQ(::listen(listener_, 4), 0);
        ::fcntl(listener_, F_SETFL, O_NONBLOCK);
    }
    ~FakeGateway() {
        dropConnection();
        ::close(listener_);
    }
    FakeGateway(const FakeGateway&) = delete;
    FakeGateway& operator=(const FakeGateway&) = delete;

    bool connected() const { return connection_ != -1; }

    void dropConnection() {
        if (connection_ != -1) ::close(connection_);
        connection_ = -1;
        pending_.clear();
    }

    // The ids of the clears that arrive within `wait`, in order.
    std::vector<BatchId> clearsWithin(std::chrono::milliseconds wait) {
        std::vector<BatchId> ids;
        const auto until = std::chrono::steady_clock::now() + wait;
        do {
            if (connection_ == -1) connection_ = ::accept(listener_, nullptr, nullptr);
            if (connection_ != -1) {
                char buffer[256];
                const ssize_t n = ::recv(connection_, buffer, sizeof(buffer), MSG_DONTWAIT);
                if (n > 0) pending_.insert(pending_.end(), buffer, buffer + n);
            }
            for (;;) {
                BatchCommand command{};
                const std::size_t used = handler_.validateAndParse(pending_.data(), pending_.size(), command);
                if (used == 0 || used == BatchProtocolHandler::PARSE_FATAL_ERROR) break;
                EXPECT_EQ(command.type, CommandType::BATCH_CLEAR);
                ids.push_back(command.batchId);
                pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < until);
        return ids;
    }

    // Waits for `count` clears, and then a little longer for any that should not come.
    std::vector<BatchId> clears(std::size_t count) {
        std::vector<BatchId> ids;
        waitUntil(2s, [&] {
            for (const BatchId id : clearsWithin(10ms)) ids.push_back(id);
            return ids.size() >= count;
        });
        for (const BatchId id : clearsWithin(100ms)) ids.push_back(id);
        return ids;
    }

private:
    BatchProtocolHandler handler_;
    int listener_{ -1 };
    int connection_{ -1 };
    std::vector<char> pending_;
};

} // namespace

TEST(Ticker, SendsTheClearThatIsDueAtOnceAndThenOnePerBoundaryWhateverTheClockDoes) {
    const schedule::Schedule schedule{ 1000 * 3600 + 1800, 3600 };
    const std::uint64_t first = schedule.firstClearSeconds;
    std::atomic<std::uint64_t> now{ first - 100 };
    const auto clock = [&] { return now.load(); };

    NetworkConfig config;
    config.ingressListenPort = freeTcpPort();
    FakeGateway gateway(config.ingressListenPort);
    BatchTicker ticker(config, schedule, clock);
    ticker.start();

    EXPECT_TRUE(gateway.clearsWithin(100ms).empty()) << "nothing is due before the first clear";
    EXPECT_FALSE(gateway.connected()) << "and it does not connect for nothing";

    now = first;
    EXPECT_EQ(gateway.clears(1), (std::vector<BatchId>{ 0 }));
    now = first + 3599;
    EXPECT_TRUE(gateway.clearsWithin(100ms).empty()) << "one clear per boundary";
    now = first + 3600;
    EXPECT_EQ(gateway.clears(1), (std::vector<BatchId>{ 1 }));
    now = first + 5 * 3600 + 100;
    EXPECT_EQ(gateway.clears(1), (std::vector<BatchId>{ 5 })) << "a clock that jumps ahead gets the latest id only";

    gateway.dropConnection();
    std::this_thread::sleep_for(100ms);
    now = first + 6 * 3600;
    EXPECT_EQ(gateway.clears(1), (std::vector<BatchId>{ 6 })) << "a connection that was lost is made again";
    ticker.stop();

    SCOPED_TRACE("a ticker that starts in the middle of a period, before the gateway is there");
    now = first + 8 * 3600 + 1800;
    NetworkConfig later;
    later.ingressListenPort = freeTcpPort();
    BatchTicker second(later, schedule, clock);
    second.start();
    std::this_thread::sleep_for(300ms);
    FakeGateway laterGateway(later.ingressListenPort);
    EXPECT_EQ(laterGateway.clears(1), (std::vector<BatchId>{ 8 })) << "it sends the clear already due, once the gateway is up";
    now = first + 9 * 3600;
    EXPECT_EQ(laterGateway.clears(1), (std::vector<BatchId>{ 9 }));
}

namespace {

// The events that arrive on a TCP connection to the gateway, which is also what a client of the exchange is.
class Client {
public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~Client() {
        if (fd_ != -1) ::close(fd_);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connected() const { return fd_ != -1; }

    void send(const std::vector<char>& frame) {
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const ssize_t n = ::send(fd_, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
            ASSERT_GT(n, 0);
            sent += static_cast<std::size_t>(n);
        }
    }

    void pump() {
        char buffer[4096];
        for (;;) {
            const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (n <= 0) break;
            pending_.insert(pending_.end(), buffer, buffer + n);
        }
        while (pending_.size() >= wire::EVENT_FRAME_SIZE) {
            if (const auto event = wire::decodeEvent(pending_.data(), wire::EVENT_FRAME_SIZE)) {
                events.push_back(*event);
            } else {
                ++malformed;
            }
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(wire::EVENT_FRAME_SIZE));
        }
    }

    std::vector<BatchEvent> events;
    std::size_t malformed{ 0 };

private:
    int fd_{ -1 };
    std::vector<char> pending_;
};

// The public feed, which has every event once.
class Feed {
public:
    explicit Feed(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        const int size = 1 << 20;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    }
    ~Feed() { ::close(fd_); }
    Feed(const Feed&) = delete;
    Feed& operator=(const Feed&) = delete;

    void pump() {
        char buffer[512];
        for (;;) {
            const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (n < 0) break;
            if (const auto event = wire::decodeEvent(buffer, static_cast<std::size_t>(n))) {
                events.push_back(*event);
            } else {
                ++malformed;
            }
        }
    }

    std::vector<BatchEvent> events;
    std::size_t malformed{ 0 };

private:
    int fd_{ -1 };
};

// Stands in for the performance service: takes the traces that the replicas forward.
class TelemetrySink {
public:
    explicit TelemetrySink(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    }
    ~TelemetrySink() { ::close(fd_); }
    TelemetrySink(const TelemetrySink&) = delete;
    TelemetrySink& operator=(const TelemetrySink&) = delete;

    void pump() {
        for (;;) {
            TelemetryReport report{};
            const ssize_t n = ::recv(fd_, &report, sizeof(report), MSG_DONTWAIT);
            if (n < 0) break;
            if (n == static_cast<ssize_t>(sizeof(report))) {
                reports.push_back(report);
            } else {
                ++malformed;
            }
        }
    }

    std::vector<TelemetryReport> reports;
    std::size_t malformed{ 0 };

private:
    int fd_{ -1 };
};

// Two sockets in one multicast group on this machine, and a datagram sent to the group that both get. The
// exchange needs it, since its replicas and its recorder all take one log.
bool multicastLoopsBack() {
    const std::uint16_t port = freeUdpPort();
    sockaddr_in group{};
    group.sin_family = AF_INET;
    inet_pton(AF_INET, "239.77.31.9", &group.sin_addr);
    group.sin_port = htons(port);

    auto listener = [&] {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port = htons(port);
        ::bind(fd, reinterpret_cast<sockaddr*>(&any), sizeof(any));
        ip_mreq membership{};
        membership.imr_multiaddr = group.sin_addr;
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership));
        return fd;
    };
    const int a = listener();
    const int b = listener();
    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    const unsigned char ttl = 1;
    ::setsockopt(sender, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    const bool sent = ::sendto(sender, "x", 1, 0, reinterpret_cast<sockaddr*>(&group), sizeof(group)) == 1;
    auto arrives = [](int fd) {
        pollfd p{ fd, POLLIN, 0 };
        return ::poll(&p, 1, 500) == 1;
    };
    const bool works = sent && arrives(a) && arrives(b);
    ::close(a);
    ::close(b);
    ::close(sender);
    return works;
}

// The value after the last `key` in a complete line: a program flushes each part of a line as it writes it.
std::string lastValueOf(const std::string& text, const std::string& key) {
    const std::string lines = text.substr(0, text.rfind('\n') + 1);
    const std::size_t at = lines.rfind(key);
    if (at == std::string::npos) return "";
    const std::size_t from = at + key.size();
    return lines.substr(from, lines.find_first_of(" \n", from) - from);
}

std::size_t occurrences(const std::string& text, const std::string& what) {
    std::size_t count = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++count;
    return count;
}

} // namespace

TEST(Exchange, EveryPartAgreesFromTheClientsBytesToTheDatabase) {
    if (!multicastLoopsBack()) GTEST_SKIP() << "multicast does not loop back on this machine, and the exchange runs on it";

    // The egress has no gap-fill and holds about 240 events of a burst on a default socket buffer, so the largest
    // clear here emits 35.

    // The clients: two buyers, a seller, and the one that sends the clears, as the ticker does. A connection's
    // session is its place in the order they connect.
    enum Who { BUYER_A, BUYER_B, SELLER, CLERK, CONNECTIONS };
    const auto sessionOf = [](int who) { return static_cast<SessionId>(who + 1); };

    struct Step {
        int who;
        std::vector<char> frame;
    };
    std::vector<Step> script;
    const HourIndex first = static_cast<HourIndex>(schedule::firstSlotHour(schedule::Schedule{}, 0));
    const auto order = [&](int who, const Order& o) { script.push_back({ who, wire::encodeOrder(o.command()) }); };
    const auto cancel = [&](int who, ParticipantId p, OrderToken t) { script.push_back({ who, wire::encodeCancel(p, t) }); };
    std::vector<std::size_t> clearSteps;
    const auto clear = [&](BatchId id) {
        clearSteps.push_back(script.size());
        script.push_back({ CLERK, wire::encodeBatchClear(id) });
    };
    const auto at = [](Order o, HourIndex hour) {
        o.baseHour = hour;
        return o;
    };

    // Batch 0: the design's example, a bid that gets nothing, two cancels, a token used twice, and an order
    // the handler refuses.
    order(BUYER_A, at(bid(101, 1, 0b1111111111, 1, { { 2, 2400 }, { 3, 2200 }, { 5, 2000 } }, A100 | H100), first));
    order(SELLER, at(ask(201, 1, 0b1111, 4, { { 8, 1800 }, { 8, 2000 } }, H100), first));
    order(BUYER_B, at(bid(102, 1, 0b11, 3, { { 6, 3000 } }, H200), first));
    order(SELLER, at(ask(202, 1, 0b11, 3, { { 4, 700 }, { 4, 900 } }, H200), first));
    order(BUYER_A, at(bid(103, 1, 0b1, 1, { { 1, 5 } }, A100), first));
    order(BUYER_B, at(bid(104, 9, 0b1, 1, { { 1, 5000 } }, A100), first));
    cancel(BUYER_B, 104, 9);
    cancel(BUYER_A, 101, 77);
    order(BUYER_A, at(bid(101, 1, 0b1, 1, { { 1, 100 } }, A100), first));
    order(BUYER_A, at(bid(105, 1, 0b0, 1, { { 1, 100 } }, A100), first));
    clear(0);

    // Batch 1: a random crowd, with cancels sent from a connection that is not the order's.
    {
        Lcg rng{ 20261009 };
        std::vector<std::pair<ParticipantId, OrderToken>> placed;
        for (OrderToken t = 1; t <= 14; ++t) {
            const ParticipantId who = 1 + rng.below(6);
            order(who % 2 ? BUYER_A : BUYER_B, randomBid(rng, who, t, first + 1));
            placed.emplace_back(who, t);
        }
        for (OrderToken t = 1; t <= 10; ++t) order(SELLER, randomAsk(rng, 11 + rng.below(3), t, first + 1));
        for (int k = 0; k < 3; ++k) {
            const auto [who, token] = placed[rng.below(static_cast<std::uint32_t>(placed.size()))];
            cancel(CLERK, who, token);
        }
        clear(1);
    }

    // Batch 2 is cleared under the id of batch 1 again.
    order(BUYER_A, at(bid(106, 1, 0b1, 1, { { 1, 900 } }, A100), first + 2));
    order(SELLER, at(ask(206, 1, 0b1, 1, { { 1, 400 } }, A100), first + 2));
    clear(1);

    // And one order that waits for the clear after that, which is the open batch when the script ends.
    const Order waiting = at(bid(101, 8, 0b11, 2, { { 2, 700 }, { 1, 650 } }, H200, NVLINK, US_EAST), first + 3);
    order(BUYER_A, waiting);

    // What the exchange must say, from the engine run in process on the same bytes.
    const BatchProtocolHandler handler;
    Harness expected{ EngineConfig{} };
    std::vector<std::size_t> eventsAfter;
    for (const Step& step : script) {
        BatchCommand command{};
        ASSERT_EQ(handler.validateAndParse(step.frame.data(), step.frame.size(), command), step.frame.size());
        expected.send(command, sessionOf(step.who));
        eventsAfter.push_back(expected.events.size());
    }
    const std::vector<recorder::BatchRecord> records = recordsOf(expected);
    {
        SCOPED_TRACE("what the script does");
        const auto count = [&](EventType type) {
            return std::count_if(expected.events.begin(), expected.events.end(), [&](const BatchEvent& e) { return e.type == type; });
        };
        EXPECT_EQ(count(EventType::BATCH_DONE), 3);
        EXPECT_GE(count(EventType::FILL), 20);
        EXPECT_GE(count(EventType::CANCELED), 2);
        EXPECT_GE(count(EventType::REJECTED), 3) << "a token used twice, a cancel of nothing, an empty mask";
        ASSERT_EQ(records.size(), 3u);
        EXPECT_NE(records[1].header.digest, records[2].header.digest) << "the second clear of id 1 is another batch";
    }

    // The exchange: a recorder, two replicas and the gateway, as programs on private ports.
    std::set<std::uint16_t> taken;
    const auto udpPort = [&] {
        for (;;) {
            const std::uint16_t port = freeUdpPort();
            if (taken.insert(port).second) return port;
        }
    };
    const std::uint16_t ingressPort = freeTcpPort();
    const std::uint16_t logPort = udpPort();
    const std::uint16_t resultsPort = udpPort();
    const std::uint16_t retransmitPort = udpPort();
    const std::uint16_t arbitrationPort = udpPort();
    const std::uint16_t feedPort = udpPort();
    const std::uint16_t telemetryPort = udpPort();
    const std::string logGroup = "239.77.31.1";
    const std::string resultsGroup = "239.77.31.2";
    const auto text = [](std::uint16_t port) { return std::to_string(port); };

    Feed feed(feedPort);
    TelemetrySink telemetry(telemetryPort);
    TempFile db, sequenceStore, gatewayLog, replica1Log, replica2Log, recorderLog, apiLog;
    Child gateway, replica1, replica2, recorderProcess, api;
    const auto logs = [&] {
        const std::vector<std::pair<std::string, const TempFile*>> files = {
            { "gateway", &gatewayLog }, { "replica 1", &replica1Log }, { "replica 2", &replica2Log }, { "recorder", &recorderLog },
            { "api", &apiLog }
        };
        std::string all;
        for (const auto& [name, file] : files) {
            const std::string log = slurp(file->path());
            all += "\n--- " + name + " ---\n" + log.substr(log.size() > 1500 ? log.size() - 1500 : 0);
        }
        return all;
    };
    const auto replicaArguments = [&](const char* id, const char* name) {
        return std::vector<std::string>{ REPLICA_APP,        "--replica-id",       id,
                                         "--id",             name,                 "--multicast-ip",
                                         logGroup,           "--multicast-port",   text(logPort),
                                         "--retransmit-port", text(retransmitPort), "--egress-ip",
                                         resultsGroup,       "--egress-port",      text(resultsPort),
                                         "--arbitration-port", text(arbitrationPort), "--performance-service-port",
                                         text(telemetryPort), "--stats-interval-ms", "20" };
    };
    ASSERT_TRUE(recorderProcess.spawn({ RECORDER_APP, "--db", db.path(), "--multicast-ip", logGroup, "--multicast-port",
                                        text(logPort), "--retransmit-port", text(retransmitPort), "--id", "rec",
                                        "--stats-interval-ms", "20" },
                                      recorderLog.path()));
    ASSERT_TRUE(replica1.spawn(replicaArguments("1", "r1"), replica1Log.path()));
    ASSERT_TRUE(replica2.spawn(replicaArguments("2", "r2"), replica2Log.path()));
    ASSERT_TRUE(gateway.spawn({ GATEWAY_APP, "--no-ticker", "--port", text(ingressPort), "--multicast-ip", logGroup,
                                "--multicast-port", text(logPort), "--retransmit-port", text(retransmitPort),
                                "--egress-ip", resultsGroup, "--egress-port", text(resultsPort), "--arbitration-port",
                                text(arbitrationPort), "--broadcast-ip", "127.0.0.1", "--broadcast-port", text(feedPort),
                                "--sequence-store", sequenceStore.path(), "--sequence-capacity", "4096" },
                              gatewayLog.path()));

    const auto said = [](const TempFile& log, const std::string& what) { return slurp(log.path()).find(what) != std::string::npos; };
    ASSERT_TRUE(waitUntil(20s, [&] {
        return said(recorderLog, "ready:") && said(replica1Log, "ready:") && said(replica2Log, "ready:") && said(gatewayLog, "ready:");
    })) << "the programs did not come up" << logs();
    ASSERT_TRUE(waitUntil(10s, [&] { return said(replica1Log, "leader=yes") || said(replica2Log, "leader=yes"); }))
        << "no replica was made leader" << logs();

    std::vector<std::unique_ptr<Client>> clients;
    for (int who = 0; who < CONNECTIONS; ++who) {
        clients.push_back(std::make_unique<Client>(ingressPort));
        ASSERT_TRUE(clients.back()->connected());
    }
    const auto pumpAll = [&] {
        feed.pump();
        telemetry.pump();
        for (auto& client : clients) client->pump();
    };

    // One command at a time, each sent when the events of the one before have come out of the whole loop of
    // gateway, log, replica and results, so that the order the log has is the order of the script.
    for (std::size_t i = 0; i < script.size(); ++i) {
        clients[static_cast<std::size_t>(script[i].who)]->send(script[i].frame);
        ASSERT_TRUE(waitUntil(5s, [&] {
            pumpAll();
            return feed.events.size() >= eventsAfter[i];
        })) << "after command " << i << " the public feed had " << feed.events.size() << " of " << eventsAfter[i] << " events"
            << logs();
    }
    std::this_thread::sleep_for(300ms);
    pumpAll();

    {
        SCOPED_TRACE("the public feed has every event, once, in order");
        std::vector<BatchEvent> want = expected.events;
        for (BatchEvent& e : want) e.session_id = 0;
        EXPECT_TRUE(sameEvents(want, feed.events));
        EXPECT_EQ(feed.malformed, 0u);
    }
    for (int who = 0; who < CONNECTIONS; ++who) {
        SCOPED_TRACE("the events that reach connection " + std::to_string(who) + " are those of its own session");
        std::vector<BatchEvent> want;
        for (BatchEvent e : expected.events) {
            if (e.session_id != sessionOf(who)) continue;
            e.session_id = 0;
            want.push_back(e);
        }
        EXPECT_FALSE(want.empty());
        EXPECT_TRUE(sameEvents(want, clients[static_cast<std::size_t>(who)]->events));
        EXPECT_EQ(clients[static_cast<std::size_t>(who)]->malformed, 0u);
    }

    {
        SCOPED_TRACE("each replica forwards one trace per command, in log order, stamped, with the clears' statistics");
        ASSERT_TRUE(waitUntil(5s, [&] {
            pumpAll();
            return telemetry.reports.size() >= 2 * script.size();
        })) << "the performance service had " << telemetry.reports.size() << " reports of " << 2 * script.size() << logs();
        EXPECT_EQ(telemetry.reports.size(), 2 * script.size());
        EXPECT_EQ(telemetry.malformed, 0u);
        std::map<ReplicaId, std::vector<OrderTrace>> byReplica;
        for (const TelemetryReport& report : telemetry.reports) byReplica[report.replicaId].push_back(report.trace);
        ASSERT_EQ(byReplica.size(), 2u);
        ASSERT_EQ(expected.traces.size(), script.size());
        for (const auto& [replica, traces] : byReplica) {
            SCOPED_TRACE("replica " + std::to_string(replica));
            ASSERT_EQ(traces.size(), script.size());
            for (std::size_t i = 0; i < traces.size(); ++i) {
                const OrderTrace& t = traces[i];
                EXPECT_EQ(t.owner_trace_id, i) << "a command's trace completes in its own turn, so they arrive in the order of the log";
                EXPECT_GT(t.ingress_tai_ns, 0u) << i;
                EXPECT_GE(t.replica_ingress_tai_ns, t.ingress_tai_ns) << i;
                EXPECT_GE(t.engine_pop_tai_ns, t.replica_ingress_tai_ns) << i;
                EXPECT_GE(t.match_done_tai_ns, t.engine_pop_tai_ns) << i;
                EXPECT_EQ(t.ext_counters, expected.traces[i].ext_counters) << i << ": the statistics of a clear, and zeros for every other command";
            }
            for (const std::size_t step : clearSteps) {
                EXPECT_GT(traces[step].match_done_tai_ns, traces[step].engine_pop_tai_ns) << "the clear's time is the difference";
                EXPECT_GT(traces[step].ext_counters[static_cast<std::size_t>(GpuExtSlot::SolverPivots)], 0) << step;
            }
        }
    }

    {
        SCOPED_TRACE("every program applied the whole log, and one replica leads");
        const std::string applied = "applied_seq=" + std::to_string(script.size() - 1);
        for (const TempFile* log : { &recorderLog, &replica1Log, &replica2Log }) {
            EXPECT_TRUE(waitUntil(10s, [&] { return said(*log, applied); })) << "never applied the whole log" << logs();
        }
        const int leaders = (lastValueOf(slurp(replica1Log.path()), "leader=") == "yes") +
                            (lastValueOf(slurp(replica2Log.path()), "leader=") == "yes");
        EXPECT_EQ(leaders, 1);
        EXPECT_EQ(occurrences(slurp(gatewayLog.path()), "[egress] "), expected.events.size())
            << "the gateway heard each event from one replica, once";
    }

    {
        SCOPED_TRACE("the database holds the first record of each batch id");
        ASSERT_TRUE(waitUntil(10s, [&] { return said(recorderLog, "batches=2 duplicates=1 mismatches=1"); })) << logs();
        recorder::ResultsStore reader(db.path(), recorder::ResultsStore::Mode::READ_ONLY);
        expectStoreHolds(reader, records[0]);
        expectStoreHolds(reader, records[1]);
        EXPECT_NE(reader.batch(1)->digest, records[2].header.digest);
        EXPECT_EQ(reader.watermark().batches, 2u);
    }

    {
        SCOPED_TRACE("the results API serves what the database holds");
        ASSERT_TRUE(api.spawn({ API_APP, "--db", db.path(), "--port", "0", "--open-orders" }, apiLog.path()));
        ASSERT_TRUE(waitUntil(20s, [&] { return said(apiLog, "ready:"); })) << logs();
        const std::string startup = slurp(apiLog.path());
        const std::size_t colon = startup.find("127.0.0.1:");
        ASSERT_NE(colon, std::string::npos) << startup;
        const std::uint16_t apiPort = static_cast<std::uint16_t>(std::stoi(startup.substr(colon + 10)));

        const auto ask = [&](const std::string& target) {
            const httpfix::Reply reply = httpfix::get(apiPort, target);
            EXPECT_EQ(reply.status, 200) << target << ": " << reply.body.substr(0, 300);
            const std::optional<minijson::Value> json = minijson::parse(reply.body);
            EXPECT_TRUE(json) << target;
            return json ? *json : minijson::Value{};
        };
        const auto whole = [](auto v) { return static_cast<std::int64_t>(v); };
        const auto sideName = [](recorder::RowSide side) { return side == recorder::RowSide::BID ? "bid" : "ask"; };

        const minijson::Value list = ask("/batches");
        ASSERT_EQ(list.at("batches").size(), 2u);
        EXPECT_EQ(list.at("batches")[0].at("batch").at("batch_id").str(), "1") << "the newest first";
        EXPECT_EQ(list.at("batches")[1].at("batch").at("batch_id").str(), "0");

        for (const recorder::BatchRecord* record : { &records[0], &records[1] }) {
            const recorder::BatchHeader& h = record->header;
            const std::string id = std::to_string(h.batchId);
            SCOPED_TRACE("batch " + id);

            const minijson::Value one = ask("/batches/" + id + "?verify=1");
            EXPECT_EQ(one.at("batch").at("digest").str(), hexOf(h.digest));
            EXPECT_EQ(one.at("batch").at("welfare").integer(), h.welfare);
            EXPECT_EQ(one.at("batch").at("units").integer(), whole(h.units));
            EXPECT_EQ(one.at("batch").at("fills").integer(), whole(h.fills));
            EXPECT_TRUE(one.at("intact").at("recorder-1").isTrue());

            const minijson::Value orders = ask("/batches/" + id + "/orders?limit=1000");
            ASSERT_EQ(orders.at("orders").size(), record->orders.size());
            for (std::size_t i = 0; i < record->orders.size(); ++i) {
                const recorder::OrderRow& want = record->orders[i];
                const minijson::Value& got = orders.at("orders")[i];
                EXPECT_EQ(got.at("side").str(), sideName(want.side));
                EXPECT_EQ(got.at("rank").integer(), whole(want.rank));
                EXPECT_EQ(got.at("participant_id").integer(), whole(want.participant));
                EXPECT_EQ(got.at("order_token").str(), std::to_string(want.token));
                EXPECT_EQ(got.at("filled_units").integer(), whole(want.filled));
                EXPECT_EQ(got.at("unfilled_units").integer(), whole(want.unfilled()));
                EXPECT_EQ(got.at("total_units").integer(), whole(want.terms.totalUnits));
                EXPECT_EQ(got.at("per_slot_cap").integer(), whole(want.terms.perSlotCap));
                EXPECT_EQ(got.at("curve").size(), want.terms.stepCount);
                EXPECT_EQ(got.at("amount").integer(), want.amount);
            }

            const minijson::Value fills = ask("/batches/" + id + "/fills?limit=1000");
            ASSERT_EQ(fills.at("fills").size(), record->fills.size());
            for (std::size_t i = 0; i < record->fills.size(); ++i) {
                const recorder::FillRow& want = record->fills[i];
                const minijson::Value& got = fills.at("fills")[i];
                EXPECT_EQ(got.at("side").str(), sideName(want.side));
                EXPECT_EQ(got.at("rank").integer(), whole(want.rank));
                EXPECT_EQ(got.at("hour").integer(), whole(want.hour));
                EXPECT_EQ(got.at("price").integer(), whole(want.price));
                EXPECT_EQ(got.at("units").integer(), whole(want.units));
                EXPECT_EQ(got.at("lifted").isTrue(), want.lifted != 0);
            }

            const minijson::Value prices = ask("/batches/" + id + "/prices");
            ASSERT_EQ(prices.at("prices").size(), record->prices.size());
            for (std::size_t i = 0; i < record->prices.size(); ++i) {
                const recorder::AcceleratorPriceRow& want = record->prices[i];
                const minijson::Value& got = prices.at("prices")[i];
                EXPECT_EQ(got.at("hour").integer(), whole(want.hour));
                EXPECT_EQ(got.at("accelerator").integer(), whole(want.accelerator));
                EXPECT_EQ(got.at("units").integer(), whole(want.units));
                EXPECT_EQ(got.at("amount").integer(), want.amount);
                EXPECT_EQ(got.at("price_min").integer(), whole(want.priceMin));
                EXPECT_EQ(got.at("price_max").integer(), whole(want.priceMax));
            }
        }

        std::vector<std::pair<BatchId, recorder::OrderRow>> mineWant;
        for (const recorder::BatchRecord* record : { &records[1], &records[0] }) {
            for (auto row = record->orders.rbegin(); row != record->orders.rend(); ++row) {
                if (row->participant == 101) mineWant.emplace_back(record->header.batchId, *row);
            }
        }
        ASSERT_FALSE(mineWant.empty());
        const minijson::Value mine = ask("/participants/101/orders");
        ASSERT_EQ(mine.at("orders").size(), mineWant.size());
        for (std::size_t i = 0; i < mineWant.size(); ++i) {
            EXPECT_EQ(mine.at("orders")[i].at("batch_id").str(), std::to_string(mineWant[i].first));
            EXPECT_EQ(mine.at("orders")[i].at("order_token").str(), std::to_string(mineWant[i].second.token));
            EXPECT_EQ(mine.at("orders")[i].at("filled_units").integer(), whole(mineWant[i].second.filled));
        }

        SCOPED_TRACE("the order that waits for the next clear is the open batch, served from the recorder's flush");
        const std::string open = "/participants/101/open_orders";
        ASSERT_TRUE(waitUntil(10s, [&] {
            const httpfix::Reply reply = httpfix::get(apiPort, open);
            const std::optional<minijson::Value> json = minijson::parse(reply.body);
            return reply.status == 200 && json && json->at("orders").size() == 1;
        })) << "the open order never showed" << logs();
        const minijson::Value waitingNow = ask(open);
        EXPECT_EQ(waitingNow.at("open_batch").at("batch_id").str(), "2") << "the batch after the id that cleared last";
        EXPECT_EQ(waitingNow.at("open_batch").at("first_hour").integer(), whole(first + 2));
        const minijson::Value& row = waitingNow.at("orders")[0];
        EXPECT_EQ(row.at("order_token").str(), "8");
        EXPECT_EQ(row.at("side").str(), "bid");
        EXPECT_EQ(row.at("accelerators")[0].str(), "H200-141GB");
        EXPECT_EQ(row.at("interconnects")[0].str(), "NVLink");
        EXPECT_EQ(row.at("regions")[0].str(), "US_EAST");
        ASSERT_EQ(row.at("hours").size(), 1u);
        EXPECT_EQ(row.at("hours")[0].at("from").integer(), whole(first + 3));
        EXPECT_EQ(row.at("hours")[0].at("to").integer(), whole(first + 4));
        EXPECT_EQ(row.at("total_units").integer(), 3);
        ASSERT_EQ(row.at("curve").size(), 2u);
        EXPECT_EQ(row.at("curve")[1].at("price").integer(), 650);
        EXPECT_EQ(ask("/participants/102/open_orders").at("orders").size(), 0u) << "and the others have none";
        EXPECT_EQ(ask("/batches?limit=1").at("batches")[0].at("batch").at("batch_id").str(), "1") << "it is not a batch yet";
    }

    recorderProcess.terminate();
    replica1.terminate();
    replica2.terminate();
    api.terminate();
    for (Child* child : { &recorderProcess, &replica1, &replica2, &api }) {
        const std::optional<int> status = child->wait(10s);
        ASSERT_TRUE(status) << "a program did not stop" << logs();
        EXPECT_EQ(*status, 0) << logs();
    }
    gateway.terminate();
    const std::optional<int> status = gateway.wait(10s);
    ASSERT_TRUE(status) << "the gateway did not stop" << logs();
    EXPECT_EQ(*status, 0) << logs();
    for (const TempFile* log : { &recorderLog, &replica1Log, &replica2Log }) {
        EXPECT_EQ(slurp(log->path()).find("Sequence gap"), std::string::npos) << logs();
    }
}
