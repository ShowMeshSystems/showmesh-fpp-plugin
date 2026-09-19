#include "showmesh/brightness_codec.h"

#include <cmath>
#include <vector>

#include "showmesh/json.h"

namespace showmesh {
namespace {

// Bounds a double to what TimeMillis (int64) can represent without
// undefined behavior in the cast. Wider than any plausible epoch value on
// purpose: epoch plausibility for a fade window is BrightnessEngine's own
// concern, this is only about the destination type's range.
constexpr double kInt64SafeMagnitude = 4.0e18;

// A revision or schema version this large cannot be a real count. Bounding
// it here, not just requiring isfinite, is what stops an absurd value like
// 1e19 or 1e300 from being adopted and permanently wedging the node
// against every future payload as newer-than-stale.
constexpr double kMaxPlausibleRevisionValue = 1e15;
constexpr double kMaxPlausibleSchemaVersionValue = 1000.0;

void addNumber(std::vector<json::Value::Member>* members, const char* name, double value) {
    members->emplace_back(name, json::Value::makeNumber(value));
}

void addString(std::vector<json::Value::Member>* members, const char* name, const std::string& value) {
    members->emplace_back(name, json::Value::makeString(value));
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

// The one field this codec treats as optional: a record written before
// the weather gate existed decodes with it defaulted to false ("not
// mentioned"), rather than failing the whole payload the way a missing
// required field does below.
bool readOptionalBool(const json::Value& object, const char* name, bool defaultValue, bool* out) {
    const json::Value* v = member(object, name);
    if (v == nullptr) {
        *out = defaultValue;
        return true;
    }
    if (v->type() != json::Type::kBool) return false;
    *out = v->boolean();
    return true;
}

bool readString(const json::Value& object, const char* name, std::string* out, std::string* error) {
    const json::Value* v = member(object, name);
    if (v == nullptr || v->type() != json::Type::kString) {
        *error = std::string("missing or non-string field \"") + name + "\"";
        return false;
    }
    *out = v->string();
    return true;
}

bool readMillis(const json::Value& object, const char* name, TimeMillis* out, std::string* error) {
    double d = 0.0;
    if (!readNumber(object, name, &d, error)) return false;
    if (!std::isfinite(d) || d < -kInt64SafeMagnitude || d > kInt64SafeMagnitude) {
        *error = std::string("field \"") + name + "\" is not a representable time";
        return false;
    }
    if (d != std::floor(d)) {
        // A fractional millisecond cannot come from a real clock read;
        // truncating it silently (as static_cast would) turns 1.5 into 1
        // without either side noticing the payload was malformed.
        *error = std::string("field \"") + name + "\" is not a whole number of milliseconds";
        return false;
    }
    *out = static_cast<TimeMillis>(d);
    return true;
}

// Reads a non-negative whole number bounded to a sane domain for its
// destination type, rather than merely finite: a representable but absurd
// value (1e19 for a uint64 revision) converts without undefined behavior
// and would otherwise be silently adopted.
bool readBoundedCount(const json::Value& object, const char* name, double maxValue, double* out, std::string* error) {
    double d = 0.0;
    if (!readNumber(object, name, &d, error)) return false;
    if (!std::isfinite(d) || d < 0.0 || d > maxValue || d != std::floor(d)) {
        *error = std::string("field \"") + name + "\" is out of domain";
        return false;
    }
    *out = d;
    return true;
}

}  // namespace

std::string encodeBrightnessState(const BrightnessState& state) {
    std::vector<json::Value::Member> members;
    addNumber(&members, "schemaVersion", static_cast<double>(state.schemaVersion));
    addNumber(&members, "revision", static_cast<double>(state.revision));
    addNumber(&members, "stateChangedAtMillis", static_cast<double>(state.stateChangedAtMillis));
    addString(&members, "instanceId", state.instanceId);
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
    members.emplace_back("weatherGateClosed", json::Value::makeBool(state.weatherGateClosed));
    addNumber(&members, "weatherGateRevision", static_cast<double>(state.weatherGateRevision));

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
    if (!readBoundedCount(parsed.value, "schemaVersion", kMaxPlausibleSchemaVersionValue, &schemaVersion, &result.error))
        return result;
    if (!readBoundedCount(parsed.value, "revision", kMaxPlausibleRevisionValue, &revision, &result.error)) return result;
    if (!readMillis(parsed.value, "stateChangedAtMillis", &s.stateChangedAtMillis, &result.error)) return result;
    if (!readString(parsed.value, "instanceId", &s.instanceId, &result.error)) return result;
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
    if (!readOptionalBool(parsed.value, "weatherGateClosed", false, &s.weatherGateClosed)) {
        result.error = "field \"weatherGateClosed\" must be a boolean";
        return result;
    }
    // Optional like the gate itself: a record or peer without it reads as revision 0.
    if (member(parsed.value, "weatherGateRevision") != nullptr) {
        double gateRevision = 0.0;
        if (!readBoundedCount(parsed.value, "weatherGateRevision", static_cast<double>(kMaxWeatherGateRevision),
                              &gateRevision, &result.error)) {
            return result;
        }
        s.weatherGateRevision = static_cast<std::uint64_t>(gateRevision);
    }

    s.schemaVersion = static_cast<int>(schemaVersion);
    s.revision = static_cast<std::uint64_t>(revision);

    result.ok = true;
    result.state = s;
    return result;
}

}  // namespace showmesh
