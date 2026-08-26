#include "showmesh/runtime.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "check.h"
#include "showmesh/brightness_codec.h"
#include "showmesh/brightness_store.h"
#include "showmesh/coordinator_client.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/http_transport.h"
#include "showmesh/sequence_store.h"

using showmesh::BrightnessFileStore;
using showmesh::CommandOutcome;
using showmesh::CoordinatorClient;
using showmesh::CredentialSource;
using showmesh::DefinitionPublisher;
using showmesh::HttpRequest;
using showmesh::HttpResponse;
using showmesh::HttpTransport;
using showmesh::ObservationSink;
using showmesh::PlaylistDefinitionSource;
using showmesh::PlaylistEntryObservation;
using showmesh::RetryPolicy;
using showmesh::SequenceFileStore;
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
    std::vector<std::string> playlistNames() override { return names; }

    std::string definition = kDefinition;
    std::string uuid = kUuid;
    std::string lastRequested;
    std::vector<std::string> names;
    int definitionCalls = 0;
};

class RecordingPublisher : public DefinitionPublisher {
 public:
    bool publishDefinition(const std::string& instanceUuid, const std::string& playlistName,
                           const std::string& playlistHash, const std::string& canonicalDefinition,
                           showmesh::TimeMillis capturedAtMillis) override {
        published.push_back(Record{instanceUuid, playlistName, playlistHash, canonicalDefinition, capturedAtMillis});
        return accept;
    }

    struct Record {
        std::string instanceUuid;
        std::string playlistName;
        std::string playlistHash;
        std::string canonicalDefinition;
        showmesh::TimeMillis capturedAtMillis;
    };
    std::vector<Record> published;
    bool accept = true;
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

// A transport that answers 200 and records what actually reached the
// wire. Used to run an unavailable observation through the real
// CoordinatorClient and the real buildObservationBody(), rather than
// through RecordingSink, which never builds a payload at all and so
// cannot see a field the payload builder itself would refuse on.
class RecordingTransport : public HttpTransport {
 public:
    HttpResponse post(const HttpRequest& request) override {
        requests.push_back(request);
        HttpResponse response;
        response.transportOk = true;
        response.statusCode = 200;
        return response;
    }
    std::vector<HttpRequest> requests;
};

// Always answers "no response at all", exactly what an unreachable
// coordinator looks like to the transport seam.
class UnreachableTransport : public HttpTransport {
 public:
    HttpResponse post(const HttpRequest& request) override {
        requests.push_back(request);
        HttpResponse response;
        response.transportOk = false;
        response.error = "connection refused";
        return response;
    }
    std::vector<HttpRequest> requests;
};

class AlwaysCredentials : public CredentialSource {
 public:
    bool token(std::string* out, std::string*) override {
        *out = "a-test-bearer-token";
        return true;
    }
    void invalidate() override {}
};

// A scratch directory for the sequence-persistence tests below, created
// with mkdtemp so parallel test runs never collide and removed on scope
// exit.
class TempDir {
 public:
    TempDir() {
        char buffer[] = "/tmp/showmesh-runtime-test-XXXXXX";
        const char* made = ::mkdtemp(buffer);
        CHECK(made != nullptr);
        path_ = made != nullptr ? std::string(made) : std::string();
    }
    ~TempDir() {
        for (const char* name : {"sequence-state", "sequence-state.bak", "sequence-state.tmp",
                                 "sequence-state.bak.tmp", "brightness-state", "brightness-state.bak",
                                 "brightness-state.tmp", "brightness-state.bak.tmp"}) {
            std::remove((path_ + "/" + name).c_str());
        }
        ::rmdir(path_.c_str());
    }
    const std::string& path() const { return path_; }

 private:
    std::string path_;
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

// finding 1: an unavailable observation was built with no instanceUuid
// even when the plugin had one, because neither unavailable path in
// drainOnce() copied it in. buildObservationBody() then refuses any
// observation with an empty instanceUuid (matching the coordinator's own
// refusal), so the observation never left the host. RecordingSink cannot
// see this: it never calls buildObservationBody() at all. Routing through
// a real CoordinatorClient and a real transport is what makes this class
// of defect visible again.
TEST(ATruncatedIdentityObservationReachesTheRealTransportWithItsInstanceUuid) {
    FakeDefinitions definitions;
    RecordingTransport transport;
    AlwaysCredentials credentials;
    RetryPolicy policy;
    CoordinatorClient client(&transport, &credentials, "http://coordinator.invalid", testClock, nullptr, nullptr,
                             policy);
    ShowMeshRuntime runtime(&definitions, &client, testClock);

    const std::string longName(300, 'a');
    runtime.observeCallback(longName.c_str(), "start", "mainPlaylist", 0, "", "");
    CHECK(runtime.drainOnce());

    // A locally refused observation never reaches the transport at all.
    CHECK_EQ(transport.requests.size(), std::size_t{1});
    CHECK(transport.requests.front().body.find("\"instanceUuid\":\"" + std::string(kUuid) + "\"") !=
          std::string::npos);
    CHECK(transport.requests.front().body.find("\"unavailable\":\"truncated_identity_field\"") != std::string::npos);
}

TEST(AnUnresolvedIdentityObservationReachesTheRealTransportWithItsInstanceUuid) {
    FakeDefinitions definitions;
    definitions.definition = "";
    RecordingTransport transport;
    AlwaysCredentials credentials;
    RetryPolicy policy;
    CoordinatorClient client(&transport, &credentials, "http://coordinator.invalid", testClock, nullptr, nullptr,
                             policy);
    ShowMeshRuntime runtime(&definitions, &client, testClock);

    runtime.observeCallback("Main Show", "playing", "mainPlaylist", 2, "a.fseq", "song.mp3");
    CHECK(runtime.drainOnce());

    CHECK_EQ(transport.requests.size(), std::size_t{1});
    CHECK(transport.requests.front().body.find("\"instanceUuid\":\"" + std::string(kUuid) + "\"") !=
          std::string::npos);
    CHECK(transport.requests.front().body.find("\"unavailable\":\"missing_definition\"") != std::string::npos);
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

// A restarted plugin resuming from 0 is the exact wedge SM-213 exists to
// close: the coordinator refuses any sequence it has already seen, so a
// second process that starts back at 1 is refused forever. This proves
// the fix at the runtime boundary: a second ShowMeshRuntime, backed by
// the same on-disk store, issues sequence numbers strictly above the
// first runtime's, without either runtime ever reaching a coordinator.
TEST(ARestartedRuntimeResumesAboveThePersistedSequence) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;

    {
        SequenceFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, &store);
        for (int i = 0; i < 3; ++i) {
            runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "a.fseq", "");
            CHECK(runtime.drainOnce());
        }
    }
    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(3));
    CHECK_EQ(sink.published.back().sequence, static_cast<std::uint64_t>(3));

    // A fresh runtime, as a restarted fppd would construct, over the same
    // directory.
    RecordingSink secondSink;
    SequenceFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, &secondStore);
    restarted.observeCallback("Main Show", "playing", "mainPlaylist", 3, "a.fseq", "");
    CHECK(restarted.drainOnce());

    CHECK_EQ(secondSink.published.size(), static_cast<std::size_t>(1));
    CHECK(secondSink.published[0].sequence > static_cast<std::uint64_t>(3));
    CHECK_EQ(secondSink.published[0].sequence, static_cast<std::uint64_t>(4));
}

// A runtime with no configured sequence store keeps the previous
// behavior exactly: always starts at 0, and drainOnce() does not touch
// the filesystem at all.
TEST(ARuntimeWithNoSequenceStoreConfiguredStillStartsAtZero) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);
    runtime.observeCallback("Main Show", "start", "mainPlaylist", 0, "a.fseq", "");
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.published[0].sequence, static_cast<std::uint64_t>(1));
    // No-op, never crashes, when nothing is configured.
    CHECK(runtime.flushSequenceState());
}

// drainOnce() already persists its own sequence number, so a flush right
// after one, with nothing else in between, cannot tell flushSequenceState()
// apart from a no-op that just returns true: load() == 1 either way. This
// reaches a state where the in-memory sequence is genuinely ahead of what
// is durable (drainOnce()'s own store() call fails, because the directory
// component is a plain file, not a directory, so mkdir() cannot create it
// no matter the caller's privilege), then makes the directory writable and
// flushes, so only a flush that actually calls through to store() can make
// this pass.
TEST(FlushSequenceStatePersistsAValueThatDrainOnceFailedToPersist) {
    char fileTemplate[] = "/tmp/showmesh-runtime-flush-test-XXXXXX";
    const int fd = ::mkstemp(fileTemplate);
    CHECK(fd >= 0);
    if (fd >= 0) ::close(fd);
    // The store's directory itself is the blocking file (not a subpath
    // beneath it), so replacing the file with a directory at the exact
    // same path is enough to make every path this store already computed
    // valid again.
    const std::string dirPath = fileTemplate;

    FakeDefinitions definitions;
    RecordingSink sink;
    SequenceFileStore store(dirPath);
    ShowMeshRuntime runtime(&definitions, &sink, testClock, &store);

    runtime.observeCallback("Main Show", "start", "mainPlaylist", 0, "a.fseq", "");
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(sink.published[0].sequence, static_cast<std::uint64_t>(1));
    // The observation still published; nothing landed on disk.
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(0));
    CHECK_EQ(runtime.sequencePersistFailureCount(), static_cast<std::uint64_t>(1));

    // Replace the blocking file with a real directory so the store this
    // runtime already holds a pointer into can finally be written.
    CHECK_EQ(std::remove(fileTemplate), 0);
    CHECK_EQ(::mkdir(fileTemplate, 0755), 0);

    CHECK(runtime.flushSequenceState());
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(1));

    std::remove((dirPath + "/sequence-state").c_str());
    std::remove((dirPath + "/sequence-state.bak").c_str());
    ::rmdir(fileTemplate);
}

// The exact repro from the F1 finding: a state directory that does not
// exist yet, the shape resolveSequenceStateDir() hands a fresh
// SequenceFileStore on a default host nothing has provisioned. Before the
// fix, every store() call inside drainOnce() failed silently and a
// restarted runtime always resumed at 1 no matter how many observations
// the first process had already issued.
TEST(ARuntimeCreatesAMissingSequenceStateDirectoryAndPersistsThroughARestart) {
    TempDir dir;
    const std::string missing = dir.path() + "/state";

    FakeDefinitions definitions;
    RecordingSink sink;
    {
        SequenceFileStore store(missing);
        ShowMeshRuntime runtime(&definitions, &sink, testClock, &store);
        for (int i = 0; i < 3; ++i) {
            runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "a.fseq", "");
            CHECK(runtime.drainOnce());
        }
        CHECK_EQ(runtime.sequencePersistFailureCount(), static_cast<std::uint64_t>(0));
    }
    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(3));
    CHECK_EQ(sink.published.back().sequence, static_cast<std::uint64_t>(3));

    RecordingSink secondSink;
    SequenceFileStore secondStore(missing);
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, &secondStore);
    restarted.observeCallback("Main Show", "playing", "mainPlaylist", 3, "a.fseq", "");
    CHECK(restarted.drainOnce());
    CHECK_EQ(secondSink.published[0].sequence, static_cast<std::uint64_t>(4));

    std::remove((missing + "/sequence-state").c_str());
    std::remove((missing + "/sequence-state.bak").c_str());
    ::rmdir(missing.c_str());
}

// A directory that cannot be written at all (its component is a plain
// file, forever, unlike a permission bit a root test runner would simply
// bypass): every store() fails, but the observation still publishes and
// the failure is counted rather than silently dropped.
TEST(ARuntimeCountsRatherThanSilentlyDropsAPersistentStoreFailure) {
    char fileTemplate[] = "/tmp/showmesh-runtime-unwritable-test-XXXXXX";
    const int fd = ::mkstemp(fileTemplate);
    CHECK(fd >= 0);
    if (fd >= 0) ::close(fd);
    const std::string blocked = std::string(fileTemplate) + "/state";

    FakeDefinitions definitions;
    RecordingSink sink;
    SequenceFileStore store(blocked);
    ShowMeshRuntime runtime(&definitions, &sink, testClock, &store);

    for (int i = 0; i < 3; ++i) {
        runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "a.fseq", "");
        CHECK(runtime.drainOnce());
    }
    CHECK_EQ(sink.published.size(), static_cast<std::size_t>(3));
    CHECK_EQ(runtime.sequencePersistFailureCount(), static_cast<std::uint64_t>(3));

    std::remove(fileTemplate);
}

// A corrupted on-disk file at startup must never rewind a restarted
// runtime below what a fresh SequenceState already starts at, and must
// never crash construction.
TEST(ARuntimeConstructedOverACorruptSequenceFileStartsCleanRatherThanCrashing) {
    TempDir dir;
    {
        std::ofstream corrupt(dir.path() + "/sequence-state", std::ios::trunc);
        corrupt << "not a valid record";
    }
    FakeDefinitions definitions;
    RecordingSink sink;
    SequenceFileStore store(dir.path());
    ShowMeshRuntime runtime(&definitions, &sink, testClock, &store);
    runtime.observeCallback("Main Show", "start", "mainPlaylist", 0, "a.fseq", "");
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.published[0].sequence, static_cast<std::uint64_t>(1));
}

// "No files at all" (a genuine first run) and "files present, all
// invalid" (a value was certainly issued before; its height is unknown)
// both make a fresh SequenceState resume at the same value, 0. Only the
// second one is the SM-213 wedge risk, so ShowMeshRuntime must tell an
// operator the two apart rather than resuming silently either way.
TEST(ARuntimeFlagsAllInvalidSequenceFilesAtStartupButNotAGenuineFirstRun) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    {
        SequenceFileStore fresh(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, &fresh);
        CHECK(!runtime.sequenceFilesWereAllInvalidAtStartup());
    }

    {
        SequenceFileStore seed(dir.path());
        CHECK(seed.store(500));
        CHECK(seed.store(700));  // both primary and backup now exist
    }
    {
        std::ofstream primary(dir.path() + "/sequence-state", std::ios::trunc);
        primary << "garbage";
    }
    {
        std::ofstream backup(dir.path() + "/sequence-state.bak", std::ios::trunc);
        backup << "also garbage";
    }

    RecordingSink secondSink;
    SequenceFileStore corrupted(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, &corrupted);
    CHECK(restarted.sequenceFilesWereAllInvalidAtStartup());
    // Resumes at 1 anyway, the same as a genuine first run: 0 is the only
    // safe number without a real height to resume from. The flag above,
    // not a different resumption value, is what makes this visible.
    restarted.observeCallback("Main Show", "start", "mainPlaylist", 0, "a.fseq", "");
    CHECK(restarted.drainOnce());
    CHECK_EQ(secondSink.published[0].sequence, static_cast<std::uint64_t>(1));
}

TEST(ARuntimeWithNoBrightnessStoreConfiguredStaysAtEngineDefaults) {
    FakeDefinitions definitions;
    RecordingSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, testClock);
    CHECK_EQ(runtime.brightness()->effectivePercentAt(gNow), 100);
    // No-op, never crashes, when nothing is configured.
    CHECK(runtime.flushBrightnessState());
}

// A configured store that has never been written to is the true "first
// run" case: the engine's own built-in default (100) is correct here,
// distinct from every other way brightnessStore_->load() can come back
// without a trusted record.
TEST(ARuntimeOverAnEmptyBrightnessStoreStaysAtEngineDefaults) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    BrightnessFileStore store(dir.path());
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);
    CHECK_EQ(runtime.brightness()->effectivePercentAt(gNow), 100);
    CHECK(runtime.brightnessRestartTrust() == showmesh::BrightnessRestartTrust::kTrustedOrNoRecord);
}

// The F1 regression: a corrupted primary record must not fall back to
// the backup's numbers as if they were current. The backup is, by
// construction, the state the (now unrecoverable) primary superseded, so
// trusting it can restore a value brighter than anything this host
// actually applied after it was written -- exactly what happens below
// without the fix: ceiling 80 is genuinely applied, an operator dims to
// 20 which is also genuinely applied, the primary corrupts, and a naive
// restart would come back at the superseded 80 instead of not brighter
// than the unknown true last-applied value.
TEST(ARestartWithACorruptedPrimaryAndAStaleBackupSettlesAtTheSafeCeilingNotTheStaleBackup) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    const TimeMillis savedNow = gNow;
    std::vector<std::uint8_t> frame(4, 0xff);

    {
        BrightnessFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);

        CHECK(runtime.applyBrightnessCommand("80", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());  // 80 actually applied to the wall
        CHECK(runtime.flushBrightnessState());                   // primary: 80

        CHECK(runtime.applyBrightnessCommand("20", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());  // 20 actually applied to the wall
        CHECK(runtime.flushBrightnessState());  // rotates 80 into the backup slot, primary: 20
    }

    // The primary (20, the true last-applied value) is now unreadable.
    // Only the stale backup (80) survives.
    {
        std::ofstream corrupt(dir.path() + "/brightness-state", std::ios::trunc);
        corrupt << "not a valid record";
    }

    RecordingSink secondSink;
    BrightnessFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, nullptr, nullptr, &secondStore);

    // Never the backup's superseded 80, and never the engine's own
    // bright built-in default of 100: the true last-applied value (20)
    // is unrecoverable, so the restart settles at the configured safe
    // ceiling (the runtime's default here, kDefaultSafeCeilingPercent)
    // rather than at zero, because a read error must not turn the rig
    // off.
    //
    // Note deliberately what this gives up. 20 was the last value
    // actually applied, and the safe ceiling is above it, so this DOES
    // come back brighter than what was applied. That is the accepted
    // trade: when the record cannot be trusted, the last applied value
    // is unknown, and the only value that could never be brighter is
    // zero. The safe ceiling is a bounded, operator-chosen dim rather
    // than an unbounded restore, and it is bounded well below the
    // engine's bright default. Do not "fix" this back to a
    // never-brighter assertion without reopening that decision.
    CHECK_NEAR(restarted.brightness()->ceilingAt(gNow), showmesh::kDefaultSafeCeilingPercent, 1e-9);
    CHECK(restarted.brightness()->ceilingAt(gNow) < 80.0);
    // The coordinator-visible regression this closes: an untrusted-
    // restart settle must not pass silently. The adapter logs on this
    // exact signal at startup (see plugin.cpp's logBrightnessRestartTrust),
    // so if this stops being reported, the operator-facing log line
    // stops too.
    CHECK(restarted.brightnessRestartTrust() ==
          showmesh::BrightnessRestartTrust::kPrimaryUnreadableBackupRecovered);

    gNow = savedNow;
}

// The F2 regression: a missing or doubly-corrupt record must not restart
// at the engine's built-in default of 100, the brightest value in the
// range, once real state has ever been written. A default is only
// correct on a true first run (see
// ARuntimeOverAnEmptyBrightnessStoreStaysAtEngineDefaults).
TEST(ARestartOverADoublyCorruptRecordNeverComesBackAtTheBrightDefault) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    const TimeMillis savedNow = gNow;
    std::vector<std::uint8_t> frame(4, 0xff);

    {
        BrightnessFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);
        CHECK(runtime.applyBrightnessCommand("40", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.flushBrightnessState());  // primary: 40, no backup yet
    }

    {
        std::ofstream corrupt(dir.path() + "/brightness-state", std::ios::trunc);
        corrupt << "not a valid record";
    }

    RecordingSink secondSink;
    BrightnessFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, nullptr, nullptr, &secondStore);

    // Settles at the configured safe ceiling, not the engine's bright
    // built-in default of 100 and not zero: see the sibling test above.
    CHECK_NEAR(restarted.brightness()->ceilingAt(gNow), showmesh::kDefaultSafeCeilingPercent, 1e-9);
    CHECK(restarted.brightness()->ceilingAt(gNow) < 100.0);
    CHECK(restarted.brightnessRestartTrust() == showmesh::BrightnessRestartTrust::kNeitherRecordReadable);

    gNow = savedNow;
}

// Both records unreadable, distinct from the "one existed but
// nothing readable" case above, but the same branch
// (kNeitherRecordReadable) and the same safe-ceiling settle: a checksum
// mismatch, corruption, and an outright missing-but-expected file all
// land here because BrightnessFileStore::load() cannot tell them apart
// beyond "a record was expected and none of it can be trusted".
TEST(ARestartWithBothRecordsUnreadableRestoresToTheConfiguredSafeCeiling) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    const TimeMillis savedNow = gNow;
    std::vector<std::uint8_t> frame(4, 0xff);

    {
        BrightnessFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);
        CHECK(runtime.applyBrightnessCommand("80", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.flushBrightnessState());
        CHECK(runtime.applyBrightnessCommand("20", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.flushBrightnessState());  // both primary and backup now exist
    }

    {
        std::ofstream corruptPrimary(dir.path() + "/brightness-state", std::ios::trunc);
        corruptPrimary << "garbage";
        std::ofstream corruptBackup(dir.path() + "/brightness-state.bak", std::ios::trunc);
        corruptBackup << "also garbage";
    }

    RecordingSink secondSink;
    BrightnessFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, nullptr, nullptr, &secondStore);

    CHECK_NEAR(restarted.brightness()->ceilingAt(gNow), showmesh::kDefaultSafeCeilingPercent, 1e-9);
    CHECK(restarted.brightnessRestartTrust() == showmesh::BrightnessRestartTrust::kNeitherRecordReadable);

    gNow = savedNow;
}

// A checksum mismatch on the primary (a bit-flipped value line
// paired with the original, now-stale checksum line) falls back to the
// superseded backup, exactly like the unparseable-primary case above,
// and the runtime must not trust that backup's numbers as current.
TEST(ARestartWithAChecksumMismatchedPrimaryRestoresToTheConfiguredSafeCeiling) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    const TimeMillis savedNow = gNow;
    std::vector<std::uint8_t> frame(4, 0xff);

    {
        BrightnessFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);
        CHECK(runtime.applyBrightnessCommand("80", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.flushBrightnessState());
        CHECK(runtime.applyBrightnessCommand("20", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.flushBrightnessState());  // backup now holds the 80 record
    }

    {
        // Tamper the value line but leave the checksum line as it was for
        // the true value, the same technique
        // ABitFlippedRecordWithAStillPlausibleChecksumMismatchIsRejected
        // uses in brightness_store_test.cpp.
        std::ifstream in(dir.path() + "/brightness-state", std::ios::binary);
        std::string valueLine;
        std::string checksumLine;
        std::getline(in, valueLine);
        std::getline(in, checksumLine);
        in.close();
        std::ofstream out(dir.path() + "/brightness-state", std::ios::trunc | std::ios::binary);
        out << valueLine << "x\n" << checksumLine << "\n";
    }

    RecordingSink secondSink;
    BrightnessFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, nullptr, nullptr, &secondStore);

    CHECK_NEAR(restarted.brightness()->ceilingAt(gNow), showmesh::kDefaultSafeCeilingPercent, 1e-9);
    CHECK(restarted.brightnessRestartTrust() ==
          showmesh::BrightnessRestartTrust::kPrimaryUnreadableBackupRecovered);

    gNow = savedNow;
}

// The configured safe ceiling is not hardcoded. A non-default
// value passed to the constructor is what an untrusted restart actually
// settles at.
TEST(AConfiguredNonDefaultSafeCeilingIsHonoredOnAnUntrustedRestart) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    const TimeMillis savedNow = gNow;
    std::vector<std::uint8_t> frame(4, 0xff);

    {
        BrightnessFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);
        CHECK(runtime.applyBrightnessCommand("80", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.flushBrightnessState());  // primary: 80, no backup yet
    }

    {
        std::ofstream corrupt(dir.path() + "/brightness-state", std::ios::trunc);
        corrupt << "not a valid record";
    }

    RecordingSink secondSink;
    BrightnessFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, nullptr, nullptr, &secondStore, 25);

    CHECK_NEAR(restarted.brightness()->ceilingAt(gNow), 25.0, 1e-9);
    CHECK(restarted.brightnessRestartTrust() == showmesh::BrightnessRestartTrust::kNeitherRecordReadable);

    gNow = savedNow;
}

// Gain settles to 100 on the same untrusted-restart path, because
// the ceiling is what carries the safety, not the gain.
TEST(AnUntrustedRestartSettlesGainToOneHundred) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    const TimeMillis savedNow = gNow;
    std::vector<std::uint8_t> frame(4, 0xff);

    {
        BrightnessFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);
        CHECK(runtime.applyBrightnessCommand("80", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.flushBrightnessState());
    }

    {
        std::ofstream corrupt(dir.path() + "/brightness-state", std::ios::trunc);
        corrupt << "not a valid record";
    }

    RecordingSink secondSink;
    BrightnessFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, nullptr, nullptr, &secondStore);

    CHECK_NEAR(restarted.brightness()->gainAt(gNow), 100.0, 1e-9);

    gNow = savedNow;
}

// The settle must bump the revision the same way a local command
// does, and the runtime's own publish path (encodeFullState(), what the
// adapters' publishFullStateIfChanged() sends over MultiSync) must carry
// that bumped revision rather than the zero a fresh engine starts at.
// Before this change, settleDarkAfterUntrustedRestart() touched neither
// revision_ nor stateChangedAtMillis_, so a settled node's full state
// never actually went out.
TEST(AnUntrustedRestartSettleBumpsTheRevisionAndPublishesFullState) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    const TimeMillis savedNow = gNow;
    std::vector<std::uint8_t> frame(4, 0xff);

    {
        BrightnessFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);
        CHECK(runtime.applyBrightnessCommand("80", "0").ok);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.flushBrightnessState());
    }

    {
        std::ofstream corrupt(dir.path() + "/brightness-state", std::ios::trunc);
        corrupt << "not a valid record";
    }

    RecordingSink secondSink;
    BrightnessFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, nullptr, nullptr, &secondStore);

    CHECK(restarted.brightness()->revision() != 0);
    const std::uint64_t settledRevision = restarted.brightness()->revision();
    const TimeMillis settledStateChangedAt = restarted.brightness()->captureState(gNow).stateChangedAtMillis;
    CHECK(settledStateChangedAt != 0);

    // encodeFullState() is exactly what the adapters' publishFullStateIfChanged
    // sends over MultiSync once the revision differs from the last one
    // published; decoding the payload it produces here proves the
    // settle's bumped revision genuinely reaches that path.
    const std::string payload = restarted.encodeFullState();
    CHECK(!payload.empty());
    const showmesh::BrightnessStateDecode decoded = showmesh::decodeBrightnessState(payload);
    CHECK(decoded.ok);
    CHECK_EQ(decoded.state.revision, settledRevision);
    CHECK_EQ(decoded.state.stateChangedAtMillis, settledStateChangedAt);

    gNow = savedNow;
}

TEST(FlushBrightnessStatePersistsTheCurrentStateEvenWithoutAFrame) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    BrightnessFileStore store(dir.path());
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);

    CHECK(runtime.applyBrightnessCommand("40", "0").ok);
    CHECK(runtime.flushBrightnessState());

    showmesh::BrightnessStateLoad loaded = store.load();
    CHECK(loaded.ok);
    CHECK_NEAR(loaded.state.ceilingTarget, 40.0, 1e-9);
}

// The acceptance property this whole feature exists for: a restart mid-
// fade must never apply channel data brighter than what this host had
// already applied before the restart. Exercised through the full
// runtime + on-disk store, not just BrightnessEngine::restoreFromPersisted
// directly (brightness_test.cpp already covers that in isolation), so
// this proves the store round-trips a real captured state and the
// runtime wires restoreFromPersisted to it on construction.
TEST(ARestartMidFadeNeverComesBackBrighterThanWhatWasApplied) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    const TimeMillis savedNow = gNow;

    const TimeMillis fadeStart = gNow;
    {
        BrightnessFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);

        // Drop to 30 instantly, then start fading back up to 100 over
        // 100 seconds -- a fade whose target is BRIGHTER than where it
        // starts, so a bug that resumed at the target instead of the
        // darker of the two would be visible.
        CHECK(runtime.applyBrightnessCommand("30", "0").ok);
        CHECK(runtime.applyBrightnessCommand("100", "100").ok);

        // Halfway through the fade, a frame is actually rendered: this
        // is the only thing that ever updates lastAppliedCeiling, so it
        // is the true record of what reached the outputs.
        gNow = fadeStart + 50'000;
        std::vector<std::uint8_t> frame(4, 0xff);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.flushBrightnessState());
        // runtime (and its worker thread) is torn down here, standing in
        // for the process exiting mid-fade with no further frame ever
        // rendered or flushed.
    }

    // The restart's own clock makes the persisted fade window
    // untrustworthy (before the window even started), the exact
    // condition BrightnessEngine::restoreFromPersisted refuses to resume
    // a fade under.
    gNow = fadeStart - 1;
    RecordingSink secondSink;
    BrightnessFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, nullptr, nullptr, &secondStore);

    // Halfway through a 30->100 fade is 65: brighter than 30, darker than
    // 100. The restored ceiling must land there, never at the fade's
    // brighter target of 100.
    CHECK_NEAR(restarted.brightness()->ceilingAt(gNow), 65.0, 1e-9);
    CHECK(restarted.brightness()->ceilingAt(gNow) < 100.0);

    gNow = savedNow;
}

// The F4 regression: the test above forces fadeTimingIsTrustworthy false
// so it only exercises the branch where the acceptance property already
// holds. This covers the branch that actually breaks it -- an ordinary
// restart with a working clock, mid a trustworthy up-fade -- which
// SM-214's acceptance sentence does not carve an exception for.
TEST(ARestartMidATrustedUpFadeNeverComesBackBrighterThanWhatWasApplied) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    const TimeMillis savedNow = gNow;

    const TimeMillis commandTime = gNow;
    {
        BrightnessFileStore store(dir.path());
        ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);

        // Settle at 30 and let a frame actually apply it, then start a
        // 3600s fade up to 100. The command's own flush (brightness_
        // command.h's run(), simulated here by an explicit flush right
        // after the command) is what persists the fade window at start,
        // with lastAppliedCeiling still 30: no frame has rendered the
        // fade in progress yet.
        CHECK(runtime.applyBrightnessCommand("30", "0").ok);
        std::vector<std::uint8_t> frame(4, 0xff);
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK(runtime.applyBrightnessCommand("100", "3600").ok);
        CHECK(runtime.flushBrightnessState());

        // 10 seconds later, one more frame actually applies ~30.19, then
        // the process is killed with no further frame or flush -- the
        // persisted record still says lastAppliedCeiling=30.
        gNow = commandTime + 10'000;
        runtime.modifyChannelData(frame.data(), frame.size());
        CHECK_NEAR(runtime.brightness()->ceilingAt(gNow), 30.19, 0.01);
    }

    // Restarted 30 minutes after the command, with a sane, forward clock:
    // fadeTimingIsTrustworthy is true, the branch the test above never
    // reaches.
    gNow = commandTime + 1'800'000;
    RecordingSink secondSink;
    BrightnessFileStore secondStore(dir.path());
    ShowMeshRuntime restarted(&definitions, &secondSink, testClock, nullptr, nullptr, &secondStore);

    // Naively resuming the recorded 30->100 fade from its original start
    // would land at 65 here (30 minutes is half of the 3600s window).
    // That is 35 points brighter than the 30 this host is known to have
    // applied. The restored value must not exceed 30.
    CHECK_NEAR(restarted.brightness()->ceilingAt(gNow), 30.0, 1e-9);
    CHECK(restarted.brightness()->ceilingAt(gNow) < 65.0);
    // A trusted primary is not a dark settle: nothing to report.
    CHECK(restarted.brightnessRestartTrust() == showmesh::BrightnessRestartTrust::kTrustedOrNoRecord);

    gNow = savedNow;
}

// The F3 regression: publishFullStateIfChanged() runs on FPP's per-frame
// output thread, so it must never perform the store's write itself.
// markBrightnessDirty() is the seam it calls instead: a cheap flag set
// plus a wakeup, never touching disk. flushBrightnessIfDirty() is what
// the worker thread calls to actually do the write; exercising it
// directly here, without starting the worker, makes the separation
// deterministic instead of racing a background thread.
TEST(MarkingBrightnessDirtyNeverWritesUntilTheWorkerFlushesIt) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    BrightnessFileStore store(dir.path());
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);

    CHECK(runtime.applyBrightnessCommand("40", "0").ok);
    runtime.markBrightnessDirty();

    // The frame path's own call must not have reached disk.
    CHECK(!store.load().ok);

    // What the worker thread's loop does with a pending dirty mark.
    CHECK(runtime.flushBrightnessIfDirty());
    showmesh::BrightnessStateLoad loaded = store.load();
    CHECK(loaded.ok);
    CHECK_NEAR(loaded.state.ceilingTarget, 40.0, 1e-9);

    // Nothing pending: a second call is a no-op that still reports
    // success.
    CHECK(runtime.flushBrightnessIfDirty());
}

// The F3 double-write regression: an operator command already flushes
// synchronously (brightness_command.h's run()), so the very next frame's
// dirty mark at the same revision must not write the identical record to
// disk again. store()'s rotation is what makes a redundant write
// observable: it would push the already-current record into the backup
// slot a second time, overwriting the genuinely older backup for no
// reason.
TEST(ARedundantDirtyMarkAtTheSameRevisionDoesNotRewriteTheFile) {
    TempDir dir;
    FakeDefinitions definitions;
    RecordingSink sink;
    BrightnessFileStore store(dir.path());
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, nullptr, &store);

    CHECK(runtime.applyBrightnessCommand("50", "0").ok);
    CHECK(runtime.flushBrightnessState());  // primary: 50, no backup yet

    CHECK(runtime.applyBrightnessCommand("80", "0").ok);
    CHECK(runtime.flushBrightnessState());  // rotates 50 into the backup slot, primary: 80

    // The next frame's dirty mark, at the same revision applyBrightness-
    // Command("80", "0") already produced and flushBrightnessState()
    // already persisted above.
    runtime.markBrightnessDirty();
    CHECK(runtime.flushBrightnessIfDirty());

    showmesh::BrightnessStateLoad loaded = store.load();
    CHECK(loaded.ok);
    CHECK_NEAR(loaded.state.ceilingTarget, 80.0, 1e-9);
    // The backup must still be the genuinely older 50 record, not 80
    // rotated into it a second time by the redundant flush.
    std::ifstream backup(dir.path() + "/brightness-state.bak", std::ios::binary);
    std::string backupLine;
    CHECK(static_cast<bool>(std::getline(backup, backupLine)));
    showmesh::BrightnessStateDecode backupState = showmesh::decodeBrightnessState(backupLine);
    CHECK(backupState.ok);
    CHECK_NEAR(backupState.state.ceilingTarget, 50.0, 1e-9);
}

TEST(TheWorkerPublishesEveryDefinitionOnTheHostAtStartEvenWithNothingPlaying) {
    FakeDefinitions definitions;
    definitions.names = {"Halloween Main", "Christmas Main"};
    RecordingSink sink;
    RecordingPublisher publisher;
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, &publisher);

    // No callback has fired: the coordinator would otherwise hold nothing
    // until FPP played something, and authoring happens with FPP idle.
    CHECK(runtime.sweepDefinitions());
    CHECK_EQ(publisher.published.size(), std::size_t{2});
    CHECK_EQ(publisher.published[0].playlistName, std::string("Halloween Main"));
    CHECK_EQ(publisher.published[1].playlistName, std::string("Christmas Main"));
    CHECK_EQ(publisher.published[0].instanceUuid, std::string(kUuid));
    CHECK_EQ(publisher.published[0].playlistHash.size(), std::size_t{64});
    // The complete definition the plugin hashed travels with the hash.
    CHECK(!publisher.published[0].canonicalDefinition.empty());
    CHECK(sink.published.empty());
}

// finding 3 (item 2): sweepDefinitions() never returned to the
// observation queue between definitions. Against a slow coordinator this
// let up to 5 attempts x 10 seconds of backoff pass per playlist while
// drainOnce() was never called, and the handoff holds only 16 pending
// events: real-time callbacks arriving during a many-playlist sweep would
// overflow it and coalesce away before the sweep ever got back to them.
// Queuing more events than the sweep's own playlist count, before the
// sweep starts, and checking they all drained by the time it returns
// (rather than only when a later drainOnce() call is made) is what
// distinguishes yielding between definitions from a lucky ordering.
TEST(ASweepDrainsPendingObservationsBetweenDefinitionsRatherThanAfterAll) {
    FakeDefinitions definitions;
    definitions.names = {"Halloween Main", "Christmas Main", "Fourth of July"};
    RecordingSink sink;
    RecordingPublisher publisher;
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, &publisher);

    for (int i = 0; i < 5; ++i) {
        runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "a.fseq", "");
    }
    CHECK_EQ(runtime.handoff().pending(), std::size_t{5});

    CHECK(runtime.sweepDefinitions());
    // 3 from the sweep itself, plus one per drained observation that
    // resolved identity (5): RecordingPublisher records every call, with
    // no held-definition cache of its own.
    CHECK_EQ(publisher.published.size(), std::size_t{8});
    // Drained during the sweep, not left behind for a caller to notice
    // only after it returns.
    CHECK_EQ(runtime.handoff().pending(), std::size_t{0});
    CHECK_EQ(sink.published.size(), std::size_t{5});
}

TEST(ARescanIsBoundedToOncePerMinute) {
    FakeDefinitions definitions;
    definitions.names = {"Halloween Main"};
    RecordingSink sink;
    RecordingPublisher publisher;
    const TimeMillis start = gNow;
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, &publisher);

    CHECK(runtime.maybeSweepDefinitions());
    CHECK_EQ(publisher.published.size(), std::size_t{1});

    gNow = start + 59'999;
    CHECK(!runtime.maybeSweepDefinitions());
    CHECK_EQ(publisher.published.size(), std::size_t{1});

    gNow = start + 60'000;
    CHECK(runtime.maybeSweepDefinitions());
    CHECK_EQ(publisher.published.size(), std::size_t{2});
    gNow = start;
}

TEST(ASweepWithNoInstanceUuidDoesNotStartTheRescanClock) {
    FakeDefinitions definitions;
    definitions.names = {"Halloween Main"};
    definitions.uuid.clear();
    RecordingSink sink;
    RecordingPublisher publisher;
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, &publisher);

    CHECK(!runtime.maybeSweepDefinitions());
    CHECK(publisher.published.empty());

    // The UUID appearing later must not have to wait out a minute that
    // never actually contained a sweep.
    definitions.uuid = kUuid;
    CHECK(runtime.maybeSweepDefinitions());
    CHECK_EQ(publisher.published.size(), std::size_t{1});
}

TEST(ResolvingAnEntryIdentityAlsoPublishesTheDefinitionBehindItsHash) {
    FakeDefinitions definitions;
    RecordingSink sink;
    RecordingPublisher publisher;
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, &publisher);

    runtime.observeCallback("Main Show", "playing", "mainPlaylist", 2, "a.fseq", "");
    CHECK(runtime.drainOnce());

    CHECK_EQ(sink.published.size(), std::size_t{1});
    CHECK_EQ(publisher.published.size(), std::size_t{1});
    // The same hash the observation cites, so the definition can never be
    // filed under one the observation will not match.
    CHECK_EQ(publisher.published[0].playlistHash, sink.published[0].identity.playlistHash);
}

TEST(AnUnavailableObservationPublishesNoDefinitionBecauseThereIsNoHash) {
    FakeDefinitions definitions;
    definitions.definition.clear();
    RecordingSink sink;
    RecordingPublisher publisher;
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, &publisher);

    runtime.observeCallback("Main Show", "playing", "mainPlaylist", 2, "a.fseq", "");
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.unavailable.size(), std::size_t{1});
    CHECK(publisher.published.empty());
}

TEST(ADefinitionTheCoordinatorRefusedDoesNotWithholdTheObservation) {
    FakeDefinitions definitions;
    RecordingSink sink;
    RecordingPublisher publisher;
    publisher.accept = false;
    ShowMeshRuntime runtime(&definitions, &sink, testClock, nullptr, &publisher);

    runtime.observeCallback("Main Show", "playing", "mainPlaylist", 2, "a.fseq", "");
    CHECK(runtime.drainOnce());
    CHECK_EQ(sink.published.size(), std::size_t{1});
    CHECK_EQ(runtime.publishedCount(), std::uint64_t{1});
}

// finding 4: postWithRetry() slept with a plain, uninterruptible
// sleeper_(backoffMillis) and never checked a stop flag between attempts.
// A worker stuck retrying publish() against an unreachable coordinator
// could hold ShowMeshRuntime::stop()'s join for the full retry budget
// (default policy: up to roughly 57 seconds). FPP 10's shutdown
// predicate gives up after 60 seconds, so stop() has to return in a small
// fraction of that even mid-backoff. This uses the real (default)
// sleeper, not a recording one, and the real default RetryPolicy, so the
// bound below is only meaningful because it is not mocked away.
TEST(StopReturnsPromptlyEvenWhileAPublishIsRetryingAgainstAnUnreachableCoordinator) {
    FakeDefinitions definitions;
    UnreachableTransport transport;
    AlwaysCredentials credentials;
    CoordinatorClient client(&transport, &credentials, "http://coordinator.invalid", testClock);
    ShowMeshRuntime runtime(&definitions, &client, testClock);

    runtime.start();
    runtime.observeCallback("Main Show", "playing", "mainPlaylist", 2, "a.fseq", "song.mp3");

    // Give the worker time to pick the event up and land inside its
    // first backoff wait, so stop() below has to interrupt an in-flight
    // retry rather than merely beating the worker to it.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto begin = std::chrono::steady_clock::now();
    runtime.stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto elapsedMillis = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // Well under FPP 10's 60 second shutdown deadline, and nowhere near
    // the uninterrupted worst case of roughly 57 seconds.
    CHECK(elapsedMillis < 5000);
    // The worker really did attempt delivery and really was retrying,
    // rather than the bound being trivially satisfied by nothing running.
    CHECK(!transport.requests.empty());
}
