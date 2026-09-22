#include "showmesh/brightness_query.h"

#include <cmath>
#include <utility>
#include <vector>

#include "showmesh/json.h"

namespace showmesh {

const char* const kBrightnessQueryPath = "/showmesh/brightness";
const char* const kBrightnessQueryLanPath = "/api/plugin-apis/showmesh/brightness";

BrightnessQueryResponse renderBrightnessQuery(const BrightnessEngine& engine, TimeMillis now) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kBrightnessQuerySchemaVersion));
    members.emplace_back("ceiling", json::Value::makeNumber(std::lround(engine.ceilingAt(now))));
    members.emplace_back("transitionGain", json::Value::makeNumber(std::lround(engine.gainAt(now))));
    members.emplace_back("effectiveOutput", json::Value::makeNumber(engine.effectivePercentAt(now)));
    members.emplace_back("fadeActive", json::Value::makeBool(engine.fadingAt(now)));
    members.emplace_back("updatedAtMillis", json::Value::makeNumber(static_cast<double>(now)));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!rendered.ok) {
        return BrightnessQueryResponse{200,
                                       R"({"schemaVersion":1,"ceiling":0,"transitionGain":0,)"
                                       R"("effectiveOutput":0,"fadeActive":false,"updatedAtMillis":0})"};
    }
    return BrightnessQueryResponse{200, rendered.text};
}

}  // namespace showmesh
