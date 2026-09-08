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

// finding 2: negative_position is only ever produced when
// identity.position is itself negative, and the contract's ingestion step
// 7 refuses a negative position unconditionally, whether or not
// `unavailable` is set. Putting it on the wire anyway made every
// negative_position observation an automatic 400.
TEST(ANegativePositionUnavailableObservationOmitsPositionFromTheWire) {
    PlaylistEntryObservation observation;
    observation.identity.instanceUuid = "M4-7840e12f81da4191c0d00fbb6a889314";
    observation.identity.playlistName = "Halloween";
    observation.identity.section = "mainPlaylist";
    observation.identity.position = -1;
    observation.unavailable = IdentityUnavailable::kNegativePosition;
    observation.action = PlaylistAction::kPlaying;
    observation.sequence = 5;
    observation.observedAtMillis = 1755900000000;

    const PayloadResult payload = buildObservationBody(observation);
    CHECK(payload.ok);
    CHECK(contains(payload.body, "\"unavailable\":\"negative_position\""));
    CHECK(contains(payload.body, "\"playlistName\":\"Halloween\""));
    CHECK(!contains(payload.body, "\"position\""));
}

TEST(ADefinitionBodyWithoutAnIdentityFieldIsRefusedRatherThanSentIncomplete) {
    CHECK(!buildDefinitionBody("", "Halloween", std::string(64, 'a'), "{}", 1).ok);
    CHECK(!buildDefinitionBody("M4-7840", "", std::string(64, 'a'), "{}", 1).ok);
    CHECK(!buildDefinitionBody("M4-7840", "Halloween", "", "{}", 1).ok);
    CHECK(!buildDefinitionBody("M4-7840", "Halloween", std::string(64, 'a'), "not json", 1).ok);
    CHECK(!buildDefinitionBody("M4-7840", "Halloween", std::string(64, 'a'), "[1,2,3]", 1).ok);
}

TEST(PlaylistLoopIsOmittedWhenTheCallbackReportedNone) {
    PlaylistEntryObservation observation;
    observation.identity.instanceUuid = "M4-7840e12f81da4191c0d00fbb6a889314";
    observation.identity.playlistName = "Halloween";
    observation.identity.playlistHash = std::string(64, 'a');
    observation.identity.position = 0;
    observation.entryKey = std::string(64, 'b');
    observation.action = PlaylistAction::kPlaying;
    observation.sequence = 1;

    PayloadResult result = buildObservationBody(observation);
    CHECK(result.ok);
    // Not "playlistLoop":0. A plugin reporting nothing must not compare
    // equal to one reporting its first pass, and the coordinator decides a
    // loop re-entry by comparing exactly this member.
    CHECK(!contains(result.body, "playlistLoop"));
}

TEST(PlaylistLoopZeroTravelsAsARealValue) {
    PlaylistEntryObservation observation;
    observation.identity.instanceUuid = "M4-7840e12f81da4191c0d00fbb6a889314";
    observation.identity.playlistName = "Halloween";
    observation.identity.playlistHash = std::string(64, 'a');
    observation.identity.position = 0;
    observation.entryKey = std::string(64, 'b');
    observation.action = PlaylistAction::kPlaying;
    observation.sequence = 1;
    observation.playlistLoop = 0;

    PayloadResult result = buildObservationBody(observation);
    CHECK(result.ok);
    CHECK(contains(result.body, "\"playlistLoop\":0"));
}

TEST(PlaylistLoopTravelsAsANumberAndOnAnUnavailableObservationToo) {
    PlaylistEntryObservation observation;
    observation.identity.instanceUuid = "M4-7840e12f81da4191c0d00fbb6a889314";
    observation.identity.playlistName = "Halloween";
    observation.action = PlaylistAction::kPlaying;
    observation.sequence = 7;
    observation.unavailable = IdentityUnavailable::kMissingDefinition;
    observation.playlistLoop = 3;

    PayloadResult result = buildObservationBody(observation);
    CHECK(result.ok);
    // A number, never a string, and present even with no identity: the
    // pass counter is corroborating evidence, not identity, so an
    // unavailable observation does not have to withhold it.
    CHECK(contains(result.body, "\"playlistLoop\":3"));
    CHECK(!contains(result.body, "\"playlistLoop\":\""));
}

TEST(PlaylistLoopIsNotAnInputToTheEntryKey) {
    // Contract section 1.8: corroborating evidence, never identity. If the
    // pass counter ever reached deriveEntryKey, a loop's second visit
    // would derive a different key and the coordinator's entry-key term
    // would fire on its own, making the whole playlistLoop term dead code
    // that still looked like it worked.
    const std::string definition = "{\"mainPlaylist\":[{\"type\":\"sequence\"}],\"loop\":4}";
    showmesh::IdentityResolution first =
        resolveEntryIdentity("M4-7840e12f81da4191c0d00fbb6a889314", "Halloween", definition, "", 0);
    CHECK(first.ok);

    PlaylistEntryObservation a;
    a.identity = first.identity;
    a.entryKey = first.entryKey;
    a.action = PlaylistAction::kPlaying;
    a.sequence = 1;
    a.playlistLoop = 0;

    PlaylistEntryObservation b = a;
    b.sequence = 2;
    b.playlistLoop = 1;

    CHECK_EQ(a.entryKey, b.entryKey);
    PayloadResult ra = buildObservationBody(a);
    PayloadResult rb = buildObservationBody(b);
    CHECK(ra.ok);
    CHECK(rb.ok);
    CHECK(contains(ra.body, "\"entryKey\":\"" + a.entryKey + "\""));
    CHECK(contains(rb.body, "\"entryKey\":\"" + a.entryKey + "\""));
}
