#include "showmesh/observation_payload.h"

#include <utility>
#include <vector>

#include "showmesh/json.h"

namespace showmesh {

const char* const kObservationPath = "/api/v1/integrations/fpp/playlist-entry-observations";
const char* const kDefinitionPath = "/api/v1/integrations/fpp/playlist-definitions";

namespace {

void addString(std::vector<json::Value::Member>* members, const char* name, const std::string& value) {
    members->emplace_back(name, json::Value::makeString(value));
}

void addNumber(std::vector<json::Value::Member>* members, const char* name, double value) {
    members->emplace_back(name, json::Value::makeNumber(value));
}

PayloadResult serialize(std::vector<json::Value::Member> members) {
    PayloadResult result;
    json::CanonicalResult canonical = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!canonical.ok) {
        result.error = canonical.error;
        return result;
    }
    result.ok = true;
    result.body = std::move(canonical.text);
    return result;
}

}  // namespace

const char* observationUnavailableWireValue(IdentityUnavailable reason) {
    switch (reason) {
        case IdentityUnavailable::kNone: return "";
        case IdentityUnavailable::kMissingInstanceUuid: return "missing_instance_uuid";
        case IdentityUnavailable::kMissingPlaylistName: return "missing_playlist_name";
        case IdentityUnavailable::kMissingDefinition: return "missing_definition";
        case IdentityUnavailable::kUnsupportedDefinitionShape: return "unsupported_definition_shape";
        case IdentityUnavailable::kNegativePosition: return "negative_position";
        case IdentityUnavailable::kTruncatedIdentityField: return "truncated_identity_field";
    }
    return "";
}

PayloadResult buildObservationBody(const PlaylistEntryObservation& observation) {
    PayloadResult result;
    if (observation.identity.instanceUuid.empty()) {
        // An observation with no instance cannot be attributed to one and
        // the coordinator refuses it outright, so it is refused here
        // instead of spending a round trip and an audit entry to be told.
        result.error = "an observation with no instance UUID cannot be attributed to an FPP instance";
        return result;
    }

    const bool unavailable = observation.unavailable != IdentityUnavailable::kNone;

    std::vector<json::Value::Member> members;
    addNumber(&members, "schemaVersion", observation.schemaVersion);
    addString(&members, "instanceUuid", observation.identity.instanceUuid);
    addString(&members, "action", playlistActionName(observation.action));
    addNumber(&members, "sequence", static_cast<double>(observation.sequence));
    addNumber(&members, "observedAtMillis", static_cast<double>(observation.observedAtMillis));
    addNumber(&members, "coalescedSincePreviousAcknowledged",
              static_cast<double>(observation.coalescedSincePreviousAcknowledged));

    if (unavailable) {
        addString(&members, "unavailable", observationUnavailableWireValue(observation.unavailable));
        // Corroborating evidence is permitted but not required on an
        // unavailable observation, and playlistName may not be empty when
        // present, so the three travel together or not at all.
        if (!observation.identity.playlistName.empty()) {
            addString(&members, "playlistName", observation.identity.playlistName);
            addString(&members, "section", observation.identity.section);
            addNumber(&members, "position", observation.identity.position);
        }
    } else {
        addString(&members, "playlistName", observation.identity.playlistName);
        addString(&members, "playlistHash", observation.identity.playlistHash);
        addString(&members, "section", observation.identity.section);
        addNumber(&members, "position", observation.identity.position);
        addString(&members, "entryKey", observation.entryKey);
    }

    // Absent when the entry has none, rather than present and empty: the
    // contract's table says "Absent when the entry has none", and an
    // empty string is a filename claim nothing observed.
    if (!observation.sequenceFilename.empty()) {
        addString(&members, "sequenceFilename", observation.sequenceFilename);
    }
    if (!observation.mediaFilename.empty()) {
        addString(&members, "mediaFilename", observation.mediaFilename);
    }

    return serialize(std::move(members));
}

PayloadResult buildDefinitionBody(const std::string& instanceUuid, const std::string& playlistName,
                                  const std::string& playlistHash, const std::string& canonicalDefinition,
                                  TimeMillis capturedAtMillis) {
    PayloadResult result;
    if (instanceUuid.empty() || playlistName.empty() || playlistHash.empty()) {
        result.error = "a definition publication needs an instance UUID, a playlist name, and a hash";
        return result;
    }

    json::ParseResult parsed = json::parse(canonicalDefinition);
    if (!parsed.ok) {
        result.error = "the retained canonical definition is not parseable JSON: " + parsed.error;
        return result;
    }
    if (parsed.value.type() != json::Type::kObject) {
        result.error = "the playlist definition is not a JSON object";
        return result;
    }

    std::vector<json::Value::Member> members;
    addNumber(&members, "schemaVersion", kObservationSchemaVersion);
    addString(&members, "instanceUuid", instanceUuid);
    addString(&members, "playlistName", playlistName);
    addString(&members, "playlistHash", playlistHash);
    members.emplace_back("definition", std::move(parsed.value));
    addNumber(&members, "capturedAtMillis", static_cast<double>(capturedAtMillis));

    return serialize(std::move(members));
}

}  // namespace showmesh
