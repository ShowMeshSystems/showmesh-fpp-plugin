// Unit tests for the FPP-facing edges of the adapter boundary: the
// runtime-to-canonical section name mapping (section_names.h) and the
// currentEntry filename extraction (callback_fields.h). These two headers
// are the only adapter-shared code that needs nothing but jsoncpp, so they
// can be exercised here without an installed FPP source tree.
//
// SM-275: before this change, the plugin forwarded FPP's runtime section
// spelling ("LeadIn"/"MainPlaylist"/"LeadOut") straight through, which
// never matched the coordinator's definition-derived entry key spelling
// ("leadIn"/"mainPlaylist"/"leadOut"), and it read sequence/media
// filenames from a "playlist[section]" array that does not exist on FPP
// 10's callback object. Both are exercised below against the frozen
// coordinator fixture and against FPP 10's actual GetInfo() shape.

#include <optional>
#include <string>

#include "callback_fields.h"
#include "check.h"
#include "section_names.h"
#include "showmesh/playlist_identity.h"

using showmesh::EntryIdentity;
using showmesh::deriveEntryKey;
using showmesh::adapter::canonicalPlaylistSection;
using showmesh::adapter::mediaFilenameOf;
using showmesh::adapter::playlistLoopOf;
using showmesh::adapter::sequenceFilenameOf;

namespace {

// test/fixtures/fpp/entry-key.json "baseline" case, vendored verbatim from
// the coordinator repository. If this stops matching, the coordinator and
// this plugin have drifted on the entry-key contract.
constexpr const char* kBaselineEntryKey = "4412de28018bd7bd4b20df96341da9737a24ec604d88c2b2372a8a068f55e591";

}  // namespace

// --- section name mapping -------------------------------------------------

TEST(RuntimeSpellingMapsToCanonical) {
    CHECK_EQ(canonicalPlaylistSection("LeadIn"), std::string("leadIn"));
    CHECK_EQ(canonicalPlaylistSection("MainPlaylist"), std::string("mainPlaylist"));
    CHECK_EQ(canonicalPlaylistSection("LeadOut"), std::string("leadOut"));
}

TEST(UnknownSectionPassesThroughUnchanged) {
    CHECK_EQ(canonicalPlaylistSection("New"), std::string("New"));
    CHECK_EQ(canonicalPlaylistSection(""), std::string(""));
    CHECK_EQ(canonicalPlaylistSection("mainPlaylist"), std::string("mainPlaylist"));
}

TEST(CanonicalSectionProducesFixtureEntryKey) {
    EntryIdentity identity;
    identity.instanceUuid = "6f1c1a52-1b6c-4b53-9a0e-9f7c2f0d1b44";
    identity.playlistHash = "deadbeef";
    identity.playlistName = "Main Show";
    identity.position = 0;
    identity.section = canonicalPlaylistSection("MainPlaylist");

    CHECK_EQ(deriveEntryKey(identity), std::string(kBaselineEntryKey));
}

// --- currentEntry filename extraction -------------------------------------

TEST(SequenceEntryFilenameComesFromCurrentEntry) {
    Json::Value playlist;
    playlist["currentState"] = "playing";
    playlist["currentEntry"]["type"] = "sequence";
    playlist["currentEntry"]["sequenceName"] = "Lane14-One.fseq";

    CHECK_EQ(sequenceFilenameOf(playlist), std::string("Lane14-One.fseq"));
    CHECK_EQ(mediaFilenameOf(playlist), std::string(""));
}

TEST(MediaEntryFilenameComesFromCurrentEntry) {
    Json::Value playlist;
    playlist["currentState"] = "playing";
    playlist["currentEntry"]["type"] = "media";
    playlist["currentEntry"]["mediaFilename"] = "Lane14-Intro.mp4";

    CHECK_EQ(mediaFilenameOf(playlist), std::string("Lane14-Intro.mp4"));
    CHECK_EQ(sequenceFilenameOf(playlist), std::string(""));
}

TEST(IdleCallbackWithNoCurrentEntryYieldsNoFilename) {
    // Playlist::GetInfo() sets result["currentEntry"] = GetCurrentEntry(),
    // and GetCurrentEntry() returns a default-constructed (null,
    // non-object) Json::Value while FPP_STATUS_IDLE. currentEntryOf() must
    // treat that the same as "absent" rather than crash or fabricate a
    // filename.
    Json::Value playlist;
    playlist["currentState"] = "idle";
    playlist["currentEntry"] = Json::Value();

    CHECK_EQ(sequenceFilenameOf(playlist), std::string(""));
    CHECK_EQ(mediaFilenameOf(playlist), std::string(""));
}

TEST(MissingCurrentEntryMemberYieldsNoFilename) {
    Json::Value playlist;
    playlist["currentState"] = "idle";

    CHECK_EQ(sequenceFilenameOf(playlist), std::string(""));
    CHECK_EQ(mediaFilenameOf(playlist), std::string(""));
}

// --- mainPlaylist pass counter --------------------------------------------

TEST(PlaylistLoopReadsLoopAndNeverLoopCount) {
    // This is the whole trap. Playlist::GetInfo() writes both members, and
    // the names invite reading the wrong one: `loop` is the running pass
    // counter (m_loop, incremented in Process()) and `loopCount` is the
    // configured repeat LIMIT it is compared against (m_loopCount, read
    // from the playlist config). Verified identical on FPP 9.5.3 and
    // 10.0. Reading loopCount would report a fixed limit as a lap number,
    // and for the common unlimited-repeat playlist it is 0 on every tick,
    // so the coordinator would never see it change and the loop re-entry
    // this field exists for would still be invisible.
    Json::Value playlist;
    playlist["currentState"] = "playing";
    playlist["loop"] = 2;
    playlist["loopCount"] = 0;  // unlimited repeat, the common show setting

    const std::optional<int> loop = playlistLoopOf(playlist);
    CHECK(loop.has_value());
    CHECK_EQ(*loop, 2);
}

TEST(PlaylistLoopZeroOnAPlayingPlaylistIsARealFirstPass) {
    Json::Value playlist;
    playlist["currentState"] = "playing";
    playlist["loop"] = 0;

    const std::optional<int> loop = playlistLoopOf(playlist);
    CHECK(loop.has_value());
    CHECK_EQ(*loop, 0);
}

TEST(PlaylistLoopIsAbsentWhileIdle) {
    // GetInfo()'s idle branch writes result["loop"] = 0 unconditionally.
    // That 0 means "no playlist is running", not "the running playlist is
    // on its first pass", and the two must not travel as the same value.
    Json::Value playlist;
    playlist["currentState"] = "idle";
    playlist["loop"] = 0;

    CHECK(!playlistLoopOf(playlist).has_value());
}

TEST(PlaylistLoopIsAbsentWhenTheMemberIsMissingOrNotAnInteger) {
    Json::Value missing;
    missing["currentState"] = "playing";
    CHECK(!playlistLoopOf(missing).has_value());

    Json::Value wrongType;
    wrongType["currentState"] = "playing";
    wrongType["loop"] = "2";
    CHECK(!playlistLoopOf(wrongType).has_value());

    CHECK(!playlistLoopOf(Json::Value()).has_value());
}
