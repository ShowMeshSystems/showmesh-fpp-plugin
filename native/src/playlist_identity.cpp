#include "showmesh/playlist_identity.h"

#include <cctype>
#include <vector>

#include "showmesh/json.h"
#include "showmesh/sha256.h"

namespace showmesh {

bool isUnprovisionedInstanceUuid(const std::string& value) {
    static const char* const kUnprovisioned = "unknown";
    if (value.size() != 7) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != kUnprovisioned[i]) return false;
    }
    return true;
}

const char* playlistActionName(PlaylistAction action) {
    switch (action) {
        case PlaylistAction::kStart: return "start";
        case PlaylistAction::kPlaying: return "playing";
        case PlaylistAction::kStop: return "stop";
        case PlaylistAction::kQueryNext: return "query_next";
        case PlaylistAction::kUnknown: return "unknown";
    }
    return "unknown";
}

PlaylistAction playlistActionFromName(const std::string& name) {
    if (name == "start") return PlaylistAction::kStart;
    if (name == "playing") return PlaylistAction::kPlaying;
    if (name == "stop") return PlaylistAction::kStop;
    if (name == "query_next") return PlaylistAction::kQueryNext;
    return PlaylistAction::kUnknown;
}

const char* identityUnavailableReason(IdentityUnavailable reason) {
    switch (reason) {
        case IdentityUnavailable::kNone: return "";
        case IdentityUnavailable::kMissingInstanceUuid: return "the FPP instance UUID is not available";
        case IdentityUnavailable::kMissingPlaylistName: return "the playlist name is not available";
        case IdentityUnavailable::kMissingDefinition: return "the playlist definition is not available";
        case IdentityUnavailable::kUnsupportedDefinitionShape: return "the playlist definition is not usable JSON";
        case IdentityUnavailable::kNegativePosition: return "the item position is negative";
        case IdentityUnavailable::kTruncatedIdentityField:
            return "a field needed for identity was truncated before it could be compared";
    }
    return "";
}

std::string deriveEntryKey(const EntryIdentity& identity) {
    std::vector<json::Value::Member> members;
    members.emplace_back("instanceUuid", json::Value::makeString(identity.instanceUuid));
    members.emplace_back("playlistHash", json::Value::makeString(identity.playlistHash));
    members.emplace_back("playlistName", json::Value::makeString(identity.playlistName));
    members.emplace_back("position", json::Value::makeNumber(static_cast<double>(identity.position)));
    members.emplace_back("section", json::Value::makeString(identity.section));

    json::CanonicalResult canonical = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!canonical.ok) return std::string();
    return sha256Hex(canonical.text);
}

IdentityResolution resolveEntryIdentity(const std::string& instanceUuid, const std::string& playlistName,
                                        const std::string& playlistDefinitionJson, const std::string& section,
                                        int position) {
    IdentityResolution r;
    if (instanceUuid.empty()) {
        r.reason = IdentityUnavailable::kMissingInstanceUuid;
        r.error = identityUnavailableReason(r.reason);
        return r;
    }
    if (playlistName.empty()) {
        r.reason = IdentityUnavailable::kMissingPlaylistName;
        r.error = identityUnavailableReason(r.reason);
        return r;
    }
    if (playlistDefinitionJson.empty()) {
        r.reason = IdentityUnavailable::kMissingDefinition;
        r.error = identityUnavailableReason(r.reason);
        return r;
    }
    if (position < 0) {
        r.reason = IdentityUnavailable::kNegativePosition;
        r.error = identityUnavailableReason(r.reason);
        return r;
    }

    json::CanonicalResult canonical = json::canonicalize(playlistDefinitionJson);
    if (!canonical.ok) {
        r.reason = IdentityUnavailable::kUnsupportedDefinitionShape;
        r.error = canonical.error;
        return r;
    }

    r.ok = true;
    r.canonicalDefinition = canonical.text;
    r.identity.instanceUuid = instanceUuid;
    r.identity.playlistName = playlistName;
    r.identity.playlistHash = sha256Hex(canonical.text);
    r.identity.section = section;
    r.identity.position = position;
    r.entryKey = deriveEntryKey(r.identity);
    return r;
}

}  // namespace showmesh
