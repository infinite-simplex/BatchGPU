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
#include "BatchEvent.hpp"
#include "EgressPublisher.hpp"
#include "LeaderHeartbeatClient.hpp"
#include "MatchingService.hpp"
#include "MulticastIngressReceiver.hpp"
#include "NetworkConfig.hpp"
#include "SPSCProducerPolicy.hpp"
#include "SPSCQueue.hpp"
#include "Telemetry.hpp"
#include "TelemetryForwarder.hpp"

namespace {
std::atomic<bool> g_shutdownRequested{ false };
void handleSignal(int) { g_shutdownRequested.store(true); }
} // namespace

using Engine = BatchEngine<SPSCProducerPolicy<BatchEvent>>;
using Matcher = MatchingService<BatchCommand, Engine>;

int main(int argc, char** argv) {
    NetworkConfig config;
    std::string name = "replica-1";
    ReplicaId replicaId = INVALID_REPLICA_ID;
    int statsIntervalMs = 0;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
                return argv[++i];
            };

            if (arg == "--multicast-ip") config.multicastIp = next();
            else if (arg == "--multicast-port") config.multicastPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--retransmit-ip") config.retransmitServerIp = next();
            else if (arg == "--retransmit-port") config.retransmitServerPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--egress-ip") config.egressMulticastIp = next();
            else if (arg == "--egress-port") config.egressMulticastPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--arbitration-port") config.arbitrationPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--ingress-spin-micros") config.ingressSpinMicros = static_cast<uint32_t>(std::stoul(next()));
            else if (arg == "--performance-service-ip") config.performanceServiceIp = next();
            else if (arg == "--performance-service-port") config.performanceServicePort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--id") name = next();
            else if (arg == "--replica-id") replicaId = static_cast<ReplicaId>(std::stoi(next()));
            else if (arg == "--stats-interval-ms") statsIntervalMs = std::stoi(next());
            else throw std::runtime_error("Unknown argument: " + arg);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    if (replicaId == INVALID_REPLICA_ID) {
        std::cerr << "--replica-id <n> is required (n > 0): it names this replica to the gateway's arbiter\n";
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout.setf(std::ios_base::unitbuf);

    // Heap-allocated, because these ring buffers are too large for main's stack.
    auto inboundQueue = std::make_unique<SPSCQueue<BatchCommand>>();
    auto eventQueue = std::make_unique<SPSCQueue<BatchEvent, 16384>>();
    auto traceQueue = std::make_unique<SPSCQueue<OrderTrace, 16384>>();

    SPSCProducerPolicy<BatchEvent> policy{ *eventQueue, *traceQueue };
    Engine engine(policy);
    Matcher matcher(*inboundQueue, engine);
    MulticastIngressReceiver<BatchCommand> receiver(*inboundQueue, config);
    LeaderHeartbeatClient<Matcher> heartbeatClient(replicaId, matcher, config);
    EgressPublisher<BatchEvent, 16384, LeaderHeartbeatClient<Matcher>> egressPublisher(*eventQueue, config, heartbeatClient);
    TelemetryForwarder<16384> telemetryForwarder(*traceQueue, config, replicaId);

    receiver.start();
    matcher.start();
    heartbeatClient.start();
    egressPublisher.start();
    telemetryForwarder.start();

    std::cout << "MatchingService[" << name << "] (replica-id " << replicaId << ") ready: joined "
              << config.multicastIp << ":" << config.multicastPort << ", retransmit via " << config.retransmitServerIp
              << ":" << config.retransmitServerPort << ", results to " << config.egressMulticastIp << ":"
              << config.egressMulticastPort << ", arbitration via " << config.retransmitServerIp << ":"
              << config.arbitrationPort << "\n";

    auto nextStats = std::chrono::steady_clock::now() + std::chrono::milliseconds(statsIntervalMs);
    while (!g_shutdownRequested.load(std::memory_order_relaxed) && !receiver.hitUnrecoverableGap()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (statsIntervalMs > 0 && std::chrono::steady_clock::now() >= nextStats) {
            nextStats += std::chrono::milliseconds(statsIntervalMs);
            std::cout << "[" << name << "] leader=" << (heartbeatClient.isLeader() ? "yes" : "no")
                      << " epoch=" << heartbeatClient.currentEpoch() << " applied_seq=" << matcher.lastAppliedSeq()
                      << " telemetry_collisions=" << g_telemetry_arena_collisions.load(std::memory_order_relaxed) << "\n";
        }
    }

    if (receiver.hitUnrecoverableGap()) {
        std::cerr << "MatchingService[" << name << "] stopping: unrecoverable sequence gap, this replica's book can no longer be trusted.\n";
    }
    std::cout << "MatchingService[" << name << "] shutting down...\n";
    // The matcher stops first. Its policy spins on a full event queue, so the publisher must still be
    // draining while the matcher finishes the command it is in.
    matcher.stop();
    telemetryForwarder.stop();
    egressPublisher.stop();
    heartbeatClient.stop();
    receiver.stop();
    return receiver.hitUnrecoverableGap() ? 2 : 0;
}
