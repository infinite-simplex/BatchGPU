#pragma once
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "BatchEvent.hpp"
#include "BatchRecorder.hpp"
#include "Digest.hpp"
#include "EngineFixtures.hpp"
#include "ResultsStore.hpp"
#include "SolverVersion.hpp"

extern char** environ;

// How a failing comparison of rows prints them. Here, because the comparisons are in inline functions below.
namespace recorder {

inline void PrintTo(const BatchHeader& h, std::ostream* os) {
    *os << "batch " << h.batchId << " status " << int(static_cast<std::uint8_t>(h.status)) << " void "
        << int(h.voidReason) << " first_hour " << h.firstHour << " seq " << h.eventSeq << " bids " << h.bids
        << " asks " << h.asks << " arcs " << h.arcs << " pivots " << h.pivots << " units " << h.units << " lifted "
        << h.liftedUnits << " welfare " << h.welfare << " rank_sum " << h.rankSum << " fills " << h.fills
        << " certified " << int(h.certified) << " version " << h.solverVersion << " digest " << int(h.digest[0])
        << "..";
}
inline void PrintTo(const OrderTerms& t, std::ostream* os) {
    *os << "terms accel " << t.acceleratorMask << " inter " << int(t.interconnectMask) << " region " << t.regionMask
        << " slots " << t.slotMask << " cap " << t.perSlotCap << " total " << t.totalUnits << " curve";
    for (std::size_t k = 0; k < t.stepQuantity.size(); ++k) *os << " " << t.stepQuantity[k] << "@" << t.stepPrice[k];
    *os << " (" << int(t.stepCount) << ")";
}
inline void PrintTo(const OrderRow& r, std::ostream* os) {
    *os << "order side " << int(static_cast<std::uint8_t>(r.side)) << " rank " << r.rank << " who " << r.participant
        << "/" << r.token << " ";
    PrintTo(r.terms, os);
    *os << " filled " << r.filled << " lifted " << r.lifted << " amount " << r.amount;
}
inline void PrintTo(const OpenOrderRow& r, std::ostream* os) {
    *os << "open order side " << int(static_cast<std::uint8_t>(r.side)) << " rank " << r.rank << " who " << r.participant
        << "/" << r.token << " ";
    PrintTo(r.terms, os);
}
inline void PrintTo(const LiveState& s, std::ostream* os) {
    *os << "live batch " << s.batchId << " first_hour " << s.firstHour << " seq " << s.eventSeq;
}
inline void PrintTo(const FillRow& r, std::ostream* os) {
    *os << "fill side " << int(static_cast<std::uint8_t>(r.side)) << " rank " << r.rank << " item " << int(r.accelerator)
        << "/" << int(r.interconnect) << "/" << int(r.region) << "@" << r.hour << " price " << r.price << " units "
        << r.units << " lifted " << int(r.lifted);
}
inline void PrintTo(const AcceleratorPriceRow& r, std::ostream* os) {
    *os << "price accelerator " << int(r.accelerator) << "@" << r.hour << " units " << r.units << " amount " << r.amount
        << " from " << r.priceMin << " to " << r.priceMax;
}
inline void PrintTo(const ResultsStore::Position& p, std::ostream* os) {
    *os << "batch " << p.batchId << (p.side == RowSide::BID ? " bid " : " ask ") << p.rank;
}

} // namespace recorder

// What the recorder's tests and the whole-exchange tests share: files, ports and child processes, the batches
// of random orders, and the records that a run of events must make.
namespace exfix {

class TempFile {
public:
    TempFile() {
        static std::atomic<int> counter{ 0 };
        path_ = (std::filesystem::temp_directory_path() /
                 ("batchgpu-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++)))
                    .string();
    }
    ~TempFile() {
        std::error_code ignored;
        for (const char* suffix : { "", "-wal", "-shm" }) std::filesystem::remove(path_ + suffix, ignored);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

inline std::string slurp(const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline std::string hexOf(const recorder::Digest& digest) {
    std::string out;
    char two[3];
    for (const std::uint8_t byte : digest) {
        std::snprintf(two, sizeof(two), "%02x", byte);
        out += two;
    }
    return out;
}

template <typename Predicate>
bool waitUntil(std::chrono::milliseconds limit, Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// A port that nothing holds at the moment, for a socket of the given type.
inline std::uint16_t freePort(int socketType) {
    const int fd = ::socket(AF_INET, socketType, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    socklen_t length = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    const std::uint16_t port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}
inline std::uint16_t freeUdpPort() { return freePort(SOCK_DGRAM); }
inline std::uint16_t freeTcpPort() { return freePort(SOCK_STREAM); }

// A program of the exchange, with its output in a file. The destructor kills it.
class Child {
public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
    }

    bool spawn(const std::vector<std::string>& args, const std::string& logPath) {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const int rc = ::posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (rc != 0) pid_ = -1;
        return rc == 0;
    }

    void terminate() {
        if (pid_ > 0) ::kill(pid_, SIGTERM);
    }

    // The exit status once it has exited, and none while it runs.
    std::optional<int> poll() {
        if (pid_ > 0) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                status_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
        }
        return status_;
    }

    std::optional<int> wait(std::chrono::seconds limit) {
        waitUntil(limit, [&] { return poll().has_value(); });
        return status_;
    }

private:
    pid_t pid_{ -1 };
    std::optional<int> status_;
};

struct Lcg {
    std::uint64_t state;
    std::uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
    std::uint32_t between(std::uint32_t lo, std::uint32_t hi) { return lo + below(hi - lo + 1); }
};

inline std::uint64_t hoursRun(std::uint32_t start, std::uint32_t length) {
    return ((std::uint64_t{ 1 } << length) - 1) << start;
}

// A bid for a random box of the catalog over a short run of hours, and an ask for one SKU.
inline enginefix::Order randomBid(Lcg& rng, ParticipantId participant, OrderToken token, HourIndex firstHour) {
    enginefix::Order o;
    o.side = Side::BID;
    o.participant = participant;
    o.token = token;
    o.baseHour = firstHour;
    o.slots = hoursRun(rng.below(6), rng.between(1, 4));
    o.accel = rng.between(1, 0b11111);
    o.inter = static_cast<std::uint8_t>(rng.between(1, 0b11));
    o.region = rng.between(1, 0b111);
    o.cap = rng.between(1, 3);
    o.steps.clear();
    PriceTicks price = rng.between(800, 2600);
    for (std::uint32_t k = rng.between(1, 3); k > 0; --k) {
        o.steps.push_back({ rng.between(1, 4), price });
        price = price > 300 ? price - rng.between(50, 300) : price;
    }
    return o;
}

inline enginefix::Order randomAsk(Lcg& rng, ParticipantId participant, OrderToken token, HourIndex firstHour) {
    enginefix::Order o;
    o.side = Side::ASK;
    o.participant = participant;
    o.token = token;
    o.baseHour = firstHour;
    o.slots = hoursRun(rng.below(6), rng.between(1, 4));
    o.accel = 1u << rng.below(5);
    o.inter = static_cast<std::uint8_t>(1u << rng.below(2));
    o.region = 1u << rng.below(3);
    o.cap = rng.between(1, 4);
    o.steps.clear();
    PriceTicks price = rng.between(200, 1800);
    for (std::uint32_t k = rng.between(1, 3); k > 0; --k) {
        o.steps.push_back({ rng.between(1, 5), price });
        price += rng.between(20, 250);
    }
    return o;
}

// The terms of an order, from what a test says of it. The hours are counted from the first hour of the batch.
inline recorder::OrderTerms makeTerms(std::uint32_t accelerators, std::uint8_t interconnects, std::uint32_t regions,
                                      std::uint64_t slots, NodeUnits cap,
                                      std::initializer_list<std::pair<NodeUnits, PriceTicks>> curve) {
    recorder::OrderTerms t;
    t.acceleratorMask = accelerators;
    t.interconnectMask = interconnects;
    t.regionMask = regions;
    t.slotMask = slots;
    t.perSlotCap = cap;
    for (const auto& [quantity, price] : curve) {
        t.stepQuantity[t.stepCount] = quantity;
        t.stepPrice[t.stepCount] = price;
        t.totalUnits += quantity;
        ++t.stepCount;
    }
    return t;
}

// What an order asked for, worked out from the command that carried it and the first hour of the batch it was
// accepted into.
inline recorder::OrderTerms termsOfCommand(const BatchCommand& c, HourIndex firstHour) {
    recorder::OrderTerms t;
    t.acceleratorMask = c.acceleratorMask;
    t.interconnectMask = c.interconnectMask;
    t.regionMask = c.regionMask;
    for (unsigned bit = 0; bit < 64; ++bit) {
        if (((c.slotMask >> bit) & 1) == 0) continue;
        t.slotMask |= std::uint64_t{ 1 } << (std::uint64_t{ c.baseHour } + bit - firstHour);
    }
    t.perSlotCap = c.perSlotCap;
    t.stepCount = c.stepCount;
    for (unsigned k = 0; k < c.stepCount; ++k) {
        t.stepQuantity[k] = c.steps[k].quantity;
        t.stepPrice[k] = c.steps[k].price;
        t.totalUnits += c.steps[k].quantity;
    }
    return t;
}

// The ORDER command behind each ACCEPTED event, by the event's position in the list of events.
using AcceptedFrom = std::map<std::size_t, BatchCommand>;

// What the engine told the world, as a recorder should have written it down, worked out straight from the
// events and the commands that were accepted, and with none of the recorder's own checks.
inline std::vector<recorder::BatchRecord> recordsOf(const std::vector<BatchEvent>& events, const AcceptedFrom& acceptedFrom) {
    using recorder::RowSide;
    std::vector<recorder::BatchRecord> out;
    recorder::BatchRecord current;
    std::map<std::pair<RowSide, std::uint32_t>, BatchCommand> accepted; // in the open batch
    auto side = [](const BatchEvent& e) { return e.side == Side::BID ? RowSide::BID : RowSide::ASK; };
    for (std::size_t at = 0; at < events.size(); ++at) {
        const BatchEvent& e = events[at];
        if (e.type == EventType::ACCEPTED) {
            accepted[{ side(e), e.rank }] = acceptedFrom.at(at);
        } else if (e.type == EventType::FILL) {
            current.fills.push_back(recorder::FillRow{ side(e), e.rank, e.hour, e.accelerator, e.interconnect, e.region,
                                                       e.price, e.units, e.lifted });
        } else if (e.type == EventType::ORDER_DONE) {
            recorder::OrderRow row;
            row.side = side(e);
            row.rank = e.rank;
            row.participant = e.participantId;
            row.token = e.orderToken;
            row.terms.totalUnits = e.units + e.unfilledUnits;
            row.filled = e.units;
            row.lifted = e.liftedUnits;
            row.amount = e.amount;
            current.orders.push_back(row);
        } else if (e.type == EventType::BATCH_DONE || e.type == EventType::BATCH_VOIDED) {
            recorder::BatchHeader& h = current.header;
            h.batchId = e.batchId;
            h.status = e.type == EventType::BATCH_DONE ? recorder::BatchStatus::CLEARED : recorder::BatchStatus::VOIDED;
            h.voidReason = static_cast<std::uint8_t>(e.voidReason);
            h.firstHour = e.hour;
            h.eventSeq = e.sequence_number;
            h.bids = e.bids;
            h.asks = e.asks;
            h.arcs = e.arcs;
            h.pivots = e.pivots;
            h.units = e.units;
            h.liftedUnits = e.liftedUnits;
            h.welfare = e.amount;
            h.rankSum = e.rankSum;
            h.fills = e.fills;
            h.certified = e.certified;
            h.solverVersion = SOLVER_VERSION;
            for (recorder::OrderRow& row : current.orders) {
                const std::uint32_t total = row.terms.totalUnits;
                row.terms = termsOfCommand(accepted.at({ row.side, row.rank }), h.firstHour);
                EXPECT_EQ(row.terms.totalUnits, total) << "the quantity that the order was done with is the one it was sent with";
            }
            std::map<std::pair<HourIndex, std::uint8_t>, recorder::AcceleratorPriceRow> models;
            for (const recorder::FillRow& f : current.fills) {
                if (f.side != RowSide::BID) continue;
                const auto [it, fresh] = models.try_emplace({ f.hour, f.accelerator });
                recorder::AcceleratorPriceRow& p = it->second;
                if (fresh) {
                    p.hour = f.hour;
                    p.accelerator = f.accelerator;
                    p.priceMin = p.priceMax = f.price;
                }
                p.units += f.units;
                p.amount += static_cast<std::int64_t>(f.units) * f.price;
                p.priceMin = std::min(p.priceMin, f.price);
                p.priceMax = std::max(p.priceMax, f.price);
            }
            for (const auto& [key, price] : models) current.prices.push_back(price);
            sortCanonical(current);
            h.digest = batchDigest(current);
            out.push_back(std::move(current));
            current = recorder::BatchRecord{};
            accepted.clear();
        }
    }
    return out;
}

inline std::vector<recorder::BatchRecord> recordsOf(const enginefix::Harness& harness) {
    return recordsOf(harness.events, harness.acceptedFrom);
}

inline void expectStoreHolds(const recorder::ResultsStore& store, const recorder::BatchRecord& want) {
    const BatchId id = want.header.batchId;
    SCOPED_TRACE("batch " + std::to_string(id));
    EXPECT_EQ(store.batch(id), want.header);
    EXPECT_EQ(store.orders(id), want.orders);
    EXPECT_EQ(store.fills(id), want.fills);
    EXPECT_EQ(store.prices(id), want.prices);
    EXPECT_TRUE(store.intact(id));
}

} // namespace exfix
