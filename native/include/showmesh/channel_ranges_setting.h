#pragma once

#include <string>
#include <vector>

#include "showmesh/brightness.h"

// The pure parser for the "ShowMeshChannelRanges" setting value, host
// neutral so it can be unit tested without an FPP source tree. The
// adapter-side lookup of the setting and the GetOutputRanges() fallback
// stay in native/adapters/shared/channel_ranges.h, which reaches for FPP
// headers this file must never include.

namespace showmesh {

// Parses a comma-separated "start-count[,start-count...]" setting value,
// one-based to match ChannelRange. A token is skipped, rather than
// crashing the plugin on a hand-edited setting, when it is malformed, when
// its start or count is zero or does not fit a uint32_t, or when
// start + count would overflow uint32_t and wrap ChannelRange::endExclusive().
std::vector<ChannelRange> parseChannelRangesSetting(const std::string& value);

struct ChannelRangesParse {
    bool ok = true;
    std::vector<ChannelRange> ranges;
    std::string error;
};

// The "ShowMeshExcludeChannelRanges" form of the same syntax: one bad token
// fails the whole value, because a skipped token would leave a DMX fixture
// scaled with nothing said. An empty value is valid and means no exclusions.
ChannelRangesParse parseChannelRangesSettingStrict(const std::string& value);

// Renders ranges back into the setting syntax, "start-count,start-count".
std::string formatChannelRangesSetting(const std::vector<ChannelRange>& ranges);

// What an adapter reads from FPP before configuring the engine's ranges.
struct ChannelRangeSources {
    std::string applySetting;
    std::string excludeSetting;
    // FPP's own output spans, already one-based.
    std::vector<ChannelRange> outputRanges;
};

// Configures engine and returns the lines the adapter logs. Apply list:
// applySetting, else outputRanges, else the whole buffer. A rejected exclude
// value keeps the engine's exclusions and is logged once per value.
std::vector<std::string> configureChannelRangesFrom(const ChannelRangeSources& sources, std::uint32_t totalChannels,
                                                    BrightnessEngine* engine, std::string* lastRejectedExclude);

// Configures engine with the first apply list it accepts: setting, else
// outputRanges, else the whole buffer. Appends a line to log for each rejection.
void configureApplyRanges(const std::vector<ChannelRange>& setting, const std::vector<ChannelRange>& outputRanges,
                          const std::vector<ChannelRange>& exclude, std::uint32_t totalChannels,
                          BrightnessEngine* engine, std::vector<std::string>* log);

}  // namespace showmesh
