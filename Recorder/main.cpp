#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "BatchCommand.hpp"
#include "BatchEngine.hpp"
#include "EngineBook.hpp"
#include "MatchingService.hpp"
#include "MulticastIngressReceiver.hpp"
#include "NetworkConfig.hpp"
#include "RecorderPolicy.hpp"
#include "ResultsStore.hpp"
#include "SPSCQueue.hpp"

namespace {
std::atomic<bool> g_shutdownRequested{ false };
void handleSignal(int) { g_shutdownRequested.store(true); }
} // namespace

using Engine = BatchEngine<recorder::RecorderPolicy>;
using Matcher = MatchingService<BatchCommand, Engine>;

int main(int argc, char** argv) {
    NetworkConfig config;
    std::string dbPath;
    std::string name = "recorder-1";
    int statsIntervalMs = 0;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
                return argv[++i];
            };

            if (arg == "--db") dbPath = next();
            else if (arg == "--multicast-ip") config.multicastIp = next();
            else if (arg == "--multicast-port") config.multicastPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--retransmit-ip") config.retransmitServerIp = next();
            else if (arg == "--retransmit-port") config.retransmitServerPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--ingress-spin-micros") config.ingressSpinMicros = static_cast<uint32_t>(std::stoul(next()));
            else if (arg == "--id") name = next();
            else if (arg == "--stats-interval-ms") statsIntervalMs = std::stoi(next());
            else throw std::runtime_error("Unknown argument: " + arg);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    if (dbPath.empty()) {
        std::cerr << "--db <path> is required: the SQLite file this recorder writes (on a local disk)\n";
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout.setf(std::ios_base::unitbuf);

    std::unique_ptr<recorder::ResultsStore> store;
    try {
        store = std::make_unique<recorder::ResultsStore>(dbPath);
        store->clearLive();
    } catch (const recorder::StoreError& e) {
        std::cerr << "Recorder[" << name << "] cannot open its database: " << e.what() << "\n";
        return 1;
    }

    auto inboundQueue = std::make_unique<SPSCQueue<BatchCommand>>();
    recorder::RecorderPolicy policy(*store, std::cerr);
    Engine engine(policy);
    policy.attach(recorder::bookLookupOf(engine.book()));
    Matcher matcher(*inboundQueue, engine);
    MulticastIngressReceiver<BatchCommand> receiver(*inboundQueue, config);

    receiver.start();
    matcher.start();

    std::cout << "Recorder[" << name << "] ready: joined " << config.multicastIp << ":" << config.multicastPort
              << ", retransmit via " << config.retransmitServerIp << ":" << config.retransmitServerPort
              << ", writing " << dbPath << "\n";

    auto nextStats = std::chrono::steady_clock::now() + std::chrono::milliseconds(statsIntervalMs);
    while (!g_shutdownRequested.load(std::memory_order_relaxed) && !receiver.hitUnrecoverableGap() &&
           !policy.failed()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        policy.flushLive();
        if (statsIntervalMs > 0 && std::chrono::steady_clock::now() >= nextStats) {
            nextStats += std::chrono::milliseconds(statsIntervalMs);
            std::cout << "[" << name << "] batches=" << policy.written() << " duplicates=" << policy.duplicates()
                      << " mismatches=" << policy.mismatches() << " applied_seq=" << matcher.lastAppliedSeq() << "\n";
        }
    }

    if (receiver.hitUnrecoverableGap()) {
        std::cerr << "Recorder[" << name << "] stopping: unrecoverable sequence gap, its record has a hole.\n";
    }
    if (policy.failed()) {
        std::cerr << "Recorder[" << name << "] stopping: " << policy.error() << "\n";
    }
    std::cout << "Recorder[" << name << "] shutting down...\n";
    matcher.stop();
    receiver.stop();
    policy.flushLive();
    if (policy.failed()) return 3;
    return receiver.hitUnrecoverableGap() ? 2 : 0;
}
