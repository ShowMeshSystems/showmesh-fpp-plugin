#pragma once

#include <string>

// The pure parser for the "ShowMeshSafeCeilingPercent" setting value, host
// neutral so it can be unit tested without an FPP source tree. The
// adapter-side lookup of the setting stays in
// native/adapters/shared/safe_ceiling.h, which reaches for FPP headers
// this file must never include.

namespace showmesh {

// Parses a whole-number percent (0-100) from the setting's text value.
// Surrounding whitespace is trimmed; anything else that fails to parse as
// a whole number, or that parses but falls outside 0-100, is rejected.
// The caller (safe_ceiling.h's adapter lookup) falls back to
// kDefaultSafeCeilingPercent and logs when this returns false: an absent,
// empty, unparseable, or out-of-range value is a read error, not an
// operator's explicit choice, and a read error must not turn the rig off.
bool parseSafeCeilingSetting(const std::string& value, int* outPercent);

}  // namespace showmesh
