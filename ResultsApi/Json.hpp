#pragma once
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace api {

// Writes one JSON text. It puts the commas in, and the caller keeps to the grammar: a key, then its value,
// inside an object; values alone inside an array.
class JsonWriter {
public:
    JsonWriter& beginObject() { return open('{'); }
    JsonWriter& endObject() { return close('}'); }
    JsonWriter& beginArray() { return open('['); }
    JsonWriter& endArray() { return close(']'); }

    JsonWriter& key(std::string_view name) {
        separate();
        quote(name);
        out_ += ':';
        afterKey_ = true;
        return *this;
    }

    template <typename T>
    JsonWriter& number(T value) {
        static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
        separate();
        char digits[24];
        const auto end = std::to_chars(digits, digits + sizeof(digits), value).ptr;
        out_.append(digits, end);
        return *this;
    }

    // A number with a fixed count of decimal places, written from the whole number that is the value times
    // 10 to the power of `places`.
    JsonWriter& fixed(std::uint64_t scaled, unsigned places) {
        std::uint64_t unit = 1;
        for (unsigned i = 0; i < places; ++i) unit *= 10;
        separate();
        char digits[24];
        out_.append(digits, std::to_chars(digits, digits + sizeof(digits), scaled / unit).ptr);
        if (places > 0) {
            std::string fraction(places, '0');
            std::uint64_t rest = scaled % unit;
            for (unsigned i = places; i-- > 0;) {
                fraction[i] = static_cast<char>('0' + rest % 10);
                rest /= 10;
            }
            out_ += '.';
            out_ += fraction;
        }
        return *this;
    }

    // A 64-bit id or token as a string: most JSON readers take a number for a double, which holds 53 bits.
    JsonWriter& id(std::uint64_t value) {
        separate();
        out_ += '"';
        char digits[24];
        const auto end = std::to_chars(digits, digits + sizeof(digits), value).ptr;
        out_.append(digits, end);
        out_ += '"';
        return *this;
    }

    JsonWriter& string(std::string_view text) {
        separate();
        quote(text);
        return *this;
    }

    JsonWriter& boolean(bool value) {
        separate();
        out_ += value ? "true" : "false";
        return *this;
    }

    JsonWriter& null() {
        separate();
        out_ += "null";
        return *this;
    }

    const std::string& text() const { return out_; }
    std::string take() { return std::move(out_); }

private:
    void separate() {
        if (afterKey_) {
            afterKey_ = false;
        } else if (!empty_.empty()) {
            if (empty_.back()) empty_.back() = false;
            else out_ += ',';
        }
    }

    JsonWriter& open(char bracket) {
        separate();
        out_ += bracket;
        empty_.push_back(true);
        return *this;
    }

    JsonWriter& close(char bracket) {
        out_ += bracket;
        empty_.pop_back();
        return *this;
    }

    // Every byte outside printable ASCII is escaped, as the character with that code, so the text is valid JSON
    // whatever it is given.
    void quote(std::string_view text) {
        out_ += '"';
        for (const unsigned char c : text) {
            switch (c) {
            case '"': out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\t': out_ += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char escape[8];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(c));
                    out_ += escape;
                } else {
                    out_ += static_cast<char>(c);
                }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> empty_; // for each open bracket, whether nothing has been written in it yet
    bool afterKey_{ false };
};

} // namespace api
