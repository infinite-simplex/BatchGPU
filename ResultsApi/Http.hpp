#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace api {

// A request is the line and the headers up to the blank line, and no more than this. The API takes no body.
inline constexpr std::size_t MAX_REQUEST_BYTES = 8192;

struct Request {
    std::string method;
    // The path split at '/', each part with its %XX escapes decoded. "/" has none, and a '/' at the end adds none.
    std::vector<std::string> segments;
    // In the order sent, decoded ('+' is a space).
    std::vector<std::pair<std::string, std::string>> query;
};

enum class ParseStatus {
    INCOMPLETE, // the blank line has not come yet
    OK,
    BAD_REQUEST,
    HEADER_TOO_LARGE,
    VERSION_NOT_SUPPORTED,
};

// Reads what has arrived of one request. HTTP/1.0 and HTTP/1.1, the origin form of the target only, and CRLF line
// ends. Headers are checked for their shape and then ignored, and anything after the blank line is ignored.
ParseStatus parseRequest(std::string_view bytes, Request& out);

// Decodes %XX, and '+' as a space when asked. False for a bad escape or a decoded NUL.
bool percentDecode(std::string_view in, std::string& out, bool plusIsSpace);

struct Response {
    int status{ 200 };
    std::string body; // JSON
    std::string allow; // for a 405
};

const char* statusText(int status);

// {"error": message}. The message is escaped, and is never the client's own text unchecked.
Response errorResponse(int status, std::string_view message);

// The bytes to send: HTTP/1.1, the body's length, and the connection closed after it.
std::string render(const Response& response);

} // namespace api
