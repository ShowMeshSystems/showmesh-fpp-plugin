#include "showmesh/excluded_ranges.h"

#include <cmath>
#include <utility>

namespace showmesh {

const char* const kExcludedRangesPath = "/showmesh/brightness/excluded-ranges";
const char* const kExcludedRangesLanPath = "/api/plugin-apis/showmesh/brightness/excluded-ranges";
const char* const kChannelRangesPath = "/showmesh/brightness/channel-ranges";
const char* const kChannelRangesLanPath = "/api/plugin-apis/showmesh/brightness/channel-ranges";

namespace {

std::string keyOf(RangeList list) { return list == RangeList::kApply ? "channelRanges" : "excludedRanges"; }

std::string rightForm(RangeList list) {
    return "; send {\"" + keyOf(list) + R"(":[{"startChannel":1,"channelCount":512}]})";
}

RangesRequest refused(RangeList list, const std::string& detail) {
    RangesRequest request;
    request.refusal = refuseRanges(list, detail);
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

json::Value rangesValue(const std::vector<ChannelRange>& ranges) {
    std::vector<json::Value> items;
    for (const ChannelRange& r : ranges) {
        std::vector<json::Value::Member> members;
        members.emplace_back("startChannel", json::Value::makeNumber(static_cast<double>(r.startChannel)));
        members.emplace_back("channelCount", json::Value::makeNumber(static_cast<double>(r.channelCount)));
        items.push_back(json::Value::makeObject(std::move(members)));
    }
    return json::Value::makeArray(std::move(items));
}

RangesResponse refuseRanges(RangeList list, const std::string& detail, int status) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kExcludedRangesSchemaVersion));
    members.emplace_back("applied", json::Value::makeBool(false));
    members.emplace_back("error", json::Value::makeString(status == 400 ? detail + rightForm(list) : detail));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!rendered.ok) {
        return RangesResponse{
            status, R"({"applied":false,"error":"the refusal could not be rendered","schemaVersion":1})"};
    }
    return RangesResponse{status, rendered.text};
}

RangesResponse renderRanges(RangeList list, const std::vector<ChannelRange>& ranges) {
    std::vector<json::Value::Member> members;
    members.emplace_back("schemaVersion", json::Value::makeNumber(kExcludedRangesSchemaVersion));
    members.emplace_back(keyOf(list), rangesValue(ranges));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!rendered.ok) return refuseRanges(list, "the channel list could not be rendered", 500);
    return RangesResponse{200, rendered.text};
}

RangesRequest parseRangesRequest(RangeList list, const std::string& body) {
    const std::string key = keyOf(list);
    if (body.size() > kExcludedRangesBodyLimitBytes) return refused(list, "the request body is too large");

    json::ParseResult parsed = json::parse(body);
    if (!parsed.ok || parsed.value.type() != json::Type::kObject) return refused(list, "the body is not a JSON object");

    const json::Value* items = nullptr;
    for (const json::Value::Member& m : parsed.value.members()) {
        if (m.first == key) {
            items = &m.second;
        } else if (m.first != "schemaVersion") {
            return refused(list, "unknown key \"" + m.first + "\"");
        }
    }
    if (items == nullptr || items->type() != json::Type::kArray) return refused(list, key + " must be a list");

    RangesRequest request;
    for (const json::Value& item : items->items()) {
        const std::string where = key + "[" + std::to_string(request.ranges.size()) + "]";
        if (item.type() != json::Type::kObject || item.members().size() != 2) {
            return refused(list, where + " must hold exactly startChannel and channelCount");
        }
        ChannelRange range;
        bool haveStart = false;
        bool haveCount = false;
        for (const json::Value::Member& m : item.members()) {
            if (m.first == "startChannel") haveStart = channelNumber(m.second, &range.startChannel);
            if (m.first == "channelCount") haveCount = channelNumber(m.second, &range.channelCount);
        }
        if (!haveStart || !haveCount) return refused(list, where + " needs whole numbers of 1 or more");
        if (static_cast<double>(range.startChannel) + static_cast<double>(range.channelCount) > 4294967295.0) {
            return refused(list, where + " runs past the last channel");
        }
        request.ranges.push_back(range);
    }
    request.ok = true;
    return request;
}

}  // namespace showmesh
