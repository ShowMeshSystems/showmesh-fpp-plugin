#include "showmesh/excluded_ranges.h"

#include <cmath>
#include <utility>

namespace showmesh {

const char* const kExcludedRangesPath = "/showmesh/brightness/excluded-ranges";
const char* const kExcludedRangesLanPath = "/api/plugin-apis/showmesh/brightness/excluded-ranges";

namespace {

const char* const kRightForm = R"(; send {"excludedRanges":[{"startChannel":1,"channelCount":512}]})";

ExcludedRangesRequest refused(const std::string& detail) {
    ExcludedRangesRequest request;
    request.refusal = refuseExcludedRanges(detail);
    return request;
}

// One-based and non-zero, and small enough that start + count cannot wrap
// ChannelRange::endExclusive().
bool channelNumber(const json::Value& value, std::uint32_t* out) {
    if (value.type() != json::Type::kNumber) return false;
    const double d = value.number();
    if (!std::isfinite(d) || d != std::floor(d) || d < 1.0 || d > 4294967294.0) return false;
    *out = static_cast<std::uint32_t>(d);
    return true;
}

}  // namespace

json::Value excludedRangesValue(const std::vector<ChannelRange>& ranges) {
    std::vector<json::Value> items;
    for (const ChannelRange& r : ranges) {
        std::vector<json::Value::Member> members;
        members.emplace_back("startChannel", json::Value::makeNumber(static_cast<double>(r.startChannel)));
        members.emplace_back("channelCount", json::Value::makeNumber(static_cast<double>(r.channelCount)));
        items.push_back(json::Value::makeObject(std::move(members)));
    }
    return json::Value::makeArray(std::move(items));
}

ExcludedRangesResponse refuseExcludedRanges(const std::string& detail, int status) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kExcludedRangesSchemaVersion));
    members.emplace_back("applied", json::Value::makeBool(false));
    members.emplace_back("error", json::Value::makeString(status == 400 ? detail + kRightForm : detail));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!rendered.ok) {
        return ExcludedRangesResponse{
            status, R"({"applied":false,"error":"the refusal could not be rendered","schemaVersion":1})"};
    }
    return ExcludedRangesResponse{status, rendered.text};
}

ExcludedRangesResponse renderExcludedRanges(const std::vector<ChannelRange>& ranges) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kExcludedRangesSchemaVersion));
    members.emplace_back("excludedRanges", excludedRangesValue(ranges));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!rendered.ok) return refuseExcludedRanges("the excluded ranges could not be rendered", 500);
    return ExcludedRangesResponse{200, rendered.text};
}

ExcludedRangesRequest parseExcludedRangesRequest(const std::string& body) {
    if (body.size() > kExcludedRangesBodyLimitBytes) return refused("the request body is too large");

    json::ParseResult parsed = json::parse(body);
    if (!parsed.ok || parsed.value.type() != json::Type::kObject) return refused("the body is not a JSON object");

    const json::Value* list = nullptr;
    for (const json::Value::Member& m : parsed.value.members()) {
        if (m.first == "excludedRanges") {
            list = &m.second;
        } else if (m.first != "schemaVersion") {
            return refused("unknown key \"" + m.first + "\"");
        }
    }
    if (list == nullptr || list->type() != json::Type::kArray) return refused("excludedRanges must be a list");

    ExcludedRangesRequest request;
    for (const json::Value& item : list->items()) {
        const std::string where = "excludedRanges[" + std::to_string(request.ranges.size()) + "]";
        if (item.type() != json::Type::kObject || item.members().size() != 2) {
            return refused(where + " must hold exactly startChannel and channelCount");
        }
        ChannelRange range;
        bool haveStart = false;
        bool haveCount = false;
        for (const json::Value::Member& m : item.members()) {
            if (m.first == "startChannel") haveStart = channelNumber(m.second, &range.startChannel);
            if (m.first == "channelCount") haveCount = channelNumber(m.second, &range.channelCount);
        }
        if (!haveStart || !haveCount) return refused(where + " needs whole numbers of 1 or more");
        if (static_cast<double>(range.startChannel) + static_cast<double>(range.channelCount) > 4294967295.0) {
            return refused(where + " runs past the last channel");
        }
        request.ranges.push_back(range);
    }
    request.ok = true;
    return request;
}

}  // namespace showmesh
