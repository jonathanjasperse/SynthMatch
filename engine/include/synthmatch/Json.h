#pragma once

// A small JSON reader and writer, enough for preset files and for the edits the
// assistant produces. No dependencies.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synthmatch {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>; // keeps insertion order

    Json() = default;
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(int value) : type_(Type::Number), number_(value) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Json(std::string_view value) : type_(Type::String), string_(value) {}
    Json(const char* value) : type_(Type::String), string_(value) {}

    static Json array() { return Json(Type::Array); }
    static Json object() { return Json(Type::Object); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool boolean() const { return bool_; }
    double number() const { return number_; }
    const std::string& string() const { return string_; }
    const Array& items() const { return array_; }
    const Object& members() const { return object_; }

    // Object access. find() returns nullptr when the key is missing.
    const Json* find(std::string_view key) const;
    Json& set(std::string key, Json value); // replaces an existing key
    // Array access.
    void push(Json value) { array_.push_back(std::move(value)); }

    // Accepts standard JSON, plus trailing commas (small language models
    // sometimes write them).
    static std::optional<Json> parse(std::string_view text, std::string* error = nullptr);

    // indent > 0 pretty-prints; arrays of plain numbers stay on one line.
    std::string dump(int indent = 2) const;

private:
    explicit Json(Type type) : type_(type) {}
    void write(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

// Formats a number the way presets show them: up to 6 significant digits, no
// trailing zeros ("0.3", "350", "-12.5").
std::string formatNumber(double value);

} // namespace synthmatch
