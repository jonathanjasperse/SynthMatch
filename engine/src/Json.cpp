#include "synthmatch/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace synthmatch {

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Json> parseDocument(std::string* error)
    {
        Json value;
        if (parseValue(value, 0)) {
            skipSpace();
            if (pos_ != text_.size())
                fail("unexpected text after the end of the document");
        }
        if (!error_.empty()) {
            if (error)
                *error = error_ + " (at character " + std::to_string(errorPos_) + ")";
            return std::nullopt;
        }
        return value;
    }

private:
    static constexpr int kMaxDepth = 64;

    bool fail(const char* message)
    {
        if (error_.empty()) {
            error_ = message;
            errorPos_ = pos_;
        }
        return false;
    }

    void skipSpace()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool consume(char c)
    {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool literal(std::string_view word)
    {
        if (text_.substr(pos_, word.size()) == word) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    bool parseValue(Json& out, int depth)
    {
        if (depth > kMaxDepth)
            return fail("nested too deeply");
        skipSpace();
        if (pos_ >= text_.size())
            return fail("unexpected end of text");

        const char c = text_[pos_];
        if (c == '{')
            return parseObject(out, depth);
        if (c == '[')
            return parseArray(out, depth);
        if (c == '"') {
            std::string s;
            if (!parseString(s))
                return false;
            out = Json(std::move(s));
            return true;
        }
        if (literal("true")) {
            out = Json(true);
            return true;
        }
        if (literal("false")) {
            out = Json(false);
            return true;
        }
        if (literal("null")) {
            out = Json();
            return true;
        }
        return parseNumber(out);
    }

    bool parseObject(Json& out, int depth)
    {
        ++pos_; // {
        out = Json::object();
        if (consume('}'))
            return true;
        while (true) {
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                return fail("expected a string key");
            std::string key;
            if (!parseString(key))
                return false;
            if (!consume(':'))
                return fail("expected ':' after a key");
            Json value;
            if (!parseValue(value, depth + 1))
                return false;
            out.set(std::move(key), std::move(value));
            if (consume(',')) {
                if (consume('}')) // trailing comma
                    return true;
                continue;
            }
            if (consume('}'))
                return true;
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(Json& out, int depth)
    {
        ++pos_; // [
        out = Json::array();
        if (consume(']'))
            return true;
        while (true) {
            Json value;
            if (!parseValue(value, depth + 1))
                return false;
            out.push(std::move(value));
            if (consume(',')) {
                if (consume(']')) // trailing comma
                    return true;
                continue;
            }
            if (consume(']'))
                return true;
            return fail("expected ',' or ']'");
        }
    }

    static void appendUtf8(std::string& s, unsigned code)
    {
        if (code < 0x80) {
            s += static_cast<char>(code);
        } else if (code < 0x800) {
            s += static_cast<char>(0xC0 | (code >> 6));
            s += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            s += static_cast<char>(0xE0 | (code >> 12));
            s += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (code >> 18));
            s += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool parseHex4(unsigned& code)
    {
        if (pos_ + 4 > text_.size())
            return fail("bad \\u escape");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9')
                code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f')
                code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                code |= static_cast<unsigned>(h - 'A' + 10);
            else
                return fail("bad \\u escape");
        }
        return true;
    }

    bool parseString(std::string& out)
    {
        ++pos_; // opening quote
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size())
                break;
            const char e = text_[pos_++];
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
                unsigned code = 0;
                if (!parseHex4(code))
                    return false;
                // A surrogate pair encodes one character above U+FFFF.
                if (code >= 0xD800 && code <= 0xDBFF && text_.substr(pos_, 2) == "\\u") {
                    pos_ += 2;
                    unsigned low = 0;
                    if (!parseHex4(low))
                        return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, code);
                break;
            }
            default:
                return fail("bad escape in a string");
            }
        }
        return fail("unterminated string");
    }

    bool parseNumber(Json& out)
    {
        const std::size_t start = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
                ++pos_;
            else
                break;
        }
        if (pos_ == start)
            return fail("unexpected character");
        const std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || !std::isfinite(value)) {
            pos_ = start;
            return fail("bad number");
        }
        out = Json(value);
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
    std::size_t errorPos_ = 0;
};

void writeString(std::string& out, const std::string& s)
{
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

} // namespace

std::string formatNumber(double value)
{
    if (!std::isfinite(value))
        return "0";
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.6g", value);
    std::string s = buffer;
    if (s == "-0")
        s = "0";
    return s;
}

const Json* Json::find(std::string_view key) const
{
    for (const auto& [k, v] : object_)
        if (k == key)
            return &v;
    return nullptr;
}

Json& Json::set(std::string key, Json value)
{
    for (auto& [k, v] : object_) {
        if (k == key) {
            v = std::move(value);
            return v;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
    return object_.back().second;
}

std::optional<Json> Json::parse(std::string_view text, std::string* error)
{
    return Parser(text).parseDocument(error);
}

std::string Json::dump(int indent) const
{
    std::string out;
    write(out, indent, 0);
    return out;
}

void Json::write(std::string& out, int indent, int depth) const
{
    const auto newline = [&](int d) {
        if (indent > 0) {
            out += '\n';
            out.append(static_cast<std::size_t>(indent * d), ' ');
        }
    };

    switch (type_) {
    case Type::Null:
        out += "null";
        break;
    case Type::Bool:
        out += bool_ ? "true" : "false";
        break;
    case Type::Number:
        out += formatNumber(number_);
        break;
    case Type::String:
        writeString(out, string_);
        break;
    case Type::Array: {
        bool plain = true; // arrays of numbers and strings stay on one line
        for (const auto& item : array_)
            plain = plain && !item.isArray() && !item.isObject();
        out += '[';
        for (std::size_t i = 0; i < array_.size(); ++i) {
            if (i > 0)
                out += indent > 0 && plain ? ", " : ",";
            if (!plain)
                newline(depth + 1);
            array_[i].write(out, indent, depth + 1);
        }
        if (!plain && !array_.empty())
            newline(depth);
        out += ']';
        break;
    }
    case Type::Object:
        out += '{';
        for (std::size_t i = 0; i < object_.size(); ++i) {
            if (i > 0)
                out += ',';
            newline(depth + 1);
            writeString(out, object_[i].first);
            out += indent > 0 ? ": " : ":";
            object_[i].second.write(out, indent, depth + 1);
        }
        if (!object_.empty())
            newline(depth);
        out += '}';
        break;
    }
}

} // namespace synthmatch
