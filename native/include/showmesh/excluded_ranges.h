#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "showmesh/brightness.h"
#include "showmesh/json.h"

// The two channel-list writes: the channels the brightness ceiling applies
// to and the channels it never changes. Host neutral, and registered, bounded
// and unauthenticated the same way as the transition-gain route.

namespace showmesh {

// See transition_gain.h for why the registered path and the LAN address
// are two different strings.
extern const char* const kExcludedRangesPath;
extern const char* const kExcludedRangesLanPath;
extern const char* const kChannelRangesPath;
extern const char* const kChannelRangesLanPath;

constexpr std::size_t kExcludedRangesBodyLimitBytes = 4096;

constexpr int kExcludedRangesSchemaVersion = 1;

// Which list a write names: kApply is "channelRanges", kExclude is
// "excludedRanges". The two bodies and answers differ only in that key.
enum class RangeList { kApply, kExclude };

struct RangesResponse {
    // 200 for an applied write, 400 for a refused body, 500 when the
    // setting could not be stored.
    int status = 400;
    // The response body, always JSON, always populated.
    std::string body;
};

struct RangesRequest {
    bool ok = false;
    std::vector<ChannelRange> ranges;
    // The refusal to answer with when ok is false.
    RangesResponse refusal;
};

using ExcludedRangesResponse = RangesResponse;
using ChannelRangesResponse = RangesResponse;

// Stores the setting text an applied write is kept under, so the FPP page
// shows it and a restart reads it back. Returns false when it was not stored.
using RangesStore = std::function<bool(const std::string& settingValue)>;

// Parses {"excludedRanges":[{"startChannel":1,"channelCount":512}]}, or the
// same under "channelRanges"; an empty list clears the setting. "schemaVersion"
// is ignored; any other key, a fraction, a zero, or a value past uint32 is refused.
RangesRequest parseRangesRequest(RangeList list, const std::string& body);

// 400 by default: one line naming what is wrong, then the form to send.
RangesResponse refuseRanges(RangeList list, const std::string& detail, int status = 400);

// 200 with {"schemaVersion":1,"<key>":[...]}, the list the engine holds.
RangesResponse renderRanges(RangeList list, const std::vector<ChannelRange>& ranges);

// The [{startChannel, channelCount}] array, shared with the brightness query.
json::Value rangesValue(const std::vector<ChannelRange>& ranges);

}  // namespace showmesh
