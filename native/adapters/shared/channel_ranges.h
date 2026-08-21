#pragma once

#include <string>
#include <vector>

#include "channeloutput/ChannelOutputSetup.h"
#include "log.h"
#include "settings.h"
#include "showmesh/brightness.h"
#include "showmesh/channel_ranges_setting.h"

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
// adapter's settingChanged() override so the two cannot name it
// differently and silently stop agreeing.
inline constexpr const char* kChannelRangesSettingName = "ShowMeshChannelRanges";

// Configures the engine's apply ranges rather than leaving them at the
// default, which is the whole totalChannels buffer. Priority: the
// "ShowMeshChannelRanges" plugin setting (an operator-controlled,
// comma-separated "start-count" list), then FPP's own GetOutputRanges()
// (the channel spans FPP's output configuration actually uses), then the
// full buffer only if neither is available. A setting that parses to a
// non-empty list but fails engine validation (overlap, a range past
// totalChannels, or similar) is rejected outright and reported rather
// than silently falling back to GetOutputRanges(), so the operator sees
// why their setting was not applied.
//
// Called once at plugin construction and again whenever the
// "ShowMeshChannelRanges" setting itself changes (each adapter's
// settingChanged() calls this). There is no broader hook in either
// pinned Plugin.h for an output-config reload that leaves the setting
// untouched: FPPPlugin's virtuals are multiSyncData, settingChanged, the
// ChannelOutputPlugin/PlaylistEventPlugin/ChannelDataPlugin/
// APIProviderPlugin surfaces, and (FPP 10 only) shutdown, none of which
// fire on a channel-output reconfiguration. A channel added to the
// output config after startup, with no setting change, is not picked up
// until the plugin restarts; this is a known limitation, not a silent
// gap.
inline void configureChannelRanges(BrightnessEngine* engine, std::uint32_t totalChannels) {
    RangeConfig config;
    const std::string setting = getSetting(kChannelRangesSettingName);
    if (!setting.empty()) {
        config.apply = parseChannelRangesSetting(setting);
        if (!config.apply.empty()) {
            const ValidationResult result = engine->configureRanges(config, totalChannels);
            if (result.ok) return;
            LogErr(VB_PLUGIN, "ShowMeshChannelRanges setting rejected: %s; falling back to output ranges\n",
                   result.error.c_str());
            config.apply.clear();
        }
    }

    // GetOutputRanges() reports 0-based (start, count) pairs; ChannelRange
    // is 1-based to match FPP's channel numbering elsewhere.
    for (const auto& r : GetOutputRanges(true)) {
        if (r.second == 0) continue;
        config.apply.push_back(ChannelRange{r.first + 1, r.second});
    }
    if (!config.apply.empty()) {
        const ValidationResult result = engine->configureRanges(config, totalChannels);
        if (result.ok) return;
        LogErr(VB_PLUGIN, "GetOutputRanges() produced ranges the engine rejected: %s; using the full channel buffer\n",
               result.error.c_str());
        config.apply.clear();
    }

    config.apply.push_back(ChannelRange{1, totalChannels});
    engine->configureRanges(config, totalChannels);
}

}  // namespace adapter
}  // namespace showmesh
