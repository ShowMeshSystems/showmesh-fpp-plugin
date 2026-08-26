#pragma once

#include <string>

#include "log.h"
#include "settings.h"
#include "showmesh/brightness.h"
#include "showmesh/safe_ceiling_setting.h"

// Resolving the "ShowMeshSafeCeilingPercent" plugin setting into the
// value ShowMeshRuntime's constructor needs, at plugin construction only:
// the setting only governs restore-time behavior (what an untrusted
// restart settles to), so unlike ShowMeshChannelRanges there is no
// settings listener wired up for it -- the restore already happened by
// the time any listener could fire.
//
// parseSafeCeilingSetting itself lives in the host-neutral core
// (showmesh/safe_ceiling_setting.h) so it is unit tested directly; this
// header keeps only the FPP setting lookup, which reaches for FPP headers
// the core must never include.

namespace showmesh {
namespace adapter {

// The plugin setting name, alongside kChannelRangesSettingName.
inline constexpr const char* kSafeCeilingSettingName = "ShowMeshSafeCeilingPercent";

// Resolves the configured safe ceiling, falling back to
// kDefaultSafeCeilingPercent. A value of 0 parses successfully and is
// honored as-is: that is an operator explicitly choosing dark, which is
// different from a read error choosing it for them.
//
// An unset setting is the ordinary state of every host that never
// configured one, so it takes the default silently, the same way
// configureChannelRanges says nothing about an absent
// ShowMeshChannelRanges. Only a value that is present and unusable is
// logged, because that one means an operator tried to configure this and
// did not get what they asked for. Logging the absent case at error
// level would put a line in every FPP host's log at every start and
// teach an operator to ignore this plugin's errors.
inline int resolveSafeCeilingPercent() {
    const std::string setting = getSetting(kSafeCeilingSettingName);
    if (setting.empty()) {
        return kDefaultSafeCeilingPercent;
    }
    int percent = kDefaultSafeCeilingPercent;
    if (parseSafeCeilingSetting(setting, &percent)) {
        return percent;
    }
    LogErr(VB_PLUGIN,
           "ShowMeshSafeCeilingPercent setting value is not a whole number between 0 and 100; using the "
           "default of %d instead\n",
           kDefaultSafeCeilingPercent);
    return kDefaultSafeCeilingPercent;
}

}  // namespace adapter
}  // namespace showmesh
