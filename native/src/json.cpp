#include "showmesh/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "showmesh/locale_guard.h"

namespace showmesh {
namespace json {
namespace {

// UTF-16 code-unit ordering of two UTF-8 strings, which is what JCS
// specifies for member name sorting. Plain byte order disagrees with it for
// exactly one class of input: a supplementary character (U+10000 and above)
// sorts after U+E000..U+FFFF in UTF-8 byte order but before it in UTF-16,
// because its surrogates start at 0xD800. Names are therefore compared as
// decoded UTF-16 code units rather than as bytes.
std::vector<std::uint16_t> toUtf16(const std::string& s) {
    std::vector<std::uint16_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        std::size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            len = 4;
        } else {
            // Not valid UTF-8 lead byte; treat as a single code unit so
            // ordering stays total rather than throwing here.
            out.push_back(c);
            ++i;
            continue;
        }
        if (i + len > s.size()) {
            out.push_back(c);
            ++i;
            continue;
        }
        for (std::size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        }
        i += len;
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            out.push_back(static_cast<std::uint16_t>(0xD800u + (cp >> 10)));
            out.push_back(static_cast<std::uint16_t>(0xDC00u + (cp & 0x3FFu)));
        } else {
            out.push_back(static_cast<std::uint16_t>(cp));
        }
    }
    return out;
}

bool lessUtf16(const std::string& a, const std::string& b) {
    return toUtf16(a) < toUtf16(b);
}

void appendEscaped(const std::string& s, std::string* out) {
    out->push_back('"');
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\b': out->append("\\b"); break;
            case '\f': out->append("\\f"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out->append(buf);
                } else {
                    out->push_back(static_cast<char>(c));
                }
        }
    }
    out->push_back('"');
}

class Parser {
 public:
    explicit Parser(const std::string& text) : text_(text) {}

    ParseResult run() {
        // strtod, called by parseNumber below, honors LC_NUMERIC; without
        // this guard a comma-decimal locale set elsewhere in the process
        // would silently misparse every fractional number in this text.
        const CLocaleGuard localeGuard;
        if (!localeGuard.ok()) return failAt("could not establish the C numeric locale");
        skipWhitespace();
        Value v;
        if (!parseValue(&v)) return fail();
        skipWhitespace();
        if (pos_ != text_.size()) return failAt("trailing content after the JSON value");
        ParseResult r;
        r.ok = true;
        r.value = std::move(v);
        return r;
    }

 private:
    ParseResult fail() {
        ParseResult r;
        r.ok = false;
        r.error = error_.empty() ? "invalid JSON" : error_;
        return r;
    }
    ParseResult failAt(const char* message) {
        error_ = std::string(message) + " at offset " + std::to_string(pos_);
        return fail();
    }
    bool err(const char* message) {
        if (error_.empty()) error_ = std::string(message) + " at offset " + std::to_string(pos_);
        return false;
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(const char* word) {
        const std::size_t n = std::strlen(word);
        if (text_.compare(pos_, n, word) != 0) return err("unrecognized literal");
        pos_ += n;
        return true;
    }

    bool parseValue(Value* out) {
        if (depth_ > kMaxDepth) return err("JSON nesting is too deep");
        if (pos_ >= text_.size()) return err("unexpected end of input");
        switch (text_[pos_]) {
            case 'n':
                if (!literal("null")) return false;
                *out = Value::makeNull();
                return true;
            case 't':
                if (!literal("true")) return false;
                *out = Value::makeBool(true);
                return true;
            case 'f':
                if (!literal("false")) return false;
                *out = Value::makeBool(false);
                return true;
            case '"': {
                std::string s;
                if (!parseString(&s)) return false;
                *out = Value::makeString(std::move(s));
                return true;
            }
            case '[': return parseArray(out);
            case '{': return parseObject(out);
            default: return parseNumber(out);
        }
    }

    bool parseArray(Value* out) {
        ++pos_;  // '['
        ++depth_;
        std::vector<Value> items;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            --depth_;
            *out = Value::makeArray(std::move(items));
            return true;
        }
        while (true) {
            skipWhitespace();
            Value v;
            if (!parseValue(&v)) return false;
            items.push_back(std::move(v));
            skipWhitespace();
            if (pos_ >= text_.size()) return err("unterminated array");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                --depth_;
                *out = Value::makeArray(std::move(items));
                return true;
            }
            return err("expected ',' or ']'");
        }
    }

    bool parseObject(Value* out) {
        ++pos_;  // '{'
        ++depth_;
        std::vector<Value::Member> members;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            --depth_;
            *out = Value::makeObject(std::move(members));
            return true;
        }
        while (true) {
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') return err("expected a member name");
            std::string name;
            if (!parseString(&name)) return false;
            for (const Value::Member& m : members) {
                if (m.first == name) return err("duplicate object member name");
            }
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return err("expected ':'");
            ++pos_;
            skipWhitespace();
            Value v;
            if (!parseValue(&v)) return false;
            members.emplace_back(std::move(name), std::move(v));
            skipWhitespace();
            if (pos_ >= text_.size()) return err("unterminated object");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                --depth_;
                *out = Value::makeObject(std::move(members));
                return true;
            }
            return err("expected ',' or '}'");
        }
    }

    static void appendCodePoint(std::uint32_t cp, std::string* out) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(std::uint32_t* out) {
        if (pos_ + 4 > text_.size()) return err("truncated \\u escape");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return err("invalid \\u escape");
            }
        }
        pos_ += 4;
        *out = v;
        return true;
    }

    bool parseString(std::string* out) {
        ++pos_;  // opening quote
        out->clear();
        while (true) {
            if (pos_ >= text_.size()) return err("unterminated string");
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return err("unescaped control character in a string");
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size()) return err("unterminated escape");
            const char e = text_[pos_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parseHex4(&cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            std::uint32_t low = 0;
                            if (!parseHex4(&low)) return false;
                            if (low < 0xDC00 || low > 0xDFFF) return err("invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            return err("unpaired high surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return err("unpaired low surrogate");
                    }
                    appendCodePoint(cp, out);
                    break;
                }
                default: return err("unrecognized escape");
            }
        }
    }

    bool parseNumber(Value* out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size()) return err("truncated number");
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        } else {
            return err("expected a JSON value");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') return err("truncated fraction");
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') return err("truncated exponent");
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        const std::string literalText = text_.substr(start, pos_ - start);
        const double d = std::strtod(literalText.c_str(), nullptr);
        if (!std::isfinite(d)) return err("number is out of the range JSON can carry");
        *out = Value::makeNumber(d);
        return true;
    }

    static constexpr int kMaxDepth = 200;

    const std::string& text_;
    std::size_t pos_ = 0;
    int depth_ = 0;
    std::string error_;
};

bool writeValue(const Value& v, std::string* out, std::string* error) {
    switch (v.type()) {
        case Type::kNull:
            out->append("null");
            return true;
        case Type::kBool:
            out->append(v.boolean() ? "true" : "false");
            return true;
        case Type::kNumber: {
            std::string formatted;
            if (!formatNumber(v.number(), &formatted)) {
                *error = "a number could not be canonicalized";
                return false;
            }
            out->append(formatted);
            return true;
        }
        case Type::kString:
            appendEscaped(v.string(), out);
            return true;
        case Type::kArray: {
            out->push_back('[');
            bool first = true;
            for (const Value& item : v.items()) {
                if (!first) out->push_back(',');
                first = false;
                if (!writeValue(item, out, error)) return false;
            }
            out->push_back(']');
            return true;
        }
        case Type::kObject: {
            std::vector<const Value::Member*> sorted;
            sorted.reserve(v.members().size());
            for (const Value::Member& m : v.members()) sorted.push_back(&m);
            std::sort(sorted.begin(), sorted.end(),
                      [](const Value::Member* a, const Value::Member* b) { return lessUtf16(a->first, b->first); });
            out->push_back('{');
            bool first = true;
            for (const Value::Member* m : sorted) {
                if (!first) out->push_back(',');
                first = false;
                appendEscaped(m->first, out);
                out->push_back(':');
                if (!writeValue(m->second, out, error)) return false;
            }
            out->push_back('}');
            return true;
        }
    }
    *error = "unknown JSON value type";
    return false;
}

}  // namespace

Value Value::makeBool(bool b) {
    Value v;
    v.type_ = Type::kBool;
    v.bool_ = b;
    return v;
}
Value Value::makeNumber(double d) {
    Value v;
    v.type_ = Type::kNumber;
    v.number_ = d;
    return v;
}
Value Value::makeString(std::string s) {
    Value v;
    v.type_ = Type::kString;
    v.string_ = std::move(s);
    return v;
}
Value Value::makeArray(std::vector<Value> items) {
    Value v;
    v.type_ = Type::kArray;
    v.items_ = std::move(items);
    return v;
}
Value Value::makeObject(std::vector<Member> members) {
    Value v;
    v.type_ = Type::kObject;
    v.members_ = std::move(members);
    return v;
}

// ECMAScript Number::toString, which JCS adopts wholesale. The shortest
// decimal form that round-trips is found first, then placed by the
// exponent rules: plain digits within 10^-6 up to 10^21, exponential
// outside that window.
bool formatNumber(double value, std::string* out) {
    // snprintf's "%e" and the strtod round-trip below both honor
    // LC_NUMERIC; without this guard a comma-decimal locale would emit
    // "1,5" instead of "1.5", which is not valid JSON.
    const CLocaleGuard localeGuard;
    if (!localeGuard.ok()) return false;
    if (!std::isfinite(value)) return false;
    if (value == 0.0) {
        *out = "0";  // negative zero is also "0"
        return true;
    }

    std::string sign;
    double v = value;
    if (v < 0) {
        sign = "-";
        v = -v;
    }

    char buf[64];
    int precision = 1;
    for (; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*e", precision - 1, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    if (precision > 17) std::snprintf(buf, sizeof(buf), "%.16e", v);

    // buf is "d[.ddd]e±XX": split it into the digit string and the decimal
    // exponent n, where the value is 0.<digits> * 10^n.
    std::string text(buf);
    const std::size_t epos = text.find('e');
    std::string mantissa = text.substr(0, epos);
    const int exponent = std::atoi(text.c_str() + epos + 1);

    std::string digits;
    for (char c : mantissa) {
        if (c != '.') digits.push_back(c);
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    const int k = static_cast<int>(digits.size());
    const int n = exponent + 1;

    std::string body;
    if (k <= n && n <= 21) {
        body = digits + std::string(static_cast<std::size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        body = digits.substr(0, static_cast<std::size_t>(n)) + "." + digits.substr(static_cast<std::size_t>(n));
    } else if (-6 < n && n <= 0) {
        body = "0." + std::string(static_cast<std::size_t>(-n), '0') + digits;
    } else {
        const int e = n - 1;
        const std::string expPart = (e >= 0 ? "e+" : "e-") + std::to_string(e >= 0 ? e : -e);
        if (k == 1) {
            body = digits + expPart;
        } else {
            body = digits.substr(0, 1) + "." + digits.substr(1) + expPart;
        }
    }

    *out = sign + body;
    return true;
}

ParseResult parse(const std::string& text) {
    Parser p(text);
    return p.run();
}

CanonicalResult canonicalize(const Value& value) {
    CanonicalResult r;
    std::string error;
    std::string text;
    if (!writeValue(value, &text, &error)) {
        r.ok = false;
        r.error = error;
        return r;
    }
    r.ok = true;
    r.text = std::move(text);
    return r;
}

CanonicalResult canonicalize(const std::string& text) {
    ParseResult parsed = parse(text);
    if (!parsed.ok) {
        CanonicalResult r;
        r.ok = false;
        r.error = parsed.error;
        return r;
    }
    return canonicalize(parsed.value);
}

}  // namespace json
}  // namespace showmesh
