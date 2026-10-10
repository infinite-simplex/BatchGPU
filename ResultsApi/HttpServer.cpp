#include "HttpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace api {

namespace {

constexpr auto POLL_SLICE = std::chrono::milliseconds(100);
constexpr auto ERROR_REPLY_TIME = std::chrono::milliseconds(2000);
constexpr auto LINGER = std::chrono::milliseconds(250);

} // namespace

HttpServer::HttpServer(Options options, HandlerFactory factory) : options_(std::move(options)), factory_(std::move(factory)) {
    if (options_.workers == 0) options_.workers = 1;
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
    if (running_.exchange(true)) return;
    try {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listenFd_ < 0) throw std::system_error(errno, std::generic_category(), "cannot create the listening socket");
        const int on = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options_.port);
        if (::inet_pton(AF_INET, options_.bindAddress.c_str(), &address.sin_addr) != 1) {
            throw std::invalid_argument("not an IPv4 address: " + options_.bindAddress);
        }
        const std::string where = options_.bindAddress + ":" + std::to_string(options_.port);
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::system_error(errno, std::generic_category(), "cannot bind " + where);
        }
        if (::listen(listenFd_, 128) != 0) throw std::system_error(errno, std::generic_category(), "cannot listen on " + where);
        socklen_t length = sizeof(address);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);

        for (std::size_t i = 0; i < options_.workers; ++i) workers_.emplace_back(&HttpServer::work, this);
    } catch (...) {
        running_ = false;
        for (std::thread& worker : workers_) worker.join();
        workers_.clear();
        if (listenFd_ >= 0) ::close(listenFd_);
        listenFd_ = -1;
        throw;
    }
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    for (std::thread& worker : workers_) worker.join();
    workers_.clear();
    if (listenFd_ >= 0) ::close(listenFd_);
    listenFd_ = -1;
}

void HttpServer::work() {
    Handler handler = factory_();
    while (running_.load(std::memory_order_relaxed)) {
        pollfd p{ listenFd_, POLLIN, 0 };
        if (::poll(&p, 1, static_cast<int>(POLL_SLICE.count())) <= 0) continue;
        const int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue; // another worker took it
        serve(fd, handler);
        ::close(fd);
    }
}

void HttpServer::serve(int fd, Handler& handler) {
    const auto deadline = std::chrono::steady_clock::now() + options_.requestTimeout;
    std::string received;
    Request request;
    ParseStatus status = ParseStatus::INCOMPLETE;
    while (status == ParseStatus::INCOMPLETE) {
        if (!waitFor(fd, POLLIN, deadline)) {
            if (running_.load(std::memory_order_relaxed)) {
                finish(fd, errorResponse(408, "the request did not arrive in time"), ERROR_REPLY_TIME);
            }
            return;
        }
        char chunk[2048];
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return; // the client went away, and there is nobody to answer
        received.append(chunk, static_cast<std::size_t>(n));
        status = parseRequest(received, request);
    }

    Response response;
    switch (status) {
    case ParseStatus::OK:
        try {
            response = handler(request);
        } catch (const std::exception& e) {
            std::cerr << "ResultsApi: a request failed: " << e.what() << "\n";
            response = errorResponse(500, "internal error");
        }
        break;
    case ParseStatus::HEADER_TOO_LARGE:
        response = errorResponse(431, "the request line and headers are over 8192 bytes");
        break;
    case ParseStatus::VERSION_NOT_SUPPORTED:
        response = errorResponse(505, "only HTTP/1.0 and HTTP/1.1 are served");
        break;
    default:
        response = errorResponse(400, "the request could not be read");
        break;
    }
    finish(fd, response, options_.requestTimeout);
}

void HttpServer::finish(int fd, const Response& response, std::chrono::milliseconds sendTime) const {
    sendAll(fd, render(response), std::chrono::steady_clock::now() + sendTime);
    ::shutdown(fd, SHUT_WR);
    // Closing a socket that still has input to read resets the connection, and the client may lose the answer. So
    // read what the client still sends until it closes, for a moment.
    const auto deadline = std::chrono::steady_clock::now() + LINGER;
    char scratch[2048];
    while (waitFor(fd, POLLIN, deadline)) {
        if (::recv(fd, scratch, sizeof(scratch), 0) <= 0) return;
    }
}

bool HttpServer::waitFor(int fd, short events, std::chrono::steady_clock::time_point deadline) const {
    for (;;) {
        if (!running_.load(std::memory_order_relaxed)) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1);
        pollfd p{ fd, events, 0 };
        const int ready = ::poll(&p, 1, static_cast<int>(std::min(left, POLL_SLICE).count()));
        if (ready > 0) return true; // ready, or hung up: the recv or send says which
        if (ready < 0 && errno != EINTR) return false;
    }
}

bool HttpServer::sendAll(int fd, std::string_view data, std::chrono::steady_clock::time_point deadline) const {
    while (!data.empty()) {
        if (!waitFor(fd, POLLOUT, deadline)) return false;
        const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

} // namespace api
