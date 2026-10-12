#pragma once

#include <string>
#include <vector>

#include "channeloutput/ChannelOutputSetup.h"
#include "log.h"
#include "settings.h"
#include "showmesh/brightness.h"
#include "showmesh/channel_ranges_setting.h"
#include "showmesh/runtime.h"

// Configuring the engine's apply ranges from something narrower than the
// whole channel buffer. Neither adapter called configureRanges before, so
// the engine scaled the default 8 MiB FPPD_MAX_CHANNELS span on every
// frame regardless of how few channels the show actually used.
//
// parseChannelRangesSetting itself lives in the host-neutral core
// (showmesh/channel_ranges_setting.h) so it is unit tested directly; this
// header keeps only the FPP setting lookup and the GetOutputRanges()
// fallback, which reach for FPP headers the core must never include.

namespace showmesh {
namespace adapter {

// The plugin setting name, shared between the lookup below and each
// adapter's registerSettingsListener() call so the two cannot name it
// differently and silently stop agreeing.
inline constexpr const char* kChannelRangesSettingName = "ShowMeshChannelRanges";
// The channels the ceiling never changes, in the same "start-count" syntax.
inline constexpr const char* kExcludeChannelRangesSettingName = "ShowMeshExcludeChannelRanges";

// Configures the engine's apply ranges rather than leaving them at the
// default, which is the whole totalChannels buffer. Priority: the
// "ShowMeshChannelRanges" plugin setting (an operator-controlled,
// comma-separated "start-count" list), then FPP's own GetOutputRanges()
// (the channel spans FPP's output configuration actually uses), then the
// full buffer only if neither is available. A setting that parses to a
// non-empty list but fails engine validation (overlap, a range past
// totalChannels, or similar) is logged and then falls back to
// GetOutputRanges(), the same as an empty or unparsed setting would.
// "ShowMeshExcludeChannelRanges" is excluded from whichever list wins.
// The decisions themselves are configureChannelRangesFrom's, in the core.
//
// Called once at plugin construction and again whenever either setting
// changes, via registerSettingsListener()
// (settings.h), which each adapter wires up in its constructor and tears
// down before its library can be unmapped. This is FPP's global settings
// store, not the per-plugin config file FPPPlugins::Plugin(name, true)
// watches; that two-argument constructor and its settingChanged()
// override are a different, unrelated mechanism and are not used here.
// A channel added to the output config after startup, with no
// ShowMeshChannelRanges change, is not picked up until the plugin
// restarts: no FPPPlugin virtual fires for an output-config reload that
// leaves the setting untouched, so this is a known limitation, not a
// silent gap.
inline void configureChannelRanges(ShowMeshRuntime* runtime, std::uint32_t totalChannels) {
    ChannelRangeSources sources;
    sources.applySetting = getSetting(kChannelRangesSettingName);
    sources.excludeSetting = getSetting(kExcludeChannelRangesSettingName);
    // GetOutputRanges() reports 0-based (start, count) pairs; ChannelRange
    // is 1-based to match FPP's channel numbering elsewhere.
    for (const auto& r : GetOutputRanges(true)) {
        if (r.second == 0) continue;
        sources.outputRanges.push_back(ChannelRange{r.first + 1, r.second});
    }
    for (const std::string& line : runtime->configureChannelRanges(sources, totalChannels)) {
        LogErr(VB_PLUGIN, "%s\n", line.c_str());
    }
}

// The two range writes' stores: persist = true writes FPP's settings file
// and then calls this plugin's own listener, which applies the value.
inline bool storeExcludeChannelRangesSetting(const std::string& value) {
    setSetting(kExcludeChannelRangesSettingName, value, true);
    return true;
}

inline bool storeChannelRangesSetting(const std::string& value) {
    setSetting(kChannelRangesSettingName, value, true);
    return true;
}

}  // namespace adapter
}  // namespace showmesh
