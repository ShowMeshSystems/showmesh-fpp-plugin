#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace showmesh {
namespace json {

enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

// Value is a minimal JSON tree. It exists so this component can canonicalize
// a playlist definition without linking a JSON library into a source bundle
// that is compiled on an FPP host with whatever toolchain that host has.
class Value {
 public:
    using Member = std::pair<std::string, Value>;

    Value() = default;
    static Value makeNull() { return Value(); }
    static Value makeBool(bool b);
    static Value makeNumber(double d);
    static Value makeString(std::string s);
    static Value makeArray(std::vector<Value> items);
    static Value makeObject(std::vector<Member> members);

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::kNull; }
    bool boolean() const { return bool_; }
    double number() const { return number_; }
    const std::string& string() const { return string_; }
    const std::vector<Value>& items() const { return items_; }
    const std::vector<Member>& members() const { return members_; }

 private:
    Type type_ = Type::kNull;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> items_;
    std::vector<Member> members_;
};

struct ParseResult {
    bool ok = false;
    Value value;
    std::string error;
};

// Parses RFC 8259 JSON. Duplicate object member names are rejected: JCS
// gives no canonical answer for which one survives, and a definition that
// hashes differently depending on that choice is not an identity.
ParseResult parse(const std::string& text);

struct CanonicalResult {
    bool ok = false;
    std::string text;
    std::string error;
};

// Serializes to the RFC 8785 JSON Canonicalization Scheme: object members
// sorted by the UTF-16 code units of their names, no insignificant
// whitespace, ECMAScript number formatting, and minimal string escaping.
CanonicalResult canonicalize(const Value& value);

// Convenience over parse + canonicalize.
CanonicalResult canonicalize(const std::string& text);

// Formats one double exactly as ECMAScript's Number::toString would, which
// is what JCS requires of every JSON number. Reports failure for a value
// JSON cannot represent at all (NaN, infinities).
bool formatNumber(double value, std::string* out);

}  // namespace json
}  // namespace showmesh
