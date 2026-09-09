#include "showmesh/definition_republish.h"

#include <cmath>
#include <utility>
#include <vector>

#include "showmesh/json.h"
#include "showmesh/runtime.h"

namespace showmesh {

const char* const kDefinitionRepublishPath = "/showmesh/playlists/republish";
const char* const kDefinitionRepublishLanPath = "/api/plugin-apis/showmesh/playlists/republish";

namespace {

std::string renderProblem(const std::string& detail) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kDefinitionRepublishSchemaVersion));
    members.emplace_back("applied", json::Value::makeBool(false));
    members.emplace_back("error", json::Value::makeString(detail));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    // A refusal whose own body failed to render still has to say something,
    // and this fallback is a literal rather than a second render attempt.
    if (!rendered.ok) return R"({"applied":false,"error":"the refusal could not be rendered","schemaVersion":1})";
    return rendered.text;
}

DefinitionRepublishResponse refuse(const std::string& detail) {
    return DefinitionRepublishResponse{400, renderProblem(detail)};
}

DefinitionRepublishResponse renderApplied(bool applied, const DefinitionHoldings& holdings, bool sweepPending) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kDefinitionRepublishSchemaVersion));
    members.emplace_back("applied", json::Value::makeBool(applied));
    members.emplace_back("definitionsCleared", json::Value::makeNumber(static_cast<double>(holdings.cleared)));
    members.emplace_back("definitionsHeld", json::Value::makeNumber(static_cast<double>(holdings.held)));
    members.emplace_back("definitionsRefusedTerminally",
                         json::Value::makeNumber(static_cast<double>(holdings.refusedTerminally)));
    members.emplace_back("sweepPending", json::Value::makeBool(sweepPending));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!rendered.ok) return refuse("the republish result could not be rendered: " + rendered.error);
    return DefinitionRepublishResponse{200, rendered.text};
}

const json::Value* memberOf(const json::Value& object, const char* name) {
    for (const json::Value::Member& m : object.members()) {
        if (m.first == name) return &m.second;
    }
    return nullptr;
}

// schemaVersion is an integer, not a number that happens to be whole.
// 1.5 and 1e0 are refused rather than rounded, so a mistyped value is
// visible instead of silently accepted.
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

DefinitionRepublishResponse applyDefinitionRepublishRequest(const std::string& body, DefinitionPublisher* publisher,
                                                            SweepRecord* sweep, std::string* lastRequestId) {
    if (publisher == nullptr || sweep == nullptr || lastRequestId == nullptr) {
        return refuse("the plugin is not configured to publish playlist definitions");
    }
    if (body.size() > kDefinitionRepublishBodyLimitBytes) {
        return refuse("the request body is larger than this route accepts");
    }

    json::ParseResult parsed = json::parse(body);
    if (!parsed.ok) return refuse("the request body is not valid JSON: " + parsed.error);
    if (parsed.value.type() != json::Type::kObject) return refuse("the request body must be a JSON object");

    long long schemaVersion = 0;
    std::string error;
    if (!wholeNumberMember(parsed.value, "schemaVersion", &schemaVersion, &error)) return refuse(error);
    if (schemaVersion != kDefinitionRepublishSchemaVersion) {
        return refuse("unsupported schemaVersion " + std::to_string(schemaVersion));
    }

    const json::Value* requestId = memberOf(parsed.value, "requestId");
    if (requestId == nullptr || requestId->type() != json::Type::kString) {
        return refuse("requestId is required and must be a string");
    }
    if (requestId->string().empty()) return refuse("requestId must not be empty");

    // A repeat of the last applied id clears nothing and reports the state
    // as it stands, so polling it is how a caller learns the sweep
    // finished. Checked after validation, not before: a malformed body
    // carrying a seen id is still malformed.
    if (*lastRequestId == requestId->string()) {
        DefinitionHoldings holdings = publisher->definitionHoldings();
        holdings.cleared = 0;
        return renderApplied(false, holdings, sweep->sweepPending());
    }

    // The held set goes; the terminally refused set stays. Clearing both
    // because they sit beside each other would turn one operator action
    // into a retry loop against a condition that cannot improve until the
    // plugin restarts.
    const DefinitionHoldings holdings = publisher->clearHeldDefinitions();
    sweep->requestSweep();
    *lastRequestId = requestId->string();
    // Section 3.9 fixes sweepPending true on an applied answer. Reading
    // the record back here instead would report false in the race where
    // the worker finished the sweep between these two lines.
    return renderApplied(true, holdings, true);
}

}  // namespace showmesh
