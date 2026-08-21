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

}  // namespace showmesh
