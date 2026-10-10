#include "Http.hpp"

#include <cctype>

#include "Json.hpp"

namespace api {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool isTokenChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

// "HTTP/1.0" and "HTTP/1.1" are served, any other well-formed version is refused as such.
ParseStatus checkVersion(std::string_view version) {
    if (version == "HTTP/1.0" || version == "HTTP/1.1") return ParseStatus::OK;
    const bool shaped = version.size() == 8 && version.substr(0, 5) == "HTTP/" &&
                        std::isdigit(static_cast<unsigned char>(version[5])) && version[6] == '.' &&
                        std::isdigit(static_cast<unsigned char>(version[7]));
    return shaped ? ParseStatus::VERSION_NOT_SUPPORTED : ParseStatus::BAD_REQUEST;
}

// A target is ASCII with no spaces, controls or fragment.
bool isPlainTarget(std::string_view target) {
    for (const unsigned char c : target) {
        if (c <= 0x20 || c >= 0x7F || c == '#') return false;
    }
    return true;
}

bool parseTarget(std::string_view target, Request& out) {
    if (target.empty() || target.front() != '/' || !isPlainTarget(target)) return false;
    const std::size_t question = target.find('?');
    const std::string_view path = target.substr(0, question);
    const std::string_view query = question == std::string_view::npos ? std::string_view() : target.substr(question + 1);

    std::size_t start = 1; // after the leading '/'
    if (path.size() > 1) {
        for (;;) {
            const std::size_t slash = path.find('/', start);
            const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            const bool last = slash == std::string_view::npos;
            if (!(last && part.empty())) { // a '/' at the end adds no part
                std::string decoded;
                if (!percentDecode(part, decoded, false)) return false;
                out.segments.push_back(std::move(decoded));
            }
            if (last) break;
            start = slash + 1;
        }
    }

    for (std::size_t at = 0; at < query.size();) {
        const std::size_t amp = query.find('&', at);
        const std::string_view pair = query.substr(at, amp == std::string_view::npos ? std::string_view::npos : amp - at);
        at = amp == std::string_view::npos ? query.size() : amp + 1;
        if (pair.empty()) continue;
        const std::size_t equals = pair.find('=');
        std::string key;
        std::string value;
        if (!percentDecode(pair.substr(0, equals), key, true)) return false;
        if (equals != std::string_view::npos && !percentDecode(pair.substr(equals + 1), value, true)) return false;
        if (key.empty()) return false;
        out.query.emplace_back(std::move(key), std::move(value));
    }
    return true;
}

// "Name: value", with a name of token characters and a value without controls.
bool isHeaderLine(std::string_view line) {
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    for (std::size_t i = 0; i < colon; ++i) {
        if (!isTokenChar(line[i])) return false;
    }
    for (std::size_t i = colon + 1; i < line.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (c < 0x20 && c != '\t') return false;
        if (c == 0x7F) return false;
    }
    return true;
}

} // namespace

bool percentDecode(std::string_view in, std::string& out, bool plusIsSpace) {
    out.clear();
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) return false;
            const int high = hexValue(in[i + 1]);
            const int low = hexValue(in[i + 2]);
            if (high < 0 || low < 0) return false;
            const int byte = high * 16 + low;
            if (byte == 0) return false;
            out += static_cast<char>(byte);
            i += 2;
        } else if (c == '+' && plusIsSpace) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return true;
}

ParseStatus parseRequest(std::string_view bytes, Request& out) {
    constexpr std::string_view END = "\r\n\r\n";
    const std::size_t end = bytes.find(END);
    if (end == std::string_view::npos) {
        return bytes.size() >= MAX_REQUEST_BYTES ? ParseStatus::HEADER_TOO_LARGE : ParseStatus::INCOMPLETE;
    }
    if (end + END.size() > MAX_REQUEST_BYTES) return ParseStatus::HEADER_TOO_LARGE;

    const std::string_view head = bytes.substr(0, end);
    const std::size_t firstEnd = head.find("\r\n");
    const std::string_view requestLine = head.substr(0, firstEnd);

    // method SP target SP version
    const std::size_t space1 = requestLine.find(' ');
    const std::size_t space2 = space1 == std::string_view::npos ? std::string_view::npos : requestLine.find(' ', space1 + 1);
    if (space2 == std::string_view::npos) return ParseStatus::BAD_REQUEST;
    const std::string_view method = requestLine.substr(0, space1);
    const std::string_view target = requestLine.substr(space1 + 1, space2 - space1 - 1);
    const std::string_view version = requestLine.substr(space2 + 1);
    if (method.empty() || method.size() > 16) return ParseStatus::BAD_REQUEST;
    for (const char c : method) {
        if (c < 'A' || c > 'Z') return ParseStatus::BAD_REQUEST;
    }
    if (const ParseStatus v = checkVersion(version); v != ParseStatus::OK) return v;

    Request request;
    request.method = std::string(method);
    if (!parseTarget(target, request)) return ParseStatus::BAD_REQUEST;

    for (std::size_t at = firstEnd == std::string_view::npos ? head.size() : firstEnd + 2; at < head.size();) {
        const std::size_t lineEnd = head.find("\r\n", at);
        const std::string_view line = head.substr(at, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - at);
        at = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
        if (!isHeaderLine(line)) return ParseStatus::BAD_REQUEST;
    }

    out = std::move(request);
    return ParseStatus::OK;
}

const char* statusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    }
    return "Unknown";
}

Response errorResponse(int status, std::string_view message) {
    JsonWriter json;
    json.beginObject().key("error").string(message).endObject();
    return Response{ status, json.take(), "" };
}

std::string render(const Response& response) {
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " + statusText(response.status) + "\r\n";
    out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    if (!response.allow.empty()) out += "Allow: " + response.allow + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += response.body;
    return out;
}

} // namespace api
