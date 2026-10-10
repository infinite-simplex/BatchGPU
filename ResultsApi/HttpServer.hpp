#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Http.hpp"

namespace api {

// A small HTTP/1.1 server: a fixed number of worker threads, each taking one connection at a time, reading
// one request, answering it and closing. That is enough for a read-only API on one machine, and it bounds what
// a client can use: a worker, for at most `requestTimeout`.
class HttpServer {
public:
    struct Options {
        std::string bindAddress{ "127.0.0.1" }; // IPv4
        std::uint16_t port{ 8080 };             // 0 takes a free port
        std::size_t workers{ 8 };
        std::chrono::milliseconds requestTimeout{ 10'000 }; // each for reading the request, and for sending the answer
    };

    using Handler = std::function<Response(const Request&)>;
    // Each worker calls it once, on its own thread, for the handler that it alone will use. The handler may
    // keep what a thread must not share, such as a database connection.
    using HandlerFactory = std::function<Handler()>;

    HttpServer(Options options, HandlerFactory factory);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Throws std::system_error when it cannot listen.
    void start();
    void stop();

    // The port it listens on, once started.
    std::uint16_t port() const { return port_; }

private:
    void work();
    void serve(int fd, Handler& handler);
    void finish(int fd, const Response& response, std::chrono::milliseconds sendTime) const;
    bool waitFor(int fd, short events, std::chrono::steady_clock::time_point deadline) const;
    bool sendAll(int fd, std::string_view data, std::chrono::steady_clock::time_point deadline) const;

    Options options_;
    HandlerFactory factory_;
    int listenFd_{ -1 };
    std::uint16_t port_{ 0 };
    std::atomic<bool> running_{ false };
    std::vector<std::thread> workers_;
};

} // namespace api
