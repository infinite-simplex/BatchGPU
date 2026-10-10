#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A reader of JSON for the tests of the API, and a check of its writer: it accepts exactly the grammar of RFC 8259
// (a text that is one value, and nothing after it) and gives nothing for anything else.
namespace minijson {

struct Value {
    enum class Kind { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    Kind kind{ Kind::NUL };
    bool boolean{ false };
    std::string text;                                   // the digits of a number, or the characters of a string
    std::vector<Value> items;                           // an array's elements
    std::vector<std::pair<std::string, Value>> members; // an object's, in the order written

    bool isNull() const { return kind == Kind::NUL; }

    const Value* find(std::string_view key) const {
        for (const auto& [name, value] : members) {
            if (name == key) return &value;
        }
        return nullptr;
    }

    const Value& at(std::string_view key) const {
        const Value* value = find(key);
        if (value == nullptr) throw std::out_of_range("no member " + std::string(key));
        return *value;
    }

    const Value& operator[](std::size_t i) const { return items.at(i); }

    std::size_t size() const { return kind == Kind::ARRAY ? items.size() : members.size(); }

    std::int64_t integer() const {
        if (kind != Kind::NUMBER) throw std::logic_error("not a number");
        errno = 0;
        char* end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (errno != 0 || end == nullptr || *end != '\0') throw std::logic_error("not a whole number: " + text);
        return value;
    }

    const std::string& str() const {
        if (kind != Kind::STRING) throw std::logic_error("not a string");
        return text;
    }

    bool isTrue() const {
        if (kind != Kind::BOOL) throw std::logic_error("not a boolean");
        return boolean;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Value> parse() {
        std::optional<Value> value = readValue(0);
        if (!value) return std::nullopt;
        skipSpace();
        if (at_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    static constexpr int MAX_DEPTH = 64;

    void skipSpace() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) ++at_;
    }

    bool take(char c) {
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    bool takeWord(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }

    std::optional<Value> readValue(int depth) {
        if (depth > MAX_DEPTH) return std::nullopt;
        skipSpace();
        if (at_ >= text_.size()) return std::nullopt;
        Value value;
        const char c = text_[at_];
        if (c == '{') return readObject(depth);
        if (c == '[') return readArray(depth);
        if (c == '"') {
            value.kind = Value::Kind::STRING;
            if (!readString(value.text)) return std::nullopt;
            return value;
        }
        if (takeWord("true")) {
            value.kind = Value::Kind::BOOL;
            value.boolean = true;
            return value;
        }
        if (takeWord("false")) {
            value.kind = Value::Kind::BOOL;
            return value;
        }
        if (takeWord("null")) return value;
        if (c == '-' || (c >= '0' && c <= '9')) {
            value.kind = Value::Kind::NUMBER;
            if (!readNumber(value.text)) return std::nullopt;
            return value;
        }
        return std::nullopt;
    }

    std::optional<Value> readObject(int depth) {
        Value value;
        value.kind = Value::Kind::OBJECT;
        take('{');
        skipSpace();
        if (take('}')) return value;
        for (;;) {
            skipSpace();
            std::string key;
            if (at_ >= text_.size() || text_[at_] != '"' || !readString(key)) return std::nullopt;
            skipSpace();
            if (!take(':')) return std::nullopt;
            std::optional<Value> member = readValue(depth + 1);
            if (!member) return std::nullopt;
            value.members.emplace_back(std::move(key), std::move(*member));
            skipSpace();
            if (take(',')) continue;
            if (take('}')) return value;
            return std::nullopt;
        }
    }

    std::optional<Value> readArray(int depth) {
        Value value;
        value.kind = Value::Kind::ARRAY;
        take('[');
        skipSpace();
        if (take(']')) return value;
        for (;;) {
            std::optional<Value> item = readValue(depth + 1);
            if (!item) return std::nullopt;
            value.items.push_back(std::move(*item));
            skipSpace();
            if (take(',')) continue;
            if (take(']')) return value;
            return std::nullopt;
        }
    }

    bool readNumber(std::string& out) {
        const std::size_t start = at_;
        take('-');
        if (take('0')) {
            // no digit may follow a leading zero
        } else if (at_ < text_.size() && text_[at_] >= '1' && text_[at_] <= '9') {
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
        } else {
            return false;
        }
        if (take('.')) {
            if (!digits()) return false;
        }
        if (take('e') || take('E')) {
            if (!take('+')) take('-');
            if (!digits()) return false;
        }
        out = std::string(text_.substr(start, at_ - start));
        return true;
    }

    bool digits() {
        const std::size_t start = at_;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
        return at_ > start;
    }

    // The characters of a string, with \u escapes below 0x100 only: the writer escapes bytes and nothing wider.
    bool readString(std::string& out) {
        if (!take('"')) return false;
        out.clear();
        while (at_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[at_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (at_ >= text_.size()) return false;
            const char e = text_[at_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (at_ + 4 > text_.size()) return false;
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text_[at_++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                if (code >= 0x100) return false;
                out += static_cast<char>(code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    std::string_view text_;
    std::size_t at_{ 0 };
};

inline std::optional<Value> parse(std::string_view text) { return Parser(text).parse(); }

} // namespace minijson
