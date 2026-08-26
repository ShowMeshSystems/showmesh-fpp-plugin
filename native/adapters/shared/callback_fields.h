#pragma once

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

}  // namespace adapter
}  // namespace showmesh
