#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "HttpServer.hpp"
#include "ResultsApi.hpp"

namespace {
std::atomic<bool> g_shutdownRequested{ false };
void handleSignal(int) { g_shutdownRequested.store(true); }
} // namespace

int main(int argc, char** argv) {
    api::HttpServer::Options options;
    api::ApiOptions apiOptions;
    std::vector<std::string> paths;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
                return argv[++i];
            };

            if (arg == "--db") {
                paths.push_back(next());
            } else if (arg == "--bind") {
                options.bindAddress = next();
            } else if (arg == "--port") {
                const int port = std::stoi(next());
                if (port < 0 || port > 65535) throw std::runtime_error("--port is from 0 to 65535");
                options.port = static_cast<std::uint16_t>(port);
            } else if (arg == "--workers") {
                const int workers = std::stoi(next());
                if (workers < 1 || workers > 256) throw std::runtime_error("--workers is from 1 to 256");
                options.workers = static_cast<std::size_t>(workers);
            } else if (arg == "--open-orders") {
                apiOptions.serveOpenOrders = true;
            } else {
                throw std::runtime_error("Unknown argument: " + arg);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    if (paths.empty()) {
        std::cerr << "--db <path> is required, once for each recorder's file\n";
        return 1;
    }

    std::vector<api::RecorderFile> files;
    for (std::size_t i = 0; i < paths.size(); ++i) files.push_back(api::RecorderFile{ "recorder-" + std::to_string(i + 1), paths[i] });

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout.setf(std::ios_base::unitbuf);

    api::HttpServer server(options, [files, apiOptions] {
        auto results = std::make_shared<api::ResultsApi>(files, apiOptions);
        return api::HttpServer::Handler([results](const api::Request& request) { return results->handle(request); });
    });
    try {
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "ResultsApi cannot start: " << e.what() << "\n";
        return 1;
    }

    if (options.bindAddress != "127.0.0.1") {
        std::cerr << "ResultsApi: this API has no authentication, so anyone who can reach " << options.bindAddress << ":"
                  << server.port() << " can read every participant's orders\n";
    }
    if (apiOptions.serveOpenOrders) {
        std::cerr << "ResultsApi: --open-orders is on, so anyone who can reach " << options.bindAddress << ":" << server.port()
                  << " can read every bid and ask that has not cleared\n";
    }
    std::cout << "ResultsApi ready: " << files.size() << " recorder file(s), listening on " << options.bindAddress << ":"
              << server.port() << "\n";

    while (!g_shutdownRequested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "ResultsApi shutting down...\n";
    server.stop();
    return 0;
}
