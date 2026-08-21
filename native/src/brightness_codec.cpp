#include "showmesh/brightness_codec.h"

#include <cmath>
#include <vector>

#include "showmesh/json.h"

namespace showmesh {
namespace {

void addNumber(std::vector<json::Value::Member>* members, const char* name, double value) {
    members->emplace_back(name, json::Value::makeNumber(value));
}

const json::Value* member(const json::Value& object, const char* name) {
    for (const json::Value::Member& m : object.members()) {
        if (m.first == name) return &m.second;
    }
    return nullptr;
}

bool readNumber(const json::Value& object, const char* name, double* out, std::string* error) {
    const json::Value* v = member(object, name);
    if (v == nullptr || v->type() != json::Type::kNumber) {
        *error = std::string("missing or non-numeric field \"") + name + "\"";
        return false;
    }
    *out = v->number();
    return true;
}

bool readMillis(const json::Value& object, const char* name, TimeMillis* out, std::string* error) {
    double d = 0.0;
    if (!readNumber(object, name, &d, error)) return false;
    if (!std::isfinite(d)) {
        *error = std::string("field \"") + name + "\" is not a finite time";
        return false;
    }
    *out = static_cast<TimeMillis>(d);
    return true;
}

}  // namespace

std::string encodeBrightnessState(const BrightnessState& state) {
    std::vector<json::Value::Member> members;
    addNumber(&members, "schemaVersion", static_cast<double>(state.schemaVersion));
    addNumber(&members, "revision", static_cast<double>(state.revision));
    addNumber(&members, "ceilingStart", state.ceilingStart);
    addNumber(&members, "ceilingTarget", state.ceilingTarget);
    addNumber(&members, "ceilingFadeStartMillis", static_cast<double>(state.ceilingFadeStartMillis));
    addNumber(&members, "ceilingFadeEndMillis", static_cast<double>(state.ceilingFadeEndMillis));
    addNumber(&members, "gainStart", state.gainStart);
    addNumber(&members, "gainTarget", state.gainTarget);
    addNumber(&members, "gainFadeStartMillis", static_cast<double>(state.gainFadeStartMillis));
    addNumber(&members, "gainFadeEndMillis", static_cast<double>(state.gainFadeEndMillis));
    addNumber(&members, "lastAppliedCeiling", state.lastAppliedCeiling);
    addNumber(&members, "lastAppliedGain", state.lastAppliedGain);
    addNumber(&members, "persistedAtMillis", static_cast<double>(state.persistedAtMillis));

    json::CanonicalResult canonical = json::canonicalize(json::Value::makeObject(std::move(members)));
    return canonical.ok ? canonical.text : std::string();
}

BrightnessStateDecode decodeBrightnessState(const std::string& text) {
    BrightnessStateDecode result;
    json::ParseResult parsed = json::parse(text);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }
    if (parsed.value.type() != json::Type::kObject) {
        result.error = "the persisted brightness state is not a JSON object";
        return result;
    }

    BrightnessState s;
    double schemaVersion = 0.0;
    double revision = 0.0;
    if (!readNumber(parsed.value, "schemaVersion", &schemaVersion, &result.error)) return result;
    if (!readNumber(parsed.value, "revision", &revision, &result.error)) return result;
    if (!readNumber(parsed.value, "ceilingStart", &s.ceilingStart, &result.error)) return result;
    if (!readNumber(parsed.value, "ceilingTarget", &s.ceilingTarget, &result.error)) return result;
    if (!readMillis(parsed.value, "ceilingFadeStartMillis", &s.ceilingFadeStartMillis, &result.error)) return result;
    if (!readMillis(parsed.value, "ceilingFadeEndMillis", &s.ceilingFadeEndMillis, &result.error)) return result;
    if (!readNumber(parsed.value, "gainStart", &s.gainStart, &result.error)) return result;
    if (!readNumber(parsed.value, "gainTarget", &s.gainTarget, &result.error)) return result;
    if (!readMillis(parsed.value, "gainFadeStartMillis", &s.gainFadeStartMillis, &result.error)) return result;
    if (!readMillis(parsed.value, "gainFadeEndMillis", &s.gainFadeEndMillis, &result.error)) return result;
    if (!readNumber(parsed.value, "lastAppliedCeiling", &s.lastAppliedCeiling, &result.error)) return result;
    if (!readNumber(parsed.value, "lastAppliedGain", &s.lastAppliedGain, &result.error)) return result;
    if (!readMillis(parsed.value, "persistedAtMillis", &s.persistedAtMillis, &result.error)) return result;

    s.schemaVersion = static_cast<int>(schemaVersion);
    s.revision = static_cast<std::uint64_t>(revision);

    result.ok = true;
    result.state = s;
    return result;
}

}  // namespace showmesh
