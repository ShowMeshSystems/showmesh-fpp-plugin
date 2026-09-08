#pragma once

#include <optional>
#include <string>

#include "fpp-json-compat.h"

// Pulling the bounded evidence out of FPP's playlist JSON, shared by both
// adapters because it is the same JSON on both majors. Everything here
// runs on FPP's callback thread, so it does only member lookups and small
// string copies: no definition fetch, no hash, no write.

namespace showmesh {
namespace adapter {

inline std::string jsonString(const Json::Value& v, const char* key) {
    if (!v.isObject() || !v.isMember(key)) return std::string();
    const Json::Value& field = v[key];
    return field.isString() ? field.asString() : std::string();
}

// The playlist name as FPP reports it in the callback's own playlist
// object. An empty result is reported as an unavailable observation later;
// it is never replaced with a filename.
inline std::string playlistNameOf(const Json::Value& playlist) {
    std::string name = jsonString(playlist, "name");
    if (name.empty()) name = jsonString(playlist, "playlistName");
    return name;
}

// The currently playing entry's own config, when the callback's own
// playlist object carries one. FPP's Playlist::GetInfo() nests this under
// "currentEntry" (Playlist::GetCurrentEntry() -> CurrentEntry()->GetConfig()),
// not under an array keyed by section: the playlist object has no section
// arrays at all on FPP 10. GetCurrentEntry() returns an empty (non-object)
// value while the playlist is idle, which isMember() below treats the same
// as "not present". Absent evidence stays absent.
inline const Json::Value* currentEntryOf(const Json::Value& playlist) {
    if (!playlist.isObject() || !playlist.isMember("currentEntry")) return nullptr;
    const Json::Value& entry = playlist["currentEntry"];
    return entry.isObject() ? &entry : nullptr;
}

// PlaylistEntrySequence::GetConfig() writes the sequence's filename to
// "sequenceName". A media or other non-sequence entry has no such member,
// so this stays absent rather than guessing.
inline std::string sequenceFilenameOf(const Json::Value& playlist) {
    const Json::Value* entry = currentEntryOf(playlist);
    return entry == nullptr ? std::string() : jsonString(*entry, "sequenceName");
}

// PlaylistEntryMedia::GetConfig() writes the media filename to
// "mediaFilename" (the playlist *definition* spells the same value
// "mediaName"; the runtime config field is named differently). A
// sequence-only entry has no such member, so this stays absent.
inline std::string mediaFilenameOf(const Json::Value& playlist) {
    const Json::Value* entry = currentEntryOf(playlist);
    return entry == nullptr ? std::string() : jsonString(*entry, "mediaFilename");
}

// FPP's own mainPlaylist pass counter. `loop` is the counter and
// `loopCount` is the configured LIMIT the counter is compared against:
// Playlist::Process() does `m_loop++` and then tests
// `!m_loopCount || (m_loop < m_loopCount)`, identically on FPP 9.5.3 and
// FPP 10.0. Reading `loopCount` here would report a repeat limit as if it
// were a lap number, and for the common unlimited-repeat playlist it is 0
// forever, which is a value that never changes and therefore never
// separates one visit from the next.
//
// Absent while the playlist is idle, even though GetInfo() does write a
// `loop` of 0 in that branch: that 0 is a placeholder for "no playlist is
// running", not the running playlist's first pass, and the contract makes
// absent and 0 different values. Absent when the member is missing or is
// not an integer, for the same reason: a guess here would be
// corroborating evidence that corroborates nothing.
inline std::optional<int> playlistLoopOf(const Json::Value& playlist) {
    if (!playlist.isObject() || !playlist.isMember("loop")) return std::nullopt;
    if (jsonString(playlist, "currentState") == "idle") return std::nullopt;
    const Json::Value& loop = playlist["loop"];
    if (!loop.isIntegral()) return std::nullopt;
    return loop.asInt();
}

}  // namespace adapter
}  // namespace showmesh
