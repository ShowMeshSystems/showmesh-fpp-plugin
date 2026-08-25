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

// An edited playlist: the first item's content changed. Note that both
// items of kDefinition above are byte-identical to each other, so this is
// not evidence that reordering is detected, only that a content change is.
const char* kDefinitionEdited =
    "{\"name\":\"Main Show\",\"repeat\":0,"
    "\"mainPlaylist\":["
    "{\"type\":\"both\",\"sequenceName\":\"b.fseq\",\"mediaName\":\"other.mp3\",\"enabled\":1},"
    "{\"type\":\"both\",\"sequenceName\":\"a.fseq\",\"mediaName\":\"song.mp3\",\"enabled\":1}"
    "]}";

// Two items that genuinely differ from each other, so swapping their order
// below is an actual reorder and not a no-op on identical content.
const char* kDefinitionDistinctItems =
    "{\"name\":\"Main Show\",\"repeat\":0,"
    "\"mainPlaylist\":["
    "{\"type\":\"both\",\"sequenceName\":\"a.fseq\",\"mediaName\":\"first.mp3\",\"enabled\":1},"
    "{\"type\":\"both\",\"sequenceName\":\"b.fseq\",\"mediaName\":\"second.mp3\",\"enabled\":1}"
    "]}";

// kDefinitionDistinctItems with its two items swapped, content otherwise
// unchanged.
const char* kDefinitionDistinctItemsReordered =
    "{\"name\":\"Main Show\",\"repeat\":0,"
    "\"mainPlaylist\":["
    "{\"type\":\"both\",\"sequenceName\":\"b.fseq\",\"mediaName\":\"second.mp3\",\"enabled\":1},"
    "{\"type\":\"both\",\"sequenceName\":\"a.fseq\",\"mediaName\":\"first.mp3\",\"enabled\":1}"
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

    // The edit case above changes item content, not order: kDefinition's
    // two items are byte-identical to each other, so reordering them is a
    // no-op regardless of whether canonicalization is order-sensitive.
    // Exercise an actual reorder, with two items that differ from each
    // other, so an array canonicalizer that dropped order-sensitivity
    // (for example by sorting items) would still fail this.
    IdentityResolution originalOrder =
        resolveEntryIdentity(kUuid, "Main Show", kDefinitionDistinctItems, "mainPlaylist", 0);
    IdentityResolution reordered =
        resolveEntryIdentity(kUuid, "Main Show", kDefinitionDistinctItemsReordered, "mainPlaylist", 0);
    CHECK(originalOrder.ok);
    CHECK(reordered.ok);
    CHECK_NE(originalOrder.identity.playlistHash, reordered.identity.playlistHash);
    CHECK_NE(originalOrder.entryKey, reordered.entryKey);
}

TEST(DuplicateFilenamesAtDifferentPositionsProduceDistinctKeys) {
    IdentityResolution first = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 0);
    IdentityResolution second = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 1);
    CHECK(first.ok);
    CHECK(second.ok);
    CHECK_EQ(first.identity.playlistHash, second.identity.playlistHash);
    CHECK_NE(first.entryKey, second.entryKey);
}

// Pinned independently of this process: the entry key for this exact
// identity, computed once and frozen here. Comparing two same-process
// resolutions against each other is not evidence of cross-process
// determinism; a key that silently folded in something process-local (a
// pid, a memory address, an unseeded random value) would still agree with
// itself within one process. Comparing against a pinned constant instead
// catches that.
const char* kDeterministicEntryKeyForPosition3 = "eaeb24db852ed6b70d6aa39d4979fee9315126418b62c9d44e62127be0fa1327";

TEST(TheEntryKeyIsDeterministicAcrossProcesses) {
    IdentityResolution a = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 3);
    IdentityResolution b = resolveEntryIdentity(kUuid, "Main Show", kDefinition, "mainPlaylist", 3);
    CHECK_EQ(a.entryKey, b.entryKey);
    CHECK_EQ(a.entryKey, std::string(kDeterministicEntryKeyForPosition3));
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

// Pinned independently of this process: the entry key for this exact
// identity, computed once and frozen here. Comparing `before` and `after`
// against each other is not evidence of surviving a restart; both are
// still computed in this one process, so a key that silently became
// process-local (a pid folded in, for example) would still agree with
// itself here. Comparing both against a pinned constant catches that.
const char* kRestartStableEntryKeyForPosition2 = "efa981636b7d94004b3330a6ce0abbad26c68e0f2977b6a7e3134daa14ae29fe";

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

    CHECK_EQ(before.entryKey, std::string(kRestartStableEntryKeyForPosition2));
    CHECK_EQ(after.entryKey, before.entryKey);
    CHECK_EQ(after.entryKey, std::string(kRestartStableEntryKeyForPosition2));
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

// finding 6: FPP writes "Unknown" (capital U, settings.cpp on both FPP 9
// and FPP 10) as the SystemUUID default, not "unknown". A guard that only
// recognized the lowercase spelling let every un-provisioned host's real
// SystemUUID value through as if it were a real, persistent identity, and
// every such host would then share the same one.
TEST(BothCasingsOfFppsUnprovisionedSystemUuidAreRecognized) {
    CHECK(showmesh::isUnprovisionedInstanceUuid("Unknown"));
    CHECK(showmesh::isUnprovisionedInstanceUuid("unknown"));
    CHECK(showmesh::isUnprovisionedInstanceUuid("UNKNOWN"));
    CHECK(!showmesh::isUnprovisionedInstanceUuid(kUuid));
    CHECK(!showmesh::isUnprovisionedInstanceUuid(""));
    CHECK(!showmesh::isUnprovisionedInstanceUuid("unknowns"));
}
