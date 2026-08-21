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

// The entry at one position within one section, when the callback's own
// playlist object carries it. Absent evidence stays absent.
inline const Json::Value* entryAt(const Json::Value& playlist, const std::string& section, int item) {
    if (item < 0 || section.empty() || !playlist.isObject() || !playlist.isMember(section)) return nullptr;
    const Json::Value& entries = playlist[section];
    if (!entries.isArray() || static_cast<Json::ArrayIndex>(item) >= entries.size()) return nullptr;
    return &entries[static_cast<Json::ArrayIndex>(item)];
}

inline std::string sequenceFilenameOf(const Json::Value& playlist, const std::string& section, int item) {
    const Json::Value* entry = entryAt(playlist, section, item);
    return entry == nullptr ? std::string() : jsonString(*entry, "sequenceName");
}

inline std::string mediaFilenameOf(const Json::Value& playlist, const std::string& section, int item) {
    const Json::Value* entry = entryAt(playlist, section, item);
    return entry == nullptr ? std::string() : jsonString(*entry, "mediaName");
}

}  // namespace adapter
}  // namespace showmesh
