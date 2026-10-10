#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "MiniJson.hpp"

// A client of the results API for the tests: one request on its own connection, as any client makes it.
namespace httpfix {

using namespace std::chrono_literals;

struct Reply {
    int status{ 0 }; // 0 when no answer came
    std::string head;
    std::string body;
};

// Sends the parts of a request, with a pause between them, and reads until the server closes.
inline Reply roundTrip(std::uint16_t port, const std::vector<std::string>& parts, bool halfClose = false,
                       std::chrono::milliseconds pause = 0ms, std::chrono::milliseconds limit = 5s) {
    Reply reply;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return reply;
    }
    for (std::size_t part = 0; part < parts.size(); ++part) {
        if (part > 0) std::this_thread::sleep_for(pause);
        std::size_t sent = 0;
        while (sent < parts[part].size()) {
            const ssize_t n = ::send(fd, parts[part].data() + sent, parts[part].size() - sent, MSG_NOSIGNAL);
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
    }
    if (halfClose) ::shutdown(fd, SHUT_WR);

    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        pollfd p{ fd, POLLIN, 0 };
        if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) break;
        char buffer[8192];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        received.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);

    const std::size_t end = received.find("\r\n\r\n");
    if (end == std::string::npos) return reply;
    reply.head = received.substr(0, end);
    reply.body = received.substr(end + 4);
    if (reply.head.compare(0, 9, "HTTP/1.1 ") == 0) reply.status = std::atoi(reply.head.c_str() + 9);
    return reply;
}

inline Reply roundTrip(std::uint16_t port, const std::string& request, bool halfClose = false) {
    return roundTrip(port, std::vector<std::string>{ request }, halfClose);
}

inline Reply get(std::uint16_t port, const std::string& target) {
    return roundTrip(port, "GET " + target + " HTTP/1.1\r\nHost: test\r\n\r\n");
}

// The body, which must be JSON; an empty value when it is not (and the caller's checks then fail).
inline minijson::Value jsonOf(const Reply& reply) {
    std::optional<minijson::Value> value = minijson::parse(reply.body);
    return value ? *value : minijson::Value{};
}

} // namespace httpfix
