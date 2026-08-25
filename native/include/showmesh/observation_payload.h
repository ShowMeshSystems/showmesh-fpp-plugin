#pragma once

#include <cstddef>
#include <string>

#include "showmesh/playlist_identity.h"

// The two coordinator wire bodies, built exactly as the frozen contract
// fixes them (FPP-PLUGIN-COORDINATOR-CONTRACTS.md sections 1.2 and 3.3,
// mirrored under docs/upstream/showmesh/). Nothing here invents a field,
// renames one, or spells an enum differently from the table: a
// disagreement with the coordinator is a blocker raised against both
// repositories, never a unilateral change on one side.

namespace showmesh {

// The route paths, relative to the coordinator base URL.
extern const char* const kObservationPath;
extern const char* const kDefinitionPath;

// The bounds the coordinator refuses with 413 above. Checked here too, so
// an oversized body is a visible local refusal rather than a round trip
// spent to be told.
constexpr std::size_t kObservationBodyLimitBytes = 16384;
constexpr std::size_t kDefinitionBodyLimitBytes = 1048576;

// The `unavailable` wire spellings. These are NOT the human strings
// identityUnavailableReason() returns; the contract names both and says
// plainly which one travels.
const char* observationUnavailableWireValue(IdentityUnavailable reason);

struct PayloadResult {
    bool ok = false;
    std::string body;
    std::string error;
};

// Section 1.2. Derived identity (playlistHash, entryKey) is emitted only
// when `unavailable` is absent, and never when it is present: the
// coordinator refuses an unavailable observation carrying either, because
// neither can exist without the definition.
PayloadResult buildObservationBody(const PlaylistEntryObservation& observation);

// Section 3.3. canonicalDefinition is the exact canonical bytes the
// plugin hashed; it is parsed back into a JSON value here so `definition`
// travels as the object itself, which is what the coordinator
// re-canonicalizes and re-hashes before it files anything.
PayloadResult buildDefinitionBody(const std::string& instanceUuid, const std::string& playlistName,
                                  const std::string& playlistHash, const std::string& canonicalDefinition,
                                  TimeMillis capturedAtMillis);

}  // namespace showmesh
