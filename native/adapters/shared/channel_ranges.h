#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include "channeloutput/ChannelOutputSetup.h"
#include "settings.h"
#include "showmesh/brightness.h"

// Configuring the engine's apply ranges from something narrower than the
// whole channel buffer. Neither adapter called configureRanges before, so
// the engine scaled the default 8 MiB FPPD_MAX_CHANNELS span on every
// frame regardless of how few channels the show actually used.

namespace showmesh {
namespace adapter {

// Parses a comma-separated "start-count[,start-count...]" setting value,
// one-based to match ChannelRange. A malformed token is skipped rather
// than crashing the plugin on a hand-edited setting.
inline std::vector<ChannelRange> parseChannelRangesSetting(const std::string& value) {
    std::vector<ChannelRange> ranges;
    std::size_t pos = 0;
    while (pos < value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::string token = value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const std::size_t dash = token.find('-');
        if (dash != std::string::npos) {
            char* endStart = nullptr;
            char* endCount = nullptr;
            const unsigned long start = std::strtoul(token.substr(0, dash).c_str(), &endStart, 10);
            const unsigned long count = std::strtoul(token.substr(dash + 1).c_str(), &endCount, 10);
            const bool wholeToken = endStart != nullptr && *endStart == '\0' && endCount != nullptr && *endCount == '\0';
            if (wholeToken && start > 0 && count > 0) {
                ranges.push_back(ChannelRange{static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(count)});
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return ranges;
}

// Configures the engine's apply ranges rather than leaving them at the
// default, which is the whole totalChannels buffer. Priority: the
// "ShowMeshChannelRanges" plugin setting (an operator-controlled,
// comma-separated "start-count" list), then FPP's own GetOutputRanges()
// (the channel spans FPP's output configuration actually uses), then the
// full buffer only if neither is available. GetOutputRanges()'s reported
// span, and this whole configuration path, are unmeasured on real
// hardware: only the setting path has been exercised, in a test fixture.
inline void configureChannelRanges(BrightnessEngine* engine, std::uint32_t totalChannels) {
    RangeConfig config;
    const std::string setting = getSetting("ShowMeshChannelRanges");
    if (!setting.empty()) {
        config.apply = parseChannelRangesSetting(setting);
    }
    if (config.apply.empty()) {
        // GetOutputRanges() reports 0-based (start, count) pairs; ChannelRange
        // is 1-based to match FPP's channel numbering elsewhere.
        for (const auto& r : GetOutputRanges(true)) {
            if (r.second == 0) continue;
            config.apply.push_back(ChannelRange{r.first + 1, r.second});
        }
    }
    if (config.apply.empty()) {
        config.apply.push_back(ChannelRange{1, totalChannels});
    }
    engine->configureRanges(config, totalChannels);
}

}  // namespace adapter
}  // namespace showmesh
