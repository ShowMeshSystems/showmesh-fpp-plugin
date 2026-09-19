#include "showmesh/weather_gate.h"

#include <cmath>
#include <utility>
#include <vector>

#include "showmesh/json.h"

namespace showmesh {

const char* const kWeatherGatePath = "/showmesh/brightness/weather-gate";
const char* const kWeatherGateLanPath = "/api/plugin-apis/showmesh/brightness/weather-gate";

namespace {

std::string renderProblem(const std::string& detail) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kWeatherGateSchemaVersion));
    members.emplace_back("applied", json::Value::makeBool(false));
    members.emplace_back("error", json::Value::makeString(detail));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!rendered.ok) return R"({"applied":false,"error":"the refusal could not be rendered","schemaVersion":1})";
    return rendered.text;
}

WeatherGateResponse refuse(const std::string& detail) { return WeatherGateResponse{400, renderProblem(detail)}; }

// The same full state document the transition-gain route renders (gain
// start/target, the fade duration, the ceiling, and the composed effective
// output), extended with weatherGateClosed and effectiveOutputPercent. A
// weather-gate write never starts a gain fade, so gainStart and gainTarget
// both read the gain as it stands, and fadeSeconds is always 0.
WeatherGateResponse renderDocument(bool applied, const BrightnessEngine& engine, TimeMillis now) {
    const int gainNow = static_cast<int>(std::lround(engine.gainAt(now)));
    const int ceilingNow = static_cast<int>(std::lround(engine.ceilingAt(now)));
    const int effective = engine.effectivePercentAt(now);

    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kWeatherGateSchemaVersion));
    members.emplace_back("applied", json::Value::makeBool(applied));
    members.emplace_back("gainStart", json::Value::makeNumber(gainNow));
    members.emplace_back("gainTarget", json::Value::makeNumber(gainNow));
    members.emplace_back("fadeSeconds", json::Value::makeNumber(0));
    members.emplace_back("ceiling", json::Value::makeNumber(ceilingNow));
    members.emplace_back("effectiveOutput", json::Value::makeNumber(effective));
    members.emplace_back("weatherGateClosed", json::Value::makeBool(engine.weatherGateClosed()));
    members.emplace_back("effectiveOutputPercent", json::Value::makeNumber(effective));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!rendered.ok) return refuse("the state could not be rendered: " + rendered.error);
    return WeatherGateResponse{200, rendered.text};
}

const json::Value* memberOf(const json::Value& object, const char* name) {
    for (const json::Value::Member& m : object.members()) {
        if (m.first == name) return &m.second;
    }
    return nullptr;
}

}  // namespace

WeatherGateResponse applyWeatherGateRequest(const std::string& body, BrightnessEngine* engine, TimeMillis now) {
    if (engine == nullptr) return refuse("the plugin is not ready to serve this route");
    if (body.size() > kWeatherGateBodyLimitBytes) {
        return refuse("the request body is larger than this route accepts");
    }

    json::ParseResult parsed = json::parse(body);
    if (!parsed.ok) return refuse("the request body is not valid JSON: " + parsed.error);
    if (parsed.value.type() != json::Type::kObject) return refuse("the request body must be a JSON object");

    // Exactly one key, "closed": an extra or misspelled key is refused
    // rather than silently ignored, and a missing "closed" falls into the
    // same count check rather than a separate message.
    if (parsed.value.members().size() != 1) {
        return refuse(R"(the request body must contain exactly the "closed" key)");
    }
    const json::Value* closed = memberOf(parsed.value, "closed");
    if (closed == nullptr) return refuse(R"(unknown key in the request body, expected "closed")");
    if (closed->type() != json::Type::kBool) return refuse(R"("closed" must be a boolean)");

    engine->setWeatherGate(closed->boolean(), now);
    return renderDocument(true, *engine, now);
}

WeatherGateResponse renderWeatherGateState(BrightnessEngine* engine, TimeMillis now) {
    if (engine == nullptr) return refuse("the plugin is not ready to serve this route");
    return renderDocument(false, *engine, now);
}

}  // namespace showmesh
