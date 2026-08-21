#include "showmesh/runtime.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
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
    bool publish(const PlaylistEntryObservation& observation) override {
        published.push_back(observation);
        return acceptPublish;
    }
    bool publishUnavailable(const PlaylistEntryObservation& observation) override {
        unavailable.push_back(observation);
        return acceptUnavailable;
    }

    std::vector<PlaylistEntryObservation> published;
    std::vector<PlaylistEntryObservation> unavailable;
    bool acceptPublish = true;
    bool acceptUnavailable = true;
};

}  // namespace

TEST(TheRegisteredActionParsesAndValidatesItsArguments) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    CHECK(runtime.applyBrightnessCommand("75", "0").ok);
    CHECK_EQ(runtime.brightness()->effectivePercentAt(gNow), 75);

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
    CHECK_EQ(runtime.brightness()->effectivePercentAt(gNow), 75);

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

// A field that determines identity, truncated on the callback thread,
// must never resolve to identity: two different long names sharing a
// 255-byte prefix would otherwise collide on the same entry key.
TEST(ATruncatedPlaylistNameProducesAnUnavailableObservationNotAnIdentity) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    const std::string longName(300, 'a');
    runtime.observeCallback(longName.c_str(), "start", "mainPlaylist", 0, "", "");
    CHECK(runtime.drainOnce());

    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(0));
    CHECK_EQ(sink.unavailable.size(), static_cast<std::size_t>(1));
    CHECK(sink.unavailable[0].unavailable == showmesh::IdentityUnavailable::kTruncatedIdentityField);
    // The definition was never fetched with the truncated (and therefore
    // wrong) name.
    CHECK_EQ(definitions.definitionCalls, 0);
}

TEST(ATruncatedSectionProducesAnUnavailableObservation) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    const std::string longSection(100, 'b');
    runtime.observeCallback("Main Show", "start", longSection.c_str(), 0, "", "");
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.unavailable.size(), static_cast<std::size_t>(1));
    CHECK(sink.unavailable[0].unavailable == showmesh::IdentityUnavailable::kTruncatedIdentityField);
}

// The gap belongs to whoever accepts it. A REJECTED delivery, of either
// kind, acknowledges nothing, so the coalesced count must still be riding
// on the next observation.
TEST(ARejectedDeliveryOfEitherKindCarriesTheGapForward) {
    FakeDefinitions definitions;
    definitions.definition = "";
    RecordingSink sink;
    sink.acceptUnavailable = false;
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

    // Rejected, so the next one still reports at least as much.
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

// finding 7: an unavailable observation is still an observation the
// coordinator can acknowledge. publishUnavailable()'s return value was
// previously discarded, so the same gap was re-reported forever
// regardless of what the sink returned. Accepting it must clear the gap
// exactly as an accepted publish does.
TEST(AnAcceptedUnavailableObservationClearsTheGap) {
    FakeDefinitions definitions;
    definitions.definition = "";
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    for (int i = 0; i < 40; ++i) {
        runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "", "");
    }
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.unavailable[0].coalescedSincePreviousAcknowledged, static_cast<std::uint32_t>(24));

    // Accepted (RecordingSink::acceptUnavailable defaults true): the very
    // next observation reports zero rather than carrying 24 forward.
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.unavailable[1].coalescedSincePreviousAcknowledged, static_cast<std::uint32_t>(0));
}

// A sink that refuses a publish must not be credited with one, and the
// gap it failed to acknowledge must still be riding on the next attempt.
TEST(ARejectedPublishDoesNotCountAsPublishedOrClearTheGap) {
    FakeDefinitions definitions;
    RecordingSink sink;
    sink.acceptPublish = false;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    for (int i = 0; i < 20; ++i) {
        runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "a.fseq", "");
    }
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(runtime.publishedCount(), static_cast<std::uint64_t>(0));

    sink.acceptPublish = true;
    CHECK(runtime.drainOnce());
    CHECK_EQ(runtime.publishedCount(), static_cast<std::uint64_t>(1));
    // The gap from the rejected attempt rode forward onto the accepted one.
    CHECK(sink.published[1].coalescedSincePreviousAcknowledged > 0);
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
    CHECK_NEAR(joiner.brightness()->ceilingAt(gNow + 50'000), leader.brightness()->ceilingAt(gNow + 50'000), 1e-9);

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

// The lost-wakeup regression for finding 10 (and finding 8 of the
// follow-up review: the previous version of this test passed with the
// fix fully reverted, ten runs out of ten, because a 20ms sleep only
// makes it *likely* the worker is parked in wait_for, never guarantees
// it). setTestHookBeforeWait pins the worker in the exact gap between its
// last failed drainOnce() and wait_for(), so the notify below is driven
// into that gap deterministically rather than by timing luck.
TEST(TheWorkerWakesPromptlyRatherThanWaitingForThePollTimeout) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    std::mutex hookMutex;
    std::condition_variable hookCv;
    bool workerParkedInGap = false;
    bool releaseWorker = false;

    runtime.setTestHookBeforeWait([&] {
        std::unique_lock<std::mutex> lock(hookMutex);
        workerParkedInGap = true;
        hookCv.notify_all();
        hookCv.wait(lock, [&] { return releaseWorker; });
    });

    runtime.start();

    // Block until the worker has drained its startup work and is sitting
    // in the hook, i.e. it has not yet taken wakeMutex_ to enter wait_for.
    {
        std::unique_lock<std::mutex> lock(hookMutex);
        hookCv.wait(lock, [&] { return workerParkedInGap; });
    }

    // This notify lands while the worker is still held in the hook,
    // before it can be waiting on the condition variable: exactly the gap
    // a lost wakeup happens in.
    const auto begin = std::chrono::steady_clock::now();
    runtime.observeCallback("Main Show", "start", "mainPlaylist", 0, "", "");

    {
        std::lock_guard<std::mutex> lock(hookMutex);
        releaseWorker = true;
    }
    hookCv.notify_all();

    while (runtime.publishedCount() < 1) {
        std::this_thread::yield();
        if (std::chrono::steady_clock::now() - begin > std::chrono::seconds(2)) break;
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    runtime.stop();

    CHECK_EQ(runtime.publishedCount(), static_cast<std::uint64_t>(1));
    // A correct predicate on wait_for observes hasWork_ already set and
    // returns immediately; a lost wakeup instead waits out the full 250ms
    // poll fallback.
    CHECK(elapsed < std::chrono::milliseconds(200));
}

// finding 12: sequenceFilename and mediaFilename truncation is recorded on
// the callback thread but was never carried past drainOnce(). Identity
// still gates on playlistName and section only, so this observation must
// still publish, but the evidence must say a filename was cut.
TEST(TruncatedSequenceAndMediaFilenamesAreCarriedAsEvidenceNotDropped) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);

    const std::string longSequence(300, 's');
    const std::string longMedia(300, 'm');
    runtime.observeCallback("Main Show", "start", "mainPlaylist", 0, longSequence.c_str(), longMedia.c_str());
    CHECK(runtime.drainOnce());

    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(1));
    const PlaylistEntryObservation& o = sink.published[0];
    CHECK(o.unavailable == showmesh::IdentityUnavailable::kNone);
    CHECK(o.sequenceFilenameTruncated);
    CHECK(o.mediaFilenameTruncated);
    CHECK_EQ(o.sequenceFilename.size(), showmesh::kMaxFilenameLength);
}

TEST(AnAbsentSinkIsNotACrash) {
    FakeDefinitions definitions;
    ShowMeshRuntime runtime(&definitions, nullptr, testClock);
    runtime.observeCallback("Main Show", "start", "mainPlaylist", 0, "", "");
    CHECK(runtime.drainOnce());
    // A null sink accepts nothing, so nothing was actually published.
    CHECK_EQ(runtime.publishedCount(), static_cast<std::uint64_t>(0));
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
