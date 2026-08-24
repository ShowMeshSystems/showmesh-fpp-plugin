#include "showmesh/observation_payload.h"

#include <string>

#include "check.h"
#include "showmesh/json.h"
#include "showmesh/sha256.h"

using showmesh::buildDefinitionBody;
using showmesh::buildObservationBody;
using showmesh::IdentityUnavailable;
using showmesh::observationUnavailableWireValue;
using showmesh::PayloadResult;
using showmesh::PlaylistAction;
using showmesh::PlaylistEntryObservation;
using showmesh::resolveEntryIdentity;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(EveryUnavailableReasonHasTheContractsWireSpellingNotTheHumanOne) {
    CHECK_EQ(std::string(observationUnavailableWireValue(IdentityUnavailable::kMissingInstanceUuid)),
             std::string("missing_instance_uuid"));
    CHECK_EQ(std::string(observationUnavailableWireValue(IdentityUnavailable::kMissingPlaylistName)),
             std::string("missing_playlist_name"));
    CHECK_EQ(std::string(observationUnavailableWireValue(IdentityUnavailable::kMissingDefinition)),
             std::string("missing_definition"));
    CHECK_EQ(std::string(observationUnavailableWireValue(IdentityUnavailable::kUnsupportedDefinitionShape)),
             std::string("unsupported_definition_shape"));
    CHECK_EQ(std::string(observationUnavailableWireValue(IdentityUnavailable::kNegativePosition)),
             std::string("negative_position"));
    CHECK_EQ(std::string(observationUnavailableWireValue(IdentityUnavailable::kTruncatedIdentityField)),
             std::string("truncated_identity_field"));
    CHECK_EQ(std::string(observationUnavailableWireValue(IdentityUnavailable::kNone)), std::string());
}

TEST(ObservationNumbersTravelAsJsonNumbersNotStrings) {
    PlaylistEntryObservation observation;
    observation.identity.instanceUuid = "M4-7840e12f81da4191c0d00fbb6a889314";
    observation.identity.playlistName = "Halloween";
    observation.identity.playlistHash = std::string(64, 'a');
    observation.identity.section = "";
    observation.identity.position = 0;
    observation.entryKey = std::string(64, 'b');
    observation.action = PlaylistAction::kQueryNext;
    observation.sequence = 1234567;
    observation.observedAtMillis = 1755900000000;

    const PayloadResult payload = buildObservationBody(observation);
    CHECK(payload.ok);
    CHECK(contains(payload.body, "\"position\":0"));
    CHECK(contains(payload.body, "\"sequence\":1234567"));
    CHECK(contains(payload.body, "\"observedAtMillis\":1755900000000"));
    CHECK(contains(payload.body, "\"action\":\"query_next\""));
    // An empty section is a legitimate value and travels as one.
    CHECK(contains(payload.body, "\"section\":\"\""));
}

TEST(TheDefinitionBodyHashesBackToTheHashItDeclares) {
    const std::string definition =
        "{ \"name\" : \"Halloween\", \"mainPlaylist\" : [ { \"sequenceName\" : \"Thriller.fseq\" } ] }";
    showmesh::IdentityResolution resolution =
        resolveEntryIdentity("M4-7840", "Halloween", definition, "mainPlaylist", 0);
    CHECK(resolution.ok);

    const PayloadResult payload = buildDefinitionBody("M4-7840", "Halloween", resolution.identity.playlistHash,
                                                      resolution.canonicalDefinition, 1755900000000);
    CHECK(payload.ok);

    // What the coordinator does on receipt: pull `definition` back out,
    // canonicalize it, and refuse the request unless it hashes to the
    // declared playlistHash.
    showmesh::json::ParseResult parsed = showmesh::json::parse(payload.body);
    CHECK(parsed.ok);
    bool checked = false;
    for (const auto& member : parsed.value.members()) {
        if (member.first != "definition") continue;
        showmesh::json::CanonicalResult canonical = showmesh::json::canonicalize(member.second);
        CHECK(canonical.ok);
        CHECK_EQ(showmesh::sha256Hex(canonical.text), resolution.identity.playlistHash);
        checked = true;
    }
    CHECK(checked);
}

TEST(ADefinitionBodyWithoutAnIdentityFieldIsRefusedRatherThanSentIncomplete) {
    CHECK(!buildDefinitionBody("", "Halloween", std::string(64, 'a'), "{}", 1).ok);
    CHECK(!buildDefinitionBody("M4-7840", "", std::string(64, 'a'), "{}", 1).ok);
    CHECK(!buildDefinitionBody("M4-7840", "Halloween", "", "{}", 1).ok);
    CHECK(!buildDefinitionBody("M4-7840", "Halloween", std::string(64, 'a'), "not json", 1).ok);
    CHECK(!buildDefinitionBody("M4-7840", "Halloween", std::string(64, 'a'), "[1,2,3]", 1).ok);
}
