// A command-line order-entry client for the gateway. It reads commands from the terminal or from a pipe,
// sends them as the frames of NetworkService/BatchWire.hpp, and prints every event that comes back on the
// connection. Results go to the connection that sent the order, so a client must stay connected from the
// order to the clear. That is why this is a session and not one command per run.
//
//   build/Client/BatchClient                      type commands; 'help' lists them
//   build/Client/BatchClient < script.txt         run a script, then print what arrives for --linger seconds
//
// Prices are ticks of $0.01 per node-hour. An order's hours are offsets from the first hour the open batch
// sells, so "hours=0-3" is the first four hours.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "BatchEventFrame.hpp"
#include "BatchWire.hpp"
#include "Catalog.hpp"
#include "Schedule.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

__attribute__((format(printf, 1, 2))) std::string strf(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> split(const std::string& s, char separator) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(s);
    while (std::getline(in, part, separator)) parts.push_back(part);
    return parts;
}

std::uint64_t parseNumber(const std::string& text, std::uint64_t max, const std::string& what) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        throw UsageError(what + " must be a whole number, not '" + text + "'");
    }
    std::uint64_t value = 0;
    try {
        value = std::stoull(text);
    } catch (const std::exception&) {
        throw UsageError(what + " is too large: " + text);
    }
    if (value > max) throw UsageError(what + " is over " + std::to_string(max) + ": " + text);
    return value;
}

std::string dollars(std::int64_t ticks) {
    const bool negative = ticks < 0;
    const std::uint64_t magnitude = negative ? 0 - static_cast<std::uint64_t>(ticks) : static_cast<std::uint64_t>(ticks);
    return strf("%s$%llu.%02llu", negative ? "-" : "", static_cast<unsigned long long>(magnitude / 100),
                static_cast<unsigned long long>(magnitude % 100));
}

std::string hourText(std::uint64_t hour) {
    const std::time_t seconds = static_cast<std::time_t>(hour * schedule::SECONDS_PER_HOUR);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:00Z", &utc);
    return buffer;
}

std::string timeText(std::uint64_t seconds) {
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm utc{};
    gmtime_r(&t, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%MZ", &utc);
    return buffer;
}

std::uint64_t unixSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------------------------------------
// Catalog names to masks

template <std::size_t N>
std::uint32_t parseNames(const std::string& value, const char* const (&names)[N], const std::string& what) {
    if (lower(value) == "all") return static_cast<std::uint32_t>((std::uint64_t{ 1 } << N) - 1);
    std::uint32_t mask = 0;
    for (const std::string& token : split(value, ',')) {
        bool found = false;
        for (std::size_t i = 0; i < N; ++i) {
            if (lower(token) == lower(names[i])) {
                mask |= std::uint32_t{ 1 } << i;
                found = true;
            }
        }
        if (!found) {
            std::string options;
            for (std::size_t i = 0; i < N; ++i) options += std::string(i ? ", " : "") + names[i];
            throw UsageError("no " + what + " called '" + token + "'. Choose from: " + options + ", or all");
        }
    }
    if (mask == 0) throw UsageError(what + " names nothing");
    return mask;
}

std::uint32_t parseAccelerators(const std::string& value) {
    const char* names[catalog::ACCELERATOR_COUNT];
    for (std::size_t i = 0; i < catalog::ACCELERATOR_COUNT; ++i) names[i] = catalog::ACCELERATORS[i].name;
    return parseNames(value, names, "accelerator");
}

// "0-3,5,7-8" is the hours at offsets 0, 1, 2, 3, 5, 7 and 8 from the batch's first sellable hour. An offset
// past the batch's window is the server's to refuse; one past the mask is not representable.
std::uint64_t parseHours(const std::string& value) {
    std::uint64_t mask = 0;
    for (const std::string& token : split(value, ',')) {
        const std::vector<std::string> ends = split(token, '-');
        if (ends.empty() || ends.size() > 2) throw UsageError("hours must look like 0-3,5: '" + token + "'");
        const std::uint64_t last = catalog::SLOT_MASK_BITS - 1;
        const std::uint64_t from = parseNumber(ends[0], last, "an hour offset");
        const std::uint64_t to = ends.size() == 2 ? parseNumber(ends[1], last, "an hour offset") : from;
        if (to < from) throw UsageError("the range '" + token + "' runs backwards");
        for (std::uint64_t h = from; h <= to; ++h) mask |= std::uint64_t{ 1 } << h;
    }
    if (mask == 0) throw UsageError("hours names nothing");
    return mask;
}

// "4@200,2@150" is 4 node-hours at 200 ticks, then 2 more at 150.
std::vector<CurveStep> parseSteps(const std::string& value) {
    std::vector<CurveStep> steps;
    for (const std::string& token : split(value, ',')) {
        const std::vector<std::string> parts = split(token, '@');
        if (parts.size() != 2) throw UsageError("a step looks like quantity@price, not '" + token + "'");
        steps.push_back({ static_cast<NodeUnits>(parseNumber(parts[0], UINT32_MAX, "a step quantity")),
                          static_cast<PriceTicks>(parseNumber(parts[1], UINT32_MAX, "a step price")) });
    }
    if (steps.empty()) throw UsageError("steps names nothing");
    if (steps.size() > limits::MAX_STEPS) {
        throw UsageError("at most " + std::to_string(limits::MAX_STEPS) + " steps in a curve");
    }
    return steps;
}

// ---------------------------------------------------------------------------------------------------------
// Printing events

const char* nameOf(std::size_t index, const char* const* table, std::size_t count) {
    return index < count ? table[index] : "?";
}

std::string itemText(const BatchEvent& e) {
    const char* accelerator = e.accelerator < catalog::ACCELERATOR_COUNT ? catalog::ACCELERATORS[e.accelerator].name : "?";
    return strf("%s/%s/%s", accelerator, nameOf(e.interconnect, catalog::INTERCONNECTS, catalog::INTERCONNECT_COUNT),
                nameOf(e.region, catalog::REGIONS, catalog::REGION_COUNT));
}

std::string printEvent(const BatchEvent& e) {
    const char* side = e.side == Side::BID ? "bid" : e.side == Side::ASK ? "ask" : "---";
    const std::string who = strf("%s %u/%llu", side, e.participantId, static_cast<unsigned long long>(e.orderToken));
    const std::string head = strf("%-12s batch %llu  ", toString(e.type), static_cast<unsigned long long>(e.batchId));
    switch (e.type) {
    case EventType::ACCEPTED:
    case EventType::CANCELED:
        return head + strf("%s  rank %u  %u node-hours", who.c_str(), e.rank, e.units);
    case EventType::REJECTED: {
        std::string text = head + strf("%s  %s (detail %u)", who.c_str(), toString(e.rejectReason), e.rejectDetail);
        if (e.rejectReason == RejectReason::SLOT_NOT_ELIGIBLE) text += "  hour " + hourText(e.hour);
        return text;
    }
    case EventType::FILL:
        return head + strf("%s  %s  %s  %u @ %s%s", who.c_str(), itemText(e).c_str(), hourText(e.hour).c_str(), e.units,
                           dollars(e.price).c_str(), e.lifted ? "  (lifted)" : "");
    case EventType::ORDER_DONE:
        return head + strf("%s  filled %u  unfilled %u  lifted %u  %s %s", who.c_str(), e.units, e.unfilledUnits,
                           e.liftedUnits, e.side == Side::BID ? "paid" : "received", dollars(e.amount).c_str());
    case EventType::BATCH_DONE:
        return head + strf("first hour %s  traded %u  lifted %u  welfare %s  fills %u  bids %u  asks %u  arcs %u  pivots %u  "
                           "certified %s",
                           hourText(e.hour).c_str(), e.units, e.liftedUnits, dollars(e.amount).c_str(), e.fills, e.bids,
                           e.asks, e.arcs, e.pivots, e.certified ? "yes" : "NO");
    case EventType::BATCH_VOIDED:
        return head + strf("first hour %s  nothing traded: %s  bids %u  asks %u  arcs %u  pivots %u",
                           hourText(e.hour).c_str(), toString(e.voidReason), e.bids, e.asks, e.arcs, e.pivots);
    }
    return head + "?";
}

// ---------------------------------------------------------------------------------------------------------
// Commands

const char* const HELP =
    "Commands (key=value, in any order). Prices are ticks of $0.01 per node-hour.\n"
    "  bid    p=<participant> t=<token> accel=<names|all> link=<names|all> region=<names|all>\n"
    "         steps=<qty@price,...> [hours=0-3,5] [cap=<nodes per hour>]\n"
    "  ask    the same, but accel, link and region each name exactly one value\n"
    "  cancel p=<participant> t=<token>\n"
    "  clear  [id=<batch>]           with --no-ticker, ends the open batch; with the ticker, leave it to the clock\n"
    "  catalog                        the names an order can use, and the hours the open batch sells\n"
    "  wait   <seconds>               hold the rest of a script while events arrive\n"
    "  demo                           one ask, three bids and a clear, so there is something to read\n"
    "  help, quit\n"
    "\n"
    "A bid's steps must not rise in price, and an ask's must not fall. hours counts from the first hour the\n"
    "open batch sells (see 'catalog'); it defaults to 0. cap defaults to the order's whole quantity, up to 1000.\n"
    "Example: bid p=2001 t=1 accel=A100-80GB,H100-80GB link=all region=US_EAST steps=4@200,2@150 hours=0-1\n";

struct Options {
    std::string host{ "127.0.0.1" };
    std::uint16_t port{ 10001 };
    std::optional<BatchId> batch; // fixed open batch, for a gateway started with --no-ticker
    double lingerSeconds{ 2.0 };
};

class Client {
public:
    explicit Client(Options options) : options_(std::move(options)) {}

    ~Client() {
        if (fd_ != -1) ::close(fd_);
    }

    bool connect() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options_.port);
        if (inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
            std::cerr << "not an IPv4 address: " << options_.host << "\n";
            return false;
        }
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            std::cerr << "cannot connect to " << options_.host << ":" << options_.port << ": " << std::strerror(errno)
                      << ". Is the gateway running?\n";
            return false;
        }
        return true;
    }

    int run() {
        const bool terminal = isatty(STDIN_FILENO) != 0;
        if (terminal) std::cout << "Connected to " << options_.host << ":" << options_.port << ". 'help' lists commands.\n";
        std::cout << std::flush;

        bool stdinOpen = true;
        std::string partial;
        std::deque<std::string> lines;
        Clock::time_point waitUntil = Clock::now();
        Clock::time_point lastActivity = Clock::now();
        const auto linger = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(options_.lingerSeconds));

        for (;;) {
            while (!quit_ && !lines.empty() && Clock::now() >= waitUntil) {
                const std::string line = lines.front();
                lines.pop_front();
                try {
                    if (const double wait = execute(line); wait > 0) {
                        waitUntil = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(wait));
                    }
                } catch (const UsageError& e) {
                    std::cout << "error: " << e.what() << "\n";
                } catch (const std::runtime_error& e) {
                    std::cerr << e.what() << "\n";
                    return 1;
                }
                lastActivity = Clock::now();
                if (terminal && lines.empty() && !quit_) std::cout << "> " << std::flush;
            }
            if (quit_) return 0;

            const auto now = Clock::now();
            if (!stdinOpen && lines.empty() && now >= waitUntil && now - lastActivity >= linger) return 0;

            // Sleep until the wait ends, the linger ends, or something arrives.
            int timeoutMs = -1;
            auto until = [&](Clock::time_point t) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t - now).count();
                const int clamped = static_cast<int>(std::max<std::int64_t>(ms, 0)) + 1;
                timeoutMs = timeoutMs < 0 ? clamped : std::min(timeoutMs, clamped);
            };
            if (now < waitUntil) until(waitUntil);
            if (!stdinOpen && lines.empty()) until(std::max(waitUntil, lastActivity + linger));

            pollfd fds[2] = { { fd_, POLLIN, 0 }, { stdinOpen ? STDIN_FILENO : -1, POLLIN, 0 } };
            if (::poll(fds, 2, timeoutMs) < 0) {
                if (errno == EINTR) continue;
                std::cerr << "poll: " << std::strerror(errno) << "\n";
                return 1;
            }
            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                if (!receive()) {
                    std::cout << "The gateway closed the connection. It does that for a frame it cannot read.\n";
                    return 1;
                }
                lastActivity = Clock::now();
            }
            if (stdinOpen && (fds[1].revents & (POLLIN | POLLHUP)) != 0) {
                char buffer[4096];
                const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof(buffer));
                if (n <= 0) {
                    stdinOpen = false;
                    if (!partial.empty()) lines.push_back(partial);
                    partial.clear();
                } else {
                    partial.append(buffer, static_cast<std::size_t>(n));
                    for (std::size_t newline; (newline = partial.find('\n')) != std::string::npos;) {
                        lines.push_back(partial.substr(0, newline));
                        partial.erase(0, newline + 1);
                    }
                }
            }
        }
    }

private:
    // The batch that orders are being taken for. The ticker's clear with id n opens batch n + 1, so with the
    // ticker running it follows from the clock; --batch fixes it for a gateway started with --no-ticker.
    BatchId openBatch() const {
        if (options_.batch) return *options_.batch;
        const std::optional<BatchId> due = schedule::latestClearId(schedule::Schedule{}, unixSeconds());
        return due ? schedule::nextBatchId(*due) : 0;
    }

    HourIndex firstHour() const {
        return static_cast<HourIndex>(schedule::firstSlotHour(schedule::Schedule{}, openBatch()));
    }

    void send(const std::vector<char>& frame) {
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const ssize_t n = ::send(fd_, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
            sent += static_cast<std::size_t>(n);
        }
    }

    // False when the gateway has closed the connection.
    bool receive() {
        char buffer[4096];
        const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n == 0) return false;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        pending_.insert(pending_.end(), buffer, buffer + n);
        while (pending_.size() >= wire::EVENT_FRAME_SIZE) {
            if (const auto event = wire::decodeEvent(pending_.data(), wire::EVENT_FRAME_SIZE)) {
                std::cout << printEvent(*event) << "\n";
            } else {
                std::cout << "(a frame that is not an event of version " << int{ wire::PROTOCOL_VERSION } << ")\n";
            }
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(wire::EVENT_FRAME_SIZE));
        }
        std::cout << std::flush;
        return true;
    }

    using Args = std::map<std::string, std::string>;

    static Args parseArgs(const std::vector<std::string>& tokens, const std::set<std::string>& allowed) {
        Args args;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            const std::size_t equals = tokens[i].find('=');
            if (equals == std::string::npos || equals == 0) throw UsageError("expected key=value, got '" + tokens[i] + "'");
            const std::string key = lower(tokens[i].substr(0, equals));
            if (allowed.count(key) == 0) throw UsageError("unknown key '" + key + "' for " + tokens[0]);
            args[key] = tokens[i].substr(equals + 1);
        }
        return args;
    }

    static const std::string& need(const Args& args, const std::string& key) {
        const auto it = args.find(key);
        if (it == args.end()) throw UsageError("missing " + key + "=");
        return it->second;
    }

    BatchCommand buildOrder(Side side, const Args& args) const {
        BatchCommand order{};
        order.type = CommandType::ORDER;
        order.side = side;
        order.participantId = static_cast<ParticipantId>(parseNumber(need(args, "p"), UINT32_MAX, "p"));
        order.orderToken = parseNumber(need(args, "t"), UINT64_MAX, "t");
        order.acceleratorMask = parseAccelerators(need(args, "accel"));
        order.interconnectMask = static_cast<std::uint8_t>(parseNames(need(args, "link"), catalog::INTERCONNECTS, "interconnect"));
        order.regionMask = parseNames(need(args, "region"), catalog::REGIONS, "region");
        order.baseHour = firstHour();
        order.slotMask = args.count("hours") ? parseHours(args.at("hours")) : 1;

        const std::vector<CurveStep> steps = parseSteps(need(args, "steps"));
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            order.steps[i] = steps[i];
            total += steps[i].quantity;
        }
        order.stepCount = static_cast<std::uint8_t>(steps.size());
        order.perSlotCap = args.count("cap") ? static_cast<NodeUnits>(parseNumber(args.at("cap"), UINT32_MAX, "cap"))
                                             : static_cast<NodeUnits>(std::min<std::uint64_t>(total, limits::MAX_PER_SLOT_CAP));
        return order;
    }

    void sendOrder(Side side, const std::string& line) {
        const std::vector<std::string> tokens = tokenize(line);
        const Args args = parseArgs(tokens, { "p", "t", "accel", "link", "region", "steps", "hours", "cap" });
        send(wire::encodeOrder(buildOrder(side, args)));
    }

    static std::vector<std::string> tokenize(const std::string& line) {
        std::vector<std::string> tokens;
        std::istringstream in(line);
        for (std::string token; in >> token;) tokens.push_back(token);
        return tokens;
    }

    void printCatalog() const {
        const BatchId batch = openBatch();
        const HourIndex first = firstHour();
        std::cout << "open batch " << batch << ", clears at " << timeText(schedule::clearSeconds(schedule::Schedule{}, batch))
                  << "\n";
        std::cout << "sells hours 0.." << limits::SLOT_WINDOW - 1 << " = " << hourText(first) << " to "
                  << hourText(std::uint64_t{ first } + limits::SLOT_WINDOW - 1) << "\n";
        std::cout << "accelerators:";
        for (const auto& a : catalog::ACCELERATORS) std::cout << " " << a.name;
        std::cout << "\ninterconnects:";
        for (const char* name : catalog::INTERCONNECTS) std::cout << " " << name;
        std::cout << "\nregions:";
        for (const char* name : catalog::REGIONS) std::cout << " " << name;
        std::cout << "\n";
    }

    // Returns the seconds to hold the rest of the input, or 0.
    double execute(const std::string& line) {
        const std::vector<std::string> tokens = tokenize(line);
        if (tokens.empty() || tokens[0][0] == '#') return 0;
        const std::string command = lower(tokens[0]);

        if (command == "quit" || command == "exit") {
            quit_ = true;
        } else if (command == "help") {
            std::cout << HELP;
        } else if (command == "catalog") {
            printCatalog();
        } else if (command == "bid") {
            sendOrder(Side::BID, line);
        } else if (command == "ask") {
            sendOrder(Side::ASK, line);
        } else if (command == "cancel") {
            const Args args = parseArgs(tokens, { "p", "t" });
            send(wire::encodeCancel(static_cast<ParticipantId>(parseNumber(need(args, "p"), UINT32_MAX, "p")),
                                    parseNumber(need(args, "t"), UINT64_MAX, "t")));
        } else if (command == "clear") {
            const Args args = parseArgs(tokens, { "id" });
            const BatchId id = args.count("id") ? parseNumber(args.at("id"), UINT64_MAX, "id") : openBatch();
            send(wire::encodeBatchClear(id));
            if (options_.batch) options_.batch = schedule::nextBatchId(id);
        } else if (command == "wait") {
            if (tokens.size() != 2) throw UsageError("wait <seconds>");
            return static_cast<double>(parseNumber(tokens[1], 3600, "seconds"));
        } else if (command == "demo") {
            runDemo();
            return 1.5;
        } else {
            throw UsageError("unknown command '" + tokens[0] + "'. 'help' lists them");
        }
        return 0;
    }

    // One seller with H100s in US_EAST for the batch's first two hours, and three buyers. The first accepts an
    // A100 or an H100 on either interconnect, and any award above its A100 is a lift. The second takes only
    // an H100 on NVLink. The third bids under the ask's price, so it does not trade.
    void runDemo() {
        const std::vector<std::string> script = {
            "ask p=1001 t=1 accel=H100-80GB link=NVLink region=US_EAST hours=0-1 cap=5 steps=6@100,4@140",
            "bid p=2001 t=1 accel=A100-80GB,H100-80GB link=all region=US_EAST hours=0-1 cap=4 steps=4@200,2@150",
            "bid p=2002 t=1 accel=H100-80GB link=NVLink region=US_EAST hours=0 cap=3 steps=3@130",
            "bid p=2003 t=1 accel=all link=all region=US_EAST hours=0-1 cap=2 steps=4@90",
        };
        for (const std::string& step : script) {
            std::cout << "> " << step << "\n";
            execute(step);
        }
        std::cout << "> clear\n";
        execute("clear");
    }

    Options options_;
    int fd_{ -1 };
    bool quit_{ false };
    std::vector<char> pending_;
};

void usage() {
    std::cerr << "usage: BatchClient [--host IPV4] [--port N] [--batch N] [--linger SECONDS]\n"
                 "  --batch N   treat batch N as the open one, for a gateway started with --no-ticker (the first\n"
                 "              batch is 0). Without it the open batch follows the clock, as the ticker's clears do.\n"
                 "  --linger    after the input ends, keep printing events until none arrive for this long (default 2)\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw UsageError("missing value for " + arg);
                return argv[++i];
            };
            if (arg == "--host") options.host = next();
            else if (arg == "--port") options.port = static_cast<std::uint16_t>(parseNumber(next(), 65535, "--port"));
            else if (arg == "--batch") options.batch = parseNumber(next(), UINT64_MAX, "--batch");
            else if (arg == "--linger") options.lingerSeconds = static_cast<double>(parseNumber(next(), 3600, "--linger"));
            else if (arg == "--help" || arg == "-h") {
                usage();
                return 0;
            } else throw UsageError("unknown argument: " + arg);
        }
    } catch (const UsageError& e) {
        std::cerr << e.what() << "\n";
        usage();
        return 2;
    }

    Client client(options);
    if (!client.connect()) return 1;
    return client.run();
}
