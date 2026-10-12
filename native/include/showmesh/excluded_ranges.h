#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "showmesh/brightness.h"
#include "showmesh/json.h"

// The write for the channels the brightness ceiling never changes. Host
// neutral, and registered, bounded and unauthenticated the same way as the
// transition-gain route (see transition_gain.h).

namespace showmesh {

// See transition_gain.h for why the registered path and the LAN address
// are two different strings.
extern const char* const kExcludedRangesPath;
extern const char* const kExcludedRangesLanPath;

constexpr std::size_t kExcludedRangesBodyLimitBytes = 4096;

constexpr int kExcludedRangesSchemaVersion = 1;

struct ExcludedRangesResponse {
    // 200 for an applied write, 400 for a refused body, 500 when the
    // setting could not be stored.
    int status = 400;
    // The response body, always JSON, always populated.
    std::string body;
};

struct ExcludedRangesRequest {
    bool ok = false;
    std::vector<ChannelRange> ranges;
    // The refusal to answer with when ok is false.
    ExcludedRangesResponse refusal;
};

// Stores the setting text an applied write is kept under, so the FPP page
// shows it and a restart reads it back. Returns false when it was not stored.
using ExcludedRangesStore = std::function<bool(const std::string& settingValue)>;

// Parses {"excludedRanges":[{"startChannel":1,"channelCount":512}]}; an
// empty list clears the exclusions. "schemaVersion" is ignored, and any other
// key, a fraction, a zero, or a value past uint32 is refused, not clamped.
ExcludedRangesRequest parseExcludedRangesRequest(const std::string& body);

// 400 by default: one line naming what is wrong, then the form to send.
ExcludedRangesResponse refuseExcludedRanges(const std::string& detail, int status = 400);

// 200 with {"schemaVersion":1,"excludedRanges":[...]}, the list the engine holds.
ExcludedRangesResponse renderExcludedRanges(const std::vector<ChannelRange>& ranges);

// The [{startChannel, channelCount}] array, shared with the brightness query.
json::Value excludedRangesValue(const std::vector<ChannelRange>& ranges);

}  // namespace showmesh
