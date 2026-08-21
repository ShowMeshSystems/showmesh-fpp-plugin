#include "showmesh/playlist_identity.h"

#include <set>
#include <string>

#include "check.h"
#include "showmesh/callback_handoff.h"
#include "showmesh/sha256.h"

using showmesh::CallbackEvidence;
using showmesh::CallbackHandoff;
using showmesh::deriveEntryKey;
using showmesh::EntryIdentity;
using showmesh::IdentityResolution;
using showmesh::IdentityUnavailable;
using showmesh::PlaylistAction;
using showmesh::resolveEntryIdentity;
using showmesh::SequenceState;

namespace {

const char* kUuid = "6f1c1a52-1b6c-4b53-9a0e-9f7c2f0d1b44";

const char* kDefinition =
    "{\"name\":\"Main Show\",\"repeat\":0,"
    "\"mainPlaylist\":["
    "{\"type\":\"both\",\"sequenceName\":\"a.fseq\",\"mediaName\":\"song.mp3\",\"enabled\":1},"
    "{\"type\":\"both\",\"sequenceName\":\"a.fseq\",\"mediaName\":\"song.mp3\",\"enabled\":1}"
    "]}";

// The same definition with its members reordered and reformatted. A
// canonical hash must not see it as a different playlist.
const char* kDefinitionReordered =
    "{\n  \"repeat\": 0,\n  \"mainPlaylist\": [\n"
    "    {\"mediaName\": \"song.mp3\", \"enabled\": 1, \"sequenceName\": \"a.fseq\", \"type\": \"both\"},\n"
    "    {\"enabled\": 1, \"type\": \"both\", \"mediaName\": \"song.mp3\", \"sequenceName\": \"a.fseq\"}\n"
    "  ],\n  \"name\": \"Main Show\"\n}";

// A genuinely different playlist: the two items are reordered.
const char* kDefinitionEdited =
    "{\"name\":\"Main Show\",\"repeat\":0,"
    "\"mainPlaylist\":["
    "{\"type\":\"both\",\"sequenceName\":\"b.fseq\",\"mediaName\":\"other.mp3\",\"enabled\":1},"
    "{\"type\":\"both\",\"sequenceName\":\"a.fseq\",\"mediaName\":\"song.mp3\",\"enabled\":1}"
    "]}";

}  // namespace

TEST(TheCanonicalHashIgnoresFormattingAndMemberOrder) {
    IdentityResolution a = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 0);
    IdentityResolution b = resolveEntryIdentity(kUuid, "Main Show", kDefinitionReordered, "mainPlaylist", 0);
    CHECK(a.ok);
    CHECK(b.ok);
    CHECK_EQ(a.identity.playlistHash, b.identity.playlistHash);
    CHECK_EQ(a.entryKey, b.entryKey);
    CHECK_EQ(a.canonicalDefinition, b.canonicalDefinition);
    CHECK_EQ(a.identity.playlistHash, showmesh::sha256Hex(a.canonicalDefinition));
}

TEST(EditingOrReorderingThePlaylistChangesTheHashAndInvalidatesTheOldKey) {
    IdentityResolution before = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 0);
    IdentityResolution after = resolveEntryIdentity(kUuid, "Main Show", kDefinitionEdited, "mainPlaylist", 0);
    CHECK(before.ok);
    CHECK(after.ok);
    CHECK_NE(before.identity.playlistHash, after.identity.playlistHash);
    CHECK_NE(before.entryKey, after.entryKey);
}

TEST(DuplicateFilenamesAtDifferentPositionsProduceDistinctKeys) {
    IdentityResolution first = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 0);
    IdentityResolution second = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 1);
    CHECK(first.ok);
    CHECK(second.ok);
    CHECK_EQ(first.identity.playlistHash, second.identity.playlistHash);
    CHECK_NE(first.entryKey, second.entryKey);
}

TEST(TheEntryKeyIsDeterministicAcrossProcesses) {
    IdentityResolution a = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 3);
    IdentityResolution b = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 3);
    CHECK_EQ(a.entryKey, b.entryKey);
    CHECK_EQ(a.entryKey.size(), static_cast<std::size_t>(64));
}

// A name containing the characters a delimited key format would use as
// separators must not collide with a different entry. This is why the key
// hashes a canonical JSON object rather than a joined string.
TEST(SeparatorCharactersInNamesDoNotCollide) {
    EntryIdentity a;
    a.instanceUuid = kUuid;
    a.playlistName = "show:1";
    a.playlistHash = "aa";
    a.section = "main";
    a.position = 0;

    EntryIdentity b = a;
    b.playlistName = "show";
    b.section = "1|main";

    CHECK_NE(deriveEntryKey(a), deriveEntryKey(b));
}

TEST(EveryDistinctSectionAndPositionGetsItsOwnKey) {
    std::set<std::string> keys;
    for (const char* section : {"mainPlaylist", "leadIn", "leadOut"}) {
        for (int position = 0; position < 4; ++position) {
            IdentityResolution r = resolveEntryIdentity(kUuid, "Main Show", kDefinition, section, position);
            CHECK(r.ok);
            keys.insert(r.entryKey);
        }
    }
    CHECK_EQ(keys.size(), static_cast<std::size_t>(12));
}

TEST(MissingEvidenceProducesAnExplicitUnavailableReasonNotFilenameIdentity) {
    CHECK(resolveEntryIdentity("", "Main Show", kDefinition, "mainPlaylist", 0).reason ==
          IdentityUnavailable::kMissingInstanceUuid);
    CHECK(resolveEntryIdentity(kUuid, "", kDefinition, "mainPlaylist", 0).reason ==
          IdentityUnavailable::kMissingPlaylistName);
    CHECK(resolveEntryIdentity(kUuid, "Main Show", "", "mainPlaylist", 0).reason ==
          IdentityUnavailable::kMissingDefinition);
    CHECK(resolveEntryIdentity(kUuid, "Main Show", "{not json", "mainPlaylist", 0).reason ==
          IdentityUnavailable::kUnsupportedDefinitionShape);
    CHECK(resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", -1).reason ==
          IdentityUnavailable::kNegativePosition);

    IdentityResolution unavailable = resolveEntryIdentity(kUuid, "Main Show", "{not json", "mainPlaylist", 0);
    CHECK(!unavailable.ok);
    CHECK(unavailable.entryKey.empty());
    CHECK(unavailable.identity.playlistHash.empty());
}

TEST(TheSequenceOnlyEverMovesForward) {
    SequenceState seq;
    CHECK_EQ(seq.next(), static_cast<std::uint64_t>(1));
    CHECK_EQ(seq.next(), static_cast<std::uint64_t>(2));

    seq.restore(1);  // an older persisted value must not rewind it
    CHECK_EQ(seq.current(), static_cast<std::uint64_t>(2));
    CHECK_EQ(seq.next(), static_cast<std::uint64_t>(3));

    seq.restore(100);
    CHECK_EQ(seq.next(), static_cast<std::uint64_t>(101));
}

TEST(AnUnchangedPlaylistKeepsItsIdentityAcrossARestart) {
    IdentityResolution before = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 2);
    SequenceState beforeSeq;
    beforeSeq.next();
    beforeSeq.next();
    const std::uint64_t persisted = beforeSeq.current();

    // A fresh process restoring the persisted sequence continues from it,
    // and the same definition still resolves to the same entry key.
    SequenceState afterSeq;
    afterSeq.restore(persisted);
    IdentityResolution after = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 2);

    CHECK_EQ(after.entryKey, before.entryKey);
    CHECK_EQ(afterSeq.next(), persisted + 1);
}

TEST(TheCallbackHandoffKeepsTheNewestStateAndCountsWhatItDropped) {
    CallbackHandoff handoff(2);
    for (int position = 0; position < 5; ++position) {
        CallbackEvidence e;
        e.setPlaylistName("Main Show");
        e.setSection("mainPlaylist");
        e.position = position;
        e.action = PlaylistAction::kPlaying;
        e.observedAtMillis = 1000 + position;
        handoff.offer(e);
    }
    CHECK_EQ(handoff.pending(), static_cast<std::size_t>(2));
    CHECK_EQ(handoff.offeredTotal(), static_cast<std::uint64_t>(5));

    CallbackEvidence taken;
    std::uint32_t coalesced = 0;
    CHECK(handoff.take(&taken, &coalesced));
    CHECK_EQ(taken.position, 3);
    CHECK_EQ(coalesced, static_cast<std::uint32_t>(3));

    // The gap count is reported once and then cleared; the next take is
    // not a second report of the same gap.
    CHECK(handoff.take(&taken, &coalesced));
    CHECK_EQ(taken.position, 4);
    CHECK_EQ(coalesced, static_cast<std::uint32_t>(0));

    CHECK(!handoff.take(&taken, &coalesced));
    CHECK_EQ(coalesced, static_cast<std::uint32_t>(0));
}

TEST(RepeatedPlayingWithANewPositionIsANewEntry) {
    CallbackEvidence first;
    first.setPlaylistName("Main Show");
    first.setSection("mainPlaylist");
    first.position = 0;
    first.action = PlaylistAction::kPlaying;

    CallbackEvidence sameEntryDifferentAction = first;
    sameEntryDifferentAction.action = PlaylistAction::kStart;
    CHECK(first.sameEntryAs(sameEntryDifferentAction));

    CallbackEvidence advanced = first;
    advanced.position = 1;
    CHECK(!first.sameEntryAs(advanced));

    CallbackEvidence otherSection = first;
    otherSection.setSection("leadOut");
    CHECK(!first.sameEntryAs(otherSection));
}

TEST(CallbackFieldsAreTruncatedRatherThanAllocated) {
    CallbackEvidence e;
    const std::string tooLong(showmesh::kMaxPlaylistNameLength + 50, 'n');
    e.setPlaylistName(tooLong.c_str());
    CHECK_EQ(std::string(e.playlistName).size(), showmesh::kMaxPlaylistNameLength);

    e.setMediaFilename(nullptr);
    CHECK_EQ(std::string(e.mediaFilename), std::string(""));
}

TEST(ActionNamesRoundTrip) {
    for (PlaylistAction action :
         {PlaylistAction::kStart, PlaylistAction::kPlaying, PlaylistAction::kStop, PlaylistAction::kQueryNext}) {
        CHECK(showmesh::playlistActionFromName(showmesh::playlistActionName(action)) == action);
    }
    CHECK(showmesh::playlistActionFromName("something else") == PlaylistAction::kUnknown);
}
