#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "BatchCommand.hpp"
#include "BatchEvent.hpp"
#include "BatchEventFrame.hpp"
#include "BatchProtocolHandler.hpp"
#include "BatchTicker.hpp"
#include "EgressGateway.hpp"
#include "NetworkConfig.hpp"
#include "NetworkGateway.hpp"
#include "ReplicaArbiter.hpp"
#include "RetransmitServer.hpp"

namespace {
std::atomic<bool> g_shutdownRequested{ false };
void handleSignal(int) { g_shutdownRequested.store(true); }
} // namespace

using Gateway = NetworkGateway<BatchProtocolHandler, BatchCommand>;

int main(int argc, char** argv) {
    NetworkConfig config;
    std::string sequenceStorePath = "network_sequence_store.dat";
    std::size_t sequenceStoreCapacity = 1'048'576;
    bool runTicker = true;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
                return argv[++i];
            };

            if (arg == "--port") config.ingressListenPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--multicast-ip") config.multicastIp = next();
            else if (arg == "--multicast-port") config.multicastPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--retransmit-port") config.retransmitServerPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--sequence-store") sequenceStorePath = next();
            else if (arg == "--sequence-capacity") sequenceStoreCapacity = std::stoull(next());
            else if (arg == "--egress-ip") config.egressMulticastIp = next();
            else if (arg == "--egress-port") config.egressMulticastPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--broadcast-ip") config.publicBroadcastIp = next();
            else if (arg == "--broadcast-port") config.publicBroadcastPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--arbitration-port") config.arbitrationPort = static_cast<uint16_t>(std::stoi(next()));
            else if (arg == "--no-ticker") runTicker = false;
            else if (arg == "--no-console-echo") config.egressConsoleEcho = false;
            else if (arg == "--session-buffer-bytes") config.sessionOutboundBufferBytes = std::stoull(next());
            else throw std::runtime_error("Unknown argument: " + arg);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout.setf(std::ios_base::unitbuf);

    BatchProtocolHandler handler;
    Gateway gateway(handler, config, sequenceStorePath, sequenceStoreCapacity);
    RetransmitServer<BatchCommand> retransmitServer(gateway.sequenceStore(), config.retransmitServerPort);
    ReplicaArbiter arbiter(config.arbitrationPort);
    EgressGateway<Gateway, BatchEvent, BatchEventEncoder> egressGateway(gateway, config, arbiter);
    BatchTicker ticker(config);

    try {
        gateway.start();
        retransmitServer.start();
        arbiter.start();
        egressGateway.start();
        if (runTicker) ticker.start();
    } catch (const std::exception& e) {
        std::cerr << "NetworkService cannot start: " << e.what() << "\n";
        return 1;
    }

    std::cout << "NetworkService ready: orders on :" << config.ingressListenPort << ", log multicast "
              << config.multicastIp << ":" << config.multicastPort << ", retransmit :" << config.retransmitServerPort
              << ", results from " << config.egressMulticastIp << ":" << config.egressMulticastPort
              << ", public feed to " << config.publicBroadcastIp << ":" << config.publicBroadcastPort
              << ", arbitration :" << config.arbitrationPort << ", ticker " << (runTicker ? "on" : "off") << "\n";

    while (!g_shutdownRequested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    const EgressStats egress = egressGateway.stats();
    std::cout << "NetworkService egress: " << egress.events << " events delivered, " << egress.missingEvents
              << " never arrived from the replicas; client output: " << gateway.droppedOutputBytes()
              << " bytes dropped, " << gateway.pendingOutputBytes() << " still pending\n";
    std::cout << "NetworkService shutting down...\n";
    ticker.stop();
    egressGateway.stop();
    arbiter.stop();
    retransmitServer.stop();
    gateway.stop();
    return 0;
}
