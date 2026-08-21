#include "showmesh/runtime.h"

#include <string>
#include <vector>

#include "check.h"

using showmesh::CommandOutcome;
using showmesh::ObservationSink;
using showmesh::PlaylistDefinitionSource;
using showmesh::PlaylistEntryObservation;
using showmesh::ShowMeshRuntime;
using showmesh::StateAdoption;
using showmesh::TimeMillis;

namespace {

TimeMillis gNow = 1'800'000'000'000;
TimeMillis testClock() { return gNow; }

const char* kUuid = "6f1c1a52-1b6c-4b53-9a0e-9f7c2f0d1b44";
const char* kDefinition = "{\"name\":\"Main Show\",\"mainPlaylist\":[{\"sequenceName\":\"a.fseq\"}]}";

class FakeDefinitions : public PlaylistDefinitionSource {
 public:
    std::string definitionFor(const std::string& playlistName) override {
        ++definitionCalls;
        lastRequested = playlistName;
        return definition;
    }
    std::string instanceUuid() override { return uuid; }

    std::string definition = kDefinition;
    std::string uuid = kUuid;
    std::string lastRequested;
    int definitionCalls = 0;
};

class RecordingSink : public ObservationSink {
 public:
    void publish(const PlaylistEntryObservation& observation) override { published.push_back(observation); }
    void publishUnavailable(const PlaylistEntryObservation& observation) override {
        unavailable.push_back(observation);
    }

    std::vector<PlaylistEntryObservation> published;
    std::vector<PlaylistEntryObservation> unavailable;
};

}  // namespace

TEST(TheRegisteredActionParsesAndValidatesItsArguments) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    CHECK(runtime.applyBrightnessCommand("75", "0").ok);
    CHECK_EQ(runtime.brightness().effectivePercentAt(gNow), 75);

    // Every way an operator can get the arguments wrong is refused, and
    // refusing changes nothing.
    for (const char* percent : {"", "  ", "abc", "75.5", "-1", "101", "75x", "7 5", "0x4b"}) {
        const CommandOutcome outcome = runtime.applyBrightnessCommand(percent, "0");
        if (outcome.ok) {
            ::showmesh_test::reportFailure(__FILE__, __LINE__,
                                           std::string("target percent ") + percent + " was accepted");
        }
        CHECK(!outcome.message.empty());
    }
    for (const char* seconds : {"", "abc", "1.5", "-1", "86401"}) {
        const CommandOutcome outcome = runtime.applyBrightnessCommand("50", seconds);
        if (outcome.ok) {
            ::showmesh_test::reportFailure(__FILE__, __LINE__,
                                           std::string("fade seconds ") + seconds + " was accepted");
        }
    }
    CHECK_EQ(runtime.brightness().effectivePercentAt(gNow), 75);

    CHECK(runtime.applyBrightnessCommand("0", "86400").ok);
    CHECK(runtime.applyBrightnessCommand("100", "0").ok);
    // Surrounding whitespace is formatting, not value.
    CHECK(runtime.applyBrightnessCommand(" 60 ", " 5 ").ok);
}

TEST(TheCallbackHandsOffAndTheWorkerResolvesIdentity) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    runtime.observeCallback("Main Show", "start", "mainPlaylist", 0, "a.fseq", "song.mp3");
    // Nothing resolved on the callback thread: no definition was fetched.
    CHECK_EQ(definitions.definitionCalls, 0);
    CHECK_EQ(runtime.handoff().pending(), static_cast<std::size_t>(1));

    CHECK(runtime.drainOnce());
    CHECK_EQ(definitions.definitionCalls, 1);
    CHECK_EQ(definitions.lastRequested, std::string("Main Show"));
    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(sink.unavailable.size(), static_cast<std::size_t>(0));

    const PlaylistEntryObservation& o = sink.published[0];
    CHECK_EQ(o.schemaVersion, showmesh::kObservationSchemaVersion);
    CHECK_EQ(o.identity.instanceUuid, std::string(kUuid));
    CHECK_EQ(o.identity.playlistName, std::string("Main Show"));
    CHECK_EQ(o.identity.section, std::string("mainPlaylist"));
    CHECK_EQ(o.identity.position, 0);
    CHECK_EQ(o.sequenceFilename, std::string("a.fseq"));
    CHECK_EQ(o.mediaFilename, std::string("song.mp3"));
    CHECK(o.action == showmesh::PlaylistAction::kStart);
    CHECK_EQ(o.sequence, static_cast<std::uint64_t>(1));
    CHECK_EQ(o.entryKey.size(), static_cast<std::size_t>(64));
    CHECK(o.unavailable == showmesh::IdentityUnavailable::kNone);

    CHECK(!runtime.drainOnce());
}

TEST(TheSequenceIsMonotonicAcrossObservations) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    for (int i = 0; i < 5; ++i) {
        runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "a.fseq", "");
        CHECK(runtime.drainOnce());
    }
    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(5));
    for (std::size_t i = 0; i < sink.published.size(); ++i) {
        CHECK_EQ(sink.published[i].sequence, static_cast<std::uint64_t>(i + 1));
    }
}

TEST(AMissingDefinitionProducesAnUnavailableObservationNotAGuess) {
    FakeDefinitions definitions;
    definitions.definition = "";
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    runtime.observeCallback("Main Show", "playing", "mainPlaylist", 2, "a.fseq", "song.mp3");
    CHECK(runtime.drainOnce());

    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(0));
    CHECK_EQ(sink.unavailable.size(), static_cast<std::size_t>(1));
    const PlaylistEntryObservation& o = sink.unavailable[0];
    CHECK(o.unavailable == showmesh::IdentityUnavailable::kMissingDefinition);
    CHECK(o.entryKey.empty());
    CHECK(o.identity.playlistHash.empty());
    // The filenames are still carried as corroborating evidence, but they
    // are not identity.
    CHECK_EQ(o.sequenceFilename, std::string("a.fseq"));
}

TEST(AMissingInstanceUuidProducesAnUnavailableObservation) {
    FakeDefinitions definitions;
    definitions.uuid = "";
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    runtime.observeCallback("Main Show", "start", "mainPlaylist", 0, "", "");
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.unavailable.size(), static_cast<std::size_t>(1));
    CHECK(sink.unavailable[0].unavailable == showmesh::IdentityUnavailable::kMissingInstanceUuid);
}

// The gap belongs to whoever accepts it. An unavailable observation
// acknowledges nothing, so the coalesced count must still be riding on the
// next observation that does get published.
TEST(TheCoalescedGapIsCarriedUntilSomethingAcceptsIt) {
    FakeDefinitions definitions;
    definitions.definition = "";
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    for (int i = 0; i < 40; ++i) {
        runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "", "");
    }
    // The handoff holds 16; the rest were dropped and counted.
    CHECK_EQ(runtime.handoff().pending(), static_cast<std::size_t>(16));

    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.unavailable.size(), static_cast<std::size_t>(1));
    const std::uint32_t firstGap = sink.unavailable[0].coalescedSincePreviousAcknowledged;
    CHECK_EQ(firstGap, static_cast<std::uint32_t>(24));

    // Still unacknowledged, so the next one reports at least as much.
    CHECK(runtime.drainOnce());
    CHECK(sink.unavailable[1].coalescedSincePreviousAcknowledged >= firstGap);

    // Once identity resolves and a publish succeeds, the gap is settled.
    definitions.definition = kDefinition;
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(1));
    CHECK(sink.published[0].coalescedSincePreviousAcknowledged >= firstGap);

    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.published[1].coalescedSincePreviousAcknowledged, static_cast<std::uint32_t>(0));
}

TEST(FullStateSurvivesTheMultiSyncEncoding) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime leader(&definitions, &sink, testClock);
    CHECK(leader.applyBrightnessCommand("100", "0").ok);
    CHECK(leader.applyBrightnessCommand("20", "100").ok);

    const std::string payload = leader.encodeFullState();
    CHECK(!payload.empty());

    ShowMeshRuntime joiner(&definitions, &sink, testClock);
    CHECK(joiner.adoptEncodedFullState(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                       static_cast<int>(payload.size())) == StateAdoption::kAdopted);
    CHECK_NEAR(joiner.brightness().ceilingAt(gNow + 50'000), leader.brightness().ceilingAt(gNow + 50'000), 1e-9);

    // Replaying the identical payload is inert.
    CHECK(joiner.adoptEncodedFullState(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                       static_cast<int>(payload.size())) == StateAdoption::kRejectedStaleRevision);

    // Garbage from the wire is refused, not guessed at.
    const std::string garbage = "not a payload";
    CHECK(joiner.adoptEncodedFullState(reinterpret_cast<const std::uint8_t*>(garbage.data()),
                                       static_cast<int>(garbage.size())) == StateAdoption::kRejectedUnsupportedVersion);
    CHECK(joiner.adoptEncodedFullState(nullptr, 0) == StateAdoption::kRejectedUnsupportedVersion);
}

TEST(TheWorkerThreadDrainsWhatTheCallbackOffers) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);
    runtime.start();

    for (int i = 0; i < 8; ++i) {
        runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "a.fseq", "");
    }

    // stop() joins the worker, so everything the worker was going to do is
    // done by the time it returns.
    runtime.stop();
    while (runtime.drainOnce()) {
    }
    CHECK_EQ(runtime.publishedCount(), static_cast<std::uint64_t>(8));
    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(8));

    // Stopping twice is not an error.
    runtime.stop();
}

TEST(AnAbsentSinkIsNotACrash) {
    FakeDefinitions definitions;
    ShowMeshRuntime runtime(&definitions, nullptr, testClock);
    runtime.observeCallback("Main Show", "start", "mainPlaylist", 0, "", "");
    CHECK(runtime.drainOnce());
    CHECK_EQ(runtime.publishedCount(), static_cast<std::uint64_t>(1));
}

// The adapters interpolate the playlist name into a path under FPP's
// playlist directory, so a name that could climb out of it is refused
// rather than cleaned up.
TEST(APlaylistNameThatCouldEscapeItsDirectoryIsRefused) {
    for (const char* safe : {"Main Show", "main-show", "Show 2026", "a", "show.with.dots", "Act 1..Act 2"}) {
        if (!showmesh::playlistNameIsPathSafe(safe)) {
            ::showmesh_test::reportFailure(__FILE__, __LINE__, std::string("refused a safe name: ") + safe);
        }
    }
    for (const char* unsafe : {"", "../secrets", "..", ".hidden", "a/b", "a\\b", "sub/../../etc/passwd",
                               "name\nwith-newline"}) {
        if (showmesh::playlistNameIsPathSafe(unsafe)) {
            ::showmesh_test::reportFailure(__FILE__, __LINE__, std::string("accepted an unsafe name: ") + unsafe);
        }
    }
}
