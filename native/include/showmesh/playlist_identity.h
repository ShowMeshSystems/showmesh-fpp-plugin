#pragma once

#include <cstdint>
#include <string>

#include "showmesh/fading_value.h"

namespace showmesh {

// The four actions FPP's playlist callback reports. A repeated `playing`
// carrying a new section or position is item advancement, not a duplicate.
enum class PlaylistAction { kStart, kPlaying, kStop, kQueryNext, kUnknown };

const char* playlistActionName(PlaylistAction action);
PlaylistAction playlistActionFromName(const std::string& name);

// Why an observation could not carry identity. Identity never silently
// degrades to filename matching: an entry whose playlist definition or
// instance UUID is unavailable is reported as unavailable, with the reason.
enum class IdentityUnavailable {
    kNone,
    kMissingInstanceUuid,
    kMissingPlaylistName,
    kMissingDefinition,
    kUnsupportedDefinitionShape,
    kNegativePosition,
    // A field that determines identity (playlist name or section) was
    // truncated on the callback thread: the copy is bounded evidence of a
    // longer value, never the value itself, so building identity from it
    // would be confidently wrong rather than merely missing.
    kTruncatedIdentityField,
};

const char* identityUnavailableReason(IdentityUnavailable reason);

// The observation schema is versioned independently of the plugin release.
constexpr int kObservationSchemaVersion = 1;

struct EntryIdentity {
    std::string instanceUuid;
    std::string playlistName;
    // SHA-256, lowercase hex, over the RFC 8785 canonicalization of the
    // complete playlist definition FPP returned.
    std::string playlistHash;
    std::string section;
    int position = 0;
};

// deriveEntryKey hashes a canonical JSON object of the five identifying
// fields rather than a delimited concatenation, so no playlist name or
// section containing a separator character can collide with a different
// entry. Deterministic across restarts for an unchanged definition, and
// different for two entries at different positions even when their
// filenames are identical.
std::string deriveEntryKey(const EntryIdentity& identity);

struct IdentityResolution {
    bool ok = false;
    IdentityUnavailable reason = IdentityUnavailable::kNone;
    std::string error;
    EntryIdentity identity;
    // The exact canonical bytes hashed, retained as the import and
    // reconciliation evidence for this observation.
    std::string canonicalDefinition;
    std::string entryKey;
};

// Resolves identity from the raw playlist definition FPP's own
// playlist-definition API returned. No runtime field is removed from the
// definition before hashing.
IdentityResolution resolveEntryIdentity(const std::string& instanceUuid, const std::string& playlistName,
                                        const std::string& playlistDefinitionJson, const std::string& section,
                                        int position);

// The complete observation the resident worker builds. Serializing this to
// the coordinator's ingestion payload is deliberately not implemented here:
// that wire shape is frozen by the coordinator contract, and inventing a
// parallel one would be worse than not having it.
struct PlaylistEntryObservation {
    int schemaVersion = kObservationSchemaVersion;
    EntryIdentity identity;
    std::string entryKey;
    std::string sequenceFilename;
    std::string mediaFilename;
    // True when the copy on the callback thread lost bytes off the end of
    // the source field. These are corroborating evidence, not identity, so
    // truncation here does not gate the observation the way a truncated
    // playlist name or section does; it is carried so the record says a
    // filename was cut rather than reporting it as complete.
    bool sequenceFilenameTruncated = false;
    bool mediaFilenameTruncated = false;
    PlaylistAction action = PlaylistAction::kUnknown;
    std::uint64_t sequence = 0;
    TimeMillis observedAtMillis = 0;
    std::uint32_t coalescedSincePreviousAcknowledged = 0;
    IdentityUnavailable unavailable = IdentityUnavailable::kNone;
};

// SequenceState is the per-instance monotonic event sequence. It only ever
// moves forward: restoring a persisted value that is behind what this
// process has already issued does not rewind it, because a coordinator that
// saw the higher value would read the repeat as a stale duplicate.
class SequenceState {
 public:
    SequenceState() = default;
    explicit SequenceState(std::uint64_t restored) : value_(restored) {}

    std::uint64_t next() { return ++value_; }
    std::uint64_t current() const { return value_; }
    void restore(std::uint64_t persisted) {
        if (persisted > value_) value_ = persisted;
    }

 private:
    std::uint64_t value_ = 0;
};

}  // namespace showmesh
