#include "showmesh/transition_gain.h"

#include <cmath>
#include <utility>
#include <vector>

#include "showmesh/json.h"

namespace showmesh {

const char* const kTransitionGainPath = "/showmesh/brightness/transition-gain";
const char* const kTransitionGainLanPath = "/api/plugin-apis/showmesh/brightness/transition-gain";

namespace {

std::string renderProblem(const std::string& detail) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kTransitionGainSchemaVersion));
    members.emplace_back("applied", json::Value::makeBool(false));
    members.emplace_back("error", json::Value::makeString(detail));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    // A refusal whose own body failed to render still has to say something,
    // and this fallback is a literal rather than a second render attempt.
    if (!rendered.ok) return R"({"applied":false,"error":"the refusal could not be rendered","schemaVersion":1})";
    return rendered.text;
}

TransitionGainResponse refuse(const std::string& detail) {
    return TransitionGainResponse{400, renderProblem(detail)};
}

// The response section 2.2 specifies: the applied state, so a caller has
// evidence rather than an HTTP 200. gainStart and gainTarget are where the
// fade is going from and to, read after the write, so a repeat reports the
// fade already running rather than restarting it.
TransitionGainResponse renderApplied(bool applied, const BrightnessEngine& engine, int gainStart, int gainTarget,
                                     long long fadeSeconds, TimeMillis now) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kTransitionGainSchemaVersion));
    members.emplace_back("applied", json::Value::makeBool(applied));
    members.emplace_back("gainStart", json::Value::makeNumber(gainStart));
    members.emplace_back("gainTarget", json::Value::makeNumber(gainTarget));
    members.emplace_back("fadeSeconds", json::Value::makeNumber(static_cast<double>(fadeSeconds)));
    members.emplace_back("ceiling", json::Value::makeNumber(std::lround(engine.ceilingAt(now))));
    members.emplace_back("effectiveOutput", json::Value::makeNumber(engine.effectivePercentAt(now)));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!rendered.ok) return refuse("the applied state could not be rendered: " + rendered.error);
    return TransitionGainResponse{200, rendered.text};
}

const json::Value* memberOf(const json::Value& object, const char* name) {
    for (const json::Value::Member& m : object.members()) {
        if (m.first == name) return &m.second;
    }
    return nullptr;
}

// Section 2.2's integers are integers, not "numbers that happen to be
// whole". 75.5 and 1e2 are refused rather than rounded, for the same
// reason the contract refuses an out-of-range percent instead of clamping
// it: a mistyped value must be visible.
bool wholeNumberMember(const json::Value& object, const char* name, long long* out, std::string* error) {
    const json::Value* v = memberOf(object, name);
    if (v == nullptr) {
        *error = std::string(name) + " is required";
        return false;
    }
    if (v->type() != json::Type::kNumber) {
        *error = std::string(name) + " must be a number";
        return false;
    }
    double d = v->number();
    if (d != std::floor(d) || std::isnan(d) || std::isinf(d)) {
        *error = std::string(name) + " must be a whole number";
        return false;
    }
    *out = static_cast<long long>(d);
    return true;
}

}  // namespace

TransitionGainResponse applyTransitionGainRequest(const std::string& body, BrightnessEngine* engine,
                                                  std::string* lastRequestId, TimeMillis now) {
    if (engine == nullptr || lastRequestId == nullptr) return refuse("the plugin is not ready to serve this route");
    if (body.size() > kTransitionGainBodyLimitBytes) {
        return refuse("the request body is larger than this route accepts");
    }

    json::ParseResult parsed = json::parse(body);
    if (!parsed.ok) return refuse("the request body is not valid JSON: " + parsed.error);
    if (parsed.value.type() != json::Type::kObject) return refuse("the request body must be a JSON object");

    long long schemaVersion = 0;
    std::string error;
    if (!wholeNumberMember(parsed.value, "schemaVersion", &schemaVersion, &error)) return refuse(error);
    if (schemaVersion != kTransitionGainSchemaVersion) {
        return refuse("unsupported schemaVersion " + std::to_string(schemaVersion));
    }

    const json::Value* requestId = memberOf(parsed.value, "requestId");
    if (requestId == nullptr || requestId->type() != json::Type::kString) {
        return refuse("requestId is required and must be a string");
    }
    if (requestId->string().empty()) return refuse("requestId must not be empty");

    long long targetPercent = 0;
    long long fadeSeconds = 0;
    if (!wholeNumberMember(parsed.value, "targetPercent", &targetPercent, &error)) return refuse(error);
    if (!wholeNumberMember(parsed.value, "fadeSeconds", &fadeSeconds, &error)) return refuse(error);

    // A repeat of an already-applied id is a no-op that still reports the
    // state, so a caller retrying a response it never saw learns where the
    // fade got to instead of restarting it from wherever it is now.
    // Checked after validation, not before: a malformed body carrying a
    // seen id is still malformed.
    if (*lastRequestId == requestId->string()) {
        const int gainNow = static_cast<int>(std::lround(engine->gainAt(now)));
        return renderApplied(false, *engine, gainNow, gainNow, fadeSeconds, now);
    }

    const int gainStart = static_cast<int>(std::lround(engine->gainAt(now)));
    ValidationResult result = engine->setGain(static_cast<int>(targetPercent), fadeSeconds, now);
    if (!result.ok) return refuse(result.error);

    *lastRequestId = requestId->string();
    return renderApplied(true, *engine, gainStart, static_cast<int>(targetPercent), fadeSeconds, now);
}

}  // namespace showmesh
