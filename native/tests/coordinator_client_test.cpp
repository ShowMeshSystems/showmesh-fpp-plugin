#include "showmesh/coordinator_client.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "showmesh/observation_payload.h"

using showmesh::CoordinatorClient;
using showmesh::CoordinatorStatus;
using showmesh::CredentialSource;
using showmesh::HttpRequest;
using showmesh::HttpResponse;
using showmesh::HttpTransport;
using showmesh::IdentityUnavailable;
using showmesh::kMismatchVerdictAgeOutMillis;
using showmesh::PlaylistAction;
using showmesh::PlaylistEntryObservation;
using showmesh::PlaylistMismatchNotifier;
using showmesh::ReportsRefusedNotifier;
using showmesh::RetryPolicy;
using showmesh::ShowMesh_PlaylistMismatch;
using showmesh::ShowMesh_ReportsRefused;
using showmesh::StatusSink;
using showmesh::TimeMillis;

namespace {

TimeMillis gNow = 1'800'000'000'000;
TimeMillis testClock() { return gNow; }

// The Sleeper seam is a function pointer, so the recorded delays live
// here rather than in a fake object. Every test that installs it clears
// it first.
std::vector<int> gSleeps;
void recordSleep(int millis, const std::atomic<bool>* stopRequested) {
    (void)stopRequested;
    gSleeps.push_back(millis);
}

const char* kUuid = "6f1c1a52-1b6c-4b53-9a0e-9f7c2f0d1b44";
const char* kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const char* kEntryKey = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
const char* kBaseUrl = "http://coordinator.invalid:8080";
const char* kToken = "a-bearer-token-that-must-never-be-logged";

// A transport that answers from a script instead of opening a socket.
// This is what keeps the core host neutral: the tests link no HTTP
// library at all, and the concrete client lives beside the adapters.
class FakeTransport : public HttpTransport {
 public:
    HttpResponse post(const HttpRequest& request) override {
        requests.push_back(request);
        if (responses.empty()) return failure;
        HttpResponse response = responses.front();
        if (responses.size() > 1) responses.erase(responses.begin());
        return response;
    }

    // Not exercised by this file's own tests, which only drive
    // CoordinatorClient's post paths; shares post()'s script so the
    // interface's second verb has a stated behavior rather than none.
    HttpResponse get(const HttpRequest& request) override { return post(request); }

    static HttpResponse ok(int statusCode = 200) {
        HttpResponse r;
        r.transportOk = true;
        r.statusCode = statusCode;
        return r;
    }
    static HttpResponse refused(int statusCode, std::string body) {
        HttpResponse r;
        r.transportOk = true;
        r.statusCode = statusCode;
        r.body = std::move(body);
        return r;
    }
    static HttpResponse okWithBody(std::string body, int statusCode = 200) {
        HttpResponse r;
        r.transportOk = true;
        r.statusCode = statusCode;
        r.body = std::move(body);
        return r;
    }
    static HttpResponse unreachable(std::string error) {
        HttpResponse r;
        r.transportOk = false;
        r.error = std::move(error);
        return r;
    }

    std::vector<HttpRequest> requests;
    std::vector<HttpResponse> responses;
    HttpResponse failure = unreachable("connection refused");
};

class FakeCredentials : public CredentialSource {
 public:
    bool token(std::string* out, std::string* error) override {
        ++loads;
        if (!available) {
            if (error != nullptr) *error = failureText;
            return false;
        }
        *out = value;
        return true;
    }
    void invalidate() override { ++invalidations; }

    std::string value = kToken;
    bool available = true;
    std::string failureText = "credential file /etc/showmesh-fpp-plugin/credential does not exist";
    int loads = 0;
    int invalidations = 0;
};

class RecordingStatusSink : public StatusSink {
 public:
    void writeStatus(const std::string& json) override { writes.push_back(json); }
    bool readStatus(std::string* json) override {
        if (!hasPersisted) return false;
        *json = persisted;
        return true;
    }
    std::vector<std::string> writes;
    bool hasPersisted = false;
    std::string persisted;
};

// Records every raise/clear call, exact id and message included, so a
// test can assert this repository's own identity constant and the
// coordinator's own instruction text reached the notifier rather than
// merely that something was raised.
class RecordingMismatchNotifier : public PlaylistMismatchNotifier {
 public:
    struct Call {
        int id;
        std::string message;
    };

    void raiseMismatch(int id, const std::string& message) override { raised.push_back(Call{id, message}); }
    void clearMismatch(int id, const std::string& message) override { cleared.push_back(Call{id, message}); }

    std::vector<Call> raised;
    std::vector<Call> cleared;
};

// Same recording shape as RecordingMismatchNotifier, for the unrelated
// reports-refused notice.
class RecordingReportsRefusedNotifier : public ReportsRefusedNotifier {
 public:
    struct Call {
        int id;
        std::string message;
    };

    void raiseRefused(int id, const std::string& message) override { raised.push_back(Call{id, message}); }
    void clearRefused(int id, const std::string& message) override { cleared.push_back(Call{id, message}); }

    std::vector<Call> raised;
    std::vector<Call> cleared;
};

// A minimal observation receipt body, standing in for the coordinator's
// own PlaylistEntryObservationReceipt: reconciliation and
// operatorInstruction are additive and optional per api/openapi.yaml, so
// a script can omit either to exercise the absent-verdict path.
std::string receiptBody(const char* reconciliation, const char* operatorInstruction) {
    std::string body = "{\"schemaVersion\":1,\"accepted\":true,\"replay\":false";
    if (reconciliation != nullptr) {
        body += ",\"reconciliation\":\"";
        body += reconciliation;
        body += "\"";
    }
    if (operatorInstruction != nullptr) {
        body += ",\"operatorInstruction\":\"";
        body += operatorInstruction;
        body += "\"";
    }
    body += "}";
    return body;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

PlaylistEntryObservation resolvedObservation() {
    PlaylistEntryObservation observation;
    observation.identity.instanceUuid = kUuid;
    observation.identity.playlistName = "Halloween Main";
    observation.identity.playlistHash = kHash;
    observation.identity.section = "mainPlaylist";
    observation.identity.position = 3;
    observation.entryKey = kEntryKey;
    observation.sequenceFilename = "Thriller.fseq";
    observation.action = PlaylistAction::kPlaying;
    observation.sequence = 42;
    observation.observedAtMillis = gNow;
    return observation;
}

RetryPolicy fastPolicy() {
    RetryPolicy policy;
    policy.maxAttempts = 5;
    policy.initialBackoffMillis = 500;
    policy.maxBackoffMillis = 30000;
    return policy;
}

}  // namespace

TEST(APostedObservationCarriesTheContractBodyToTheContractRoute) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::ok(202));
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(transport.requests.size(), std::size_t{1});

    const HttpRequest& request = transport.requests.front();
    CHECK_EQ(request.url, std::string(kBaseUrl) + "/api/v1/integrations/fpp/playlist-entry-observations");
    CHECK_EQ(request.contentType, std::string("application/json"));
    CHECK_EQ(request.bearerToken, std::string(kToken));
    CHECK(contains(request.body, "\"schemaVersion\":1"));
    CHECK(contains(request.body, "\"instanceUuid\":\"" + std::string(kUuid) + "\""));
    CHECK(contains(request.body, "\"playlistName\":\"Halloween Main\""));
    CHECK(contains(request.body, "\"playlistHash\":\"" + std::string(kHash) + "\""));
    CHECK(contains(request.body, "\"entryKey\":\"" + std::string(kEntryKey) + "\""));
    CHECK(contains(request.body, "\"section\":\"mainPlaylist\""));
    CHECK(contains(request.body, "\"position\":3"));
    CHECK(contains(request.body, "\"action\":\"playing\""));
    CHECK(contains(request.body, "\"sequence\":42"));
    CHECK(contains(request.body, "\"coalescedSincePreviousAcknowledged\":0"));
    CHECK(contains(request.body, "\"sequenceFilename\":\"Thriller.fseq\""));
    // Absent rather than present and empty: the entry has no media file.
    CHECK(!contains(request.body, "mediaFilename"));
    CHECK(!contains(request.body, "unavailable"));
    CHECK(gSleeps.empty());

    const CoordinatorStatus status = client.status();
    CHECK_EQ(status.observationsAccepted, std::uint64_t{1});
    CHECK_EQ(status.attempts, std::uint64_t{1});
    CHECK_EQ(status.lastOutcome, std::string("accepted"));
    CHECK(status.lastError.empty());
}

TEST(AnUnavailableObservationCarriesTheWireReasonAndNoDerivedIdentity) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::ok());
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    PlaylistEntryObservation observation;
    observation.identity.instanceUuid = kUuid;
    observation.identity.playlistName = "Halloween Main";
    observation.identity.section = "mainPlaylist";
    observation.identity.position = 3;
    observation.unavailable = IdentityUnavailable::kMissingDefinition;
    observation.action = PlaylistAction::kStart;
    observation.sequence = 7;
    observation.observedAtMillis = gNow;

    CHECK(client.publishUnavailable(observation));
    const std::string& body = transport.requests.front().body;
    CHECK(contains(body, "\"unavailable\":\"missing_definition\""));
    CHECK(contains(body, "\"playlistName\":\"Halloween Main\""));
    // The coordinator refuses an unavailable observation carrying either
    // derived field, because neither can exist without the definition.
    CHECK(!contains(body, "playlistHash"));
    CHECK(!contains(body, "entryKey"));
}

TEST(AnUnauthorizedPostIsNotRetriedAndStaysVisibleInLocalStatus) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(401, "{\"type\":\"unauthorized\"}"));
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(transport.requests.size(), std::size_t{1});
    CHECK(gSleeps.empty());
    // The file may have been replaced with a working credential since it
    // was last read, so the cached copy is dropped.
    CHECK_EQ(credentials.invalidations, 1);

    const CoordinatorStatus status = client.status();
    CHECK_EQ(status.unauthorized, std::uint64_t{1});
    CHECK_EQ(status.observationsRefused, std::uint64_t{1});
    CHECK_EQ(status.lastStatusCode, 401);
    CHECK_EQ(status.lastOutcome, std::string("unauthorized"));
    CHECK(!status.lastError.empty());
}

TEST(AForbiddenPostNamesTheMissingScopeInLocalStatusAndIsNotRetried) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(403, "{\"type\":\"forbidden\",\"scope\":\"fpp:observe\"}"));
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(transport.requests.size(), std::size_t{1});
    CHECK(gSleeps.empty());

    const CoordinatorStatus status = client.status();
    CHECK_EQ(status.forbidden, std::uint64_t{1});
    CHECK_EQ(status.lastStatusCode, 403);
    CHECK_EQ(status.lastOutcome, std::string("forbidden-missing-fpp-observe-scope"));
    CHECK(contains(status.lastError, "fpp:observe"));
}

TEST(ASchemaRefusalIsTerminalAndCountedApartFromAnOutage) {
    FakeTransport transport;
    transport.responses.push_back(
        FakeTransport::refused(400, "{\"type\":\"unsupported-observation-schema-version\"}"));
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(transport.requests.size(), std::size_t{1});
    CHECK(gSleeps.empty());

    const CoordinatorStatus status = client.status();
    CHECK_EQ(status.schemaRefused, std::uint64_t{1});
    CHECK_EQ(status.transportFailures, std::uint64_t{0});
    CHECK_EQ(status.lastOutcome, std::string("schema-refused"));
}

TEST(AnUnreachableCoordinatorRetriesWithBoundedBackoffAndThenGivesUp) {
    FakeTransport transport;  // no scripted responses: always unreachable
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(transport.requests.size(), std::size_t{5});
    CHECK_EQ(gSleeps.size(), std::size_t{4});
    CHECK_EQ(gSleeps[0], 500);
    CHECK_EQ(gSleeps[1], 1000);
    CHECK_EQ(gSleeps[2], 2000);
    CHECK_EQ(gSleeps[3], 4000);

    const CoordinatorStatus status = client.status();
    CHECK_EQ(status.transportFailures, std::uint64_t{5});
    CHECK_EQ(status.retries, std::uint64_t{4});
    CHECK_EQ(status.lastOutcome, std::string("coordinator-unreachable"));
    CHECK_EQ(status.lastStatusCode, 0);
}

TEST(TheBackoffStopsDoublingAtTheCeiling) {
    FakeTransport transport;
    FakeCredentials credentials;
    gSleeps.clear();
    RetryPolicy policy;
    policy.maxAttempts = 6;
    policy.initialBackoffMillis = 10000;
    policy.maxBackoffMillis = 20000;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, policy);

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(gSleeps.size(), std::size_t{5});
    CHECK_EQ(gSleeps[0], 10000);
    CHECK_EQ(gSleeps[1], 20000);
    CHECK_EQ(gSleeps[4], 20000);
}

TEST(AServerErrorIsRetriedAndASubsequentAcceptanceEndsTheAttempts) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(503, "unavailable"));
    transport.responses.push_back(FakeTransport::ok());
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(transport.requests.size(), std::size_t{2});
    CHECK_EQ(gSleeps.size(), std::size_t{1});
    CHECK_EQ(client.status().observationsAccepted, std::uint64_t{1});
}

TEST(TheCoalescedCountRidesTheObservationAndIsAcknowledgedOnlyOnAcceptance) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(400, "refused"));
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    PlaylistEntryObservation observation = resolvedObservation();
    observation.coalescedSincePreviousAcknowledged = 9;
    CHECK(!client.publish(observation));
    CHECK(contains(transport.requests.front().body, "\"coalescedSincePreviousAcknowledged\":9"));
    // A refusal acknowledges nothing, so the local record of the gap does
    // not move either.
    CHECK_EQ(client.status().coalescedAcknowledged, std::uint64_t{0});

    transport.responses.clear();
    transport.responses.push_back(FakeTransport::ok());
    CHECK(client.publish(observation));
    CHECK_EQ(client.status().coalescedAcknowledged, std::uint64_t{9});
}

TEST(ADefinitionIsPostedToItsOwnRouteAndCarriesTheDefinitionItHashed) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::ok());
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    const std::string definition = "{\"mainPlaylist\":[{\"sequenceName\":\"Thriller.fseq\"}],\"name\":\"Halloween\"}";
    CHECK(client.publishDefinition(kUuid, "Halloween", kHash, definition, gNow));

    const HttpRequest& request = transport.requests.front();
    CHECK_EQ(request.url, std::string(kBaseUrl) + "/api/v1/integrations/fpp/playlist-definitions");
    CHECK(contains(request.body, "\"playlistHash\":\"" + std::string(kHash) + "\""));
    CHECK(contains(request.body, "\"playlistName\":\"Halloween\""));
    // The definition travels as the object itself, not as a string
    // holding JSON: the coordinator re-canonicalizes and re-hashes it.
    CHECK(contains(request.body, "\"definition\":{\"mainPlaylist\":[{\"sequenceName\":\"Thriller.fseq\"}]"));
    CHECK(contains(request.body, "\"capturedAtMillis\":" + std::to_string(gNow)));
    CHECK(client.holdsDefinition(kUuid, kHash));
}

TEST(ADefinitionHashAlreadyPostedIsNotPostedAgain) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::ok());
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    const std::string definition = "{\"name\":\"Halloween\"}";
    CHECK(client.publishDefinition(kUuid, "Halloween", kHash, definition, gNow));
    CHECK(client.publishDefinition(kUuid, "Halloween", kHash, definition, gNow));
    CHECK_EQ(transport.requests.size(), std::size_t{1});
    CHECK_EQ(client.status().definitionsAlreadyHeld, std::uint64_t{1});
}

// finding 3 (item 1): a terminal 400 (definition-hash-mismatch, or any
// other schema refusal) for a given (instanceUuid, playlistHash) cannot
// change on a retry of the identical bytes: playlistHash is the hash of
// the exact canonicalDefinition being posted, so a second call with the
// same key necessarily carries the same content. Retrying it anyway
// re-sent, in full, on every subsequent callback for that playlist,
// ahead of the observation it blocked, for a result that could never
// change. It must be remembered and skipped, exactly like a held
// (accepted) definition is.
TEST(ADefinitionTheCoordinatorRefusedTerminallyIsNotRetriedWithTheSameContent) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(400, "definition-hash-mismatch"));
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    const std::string definition = "{\"name\":\"Halloween\"}";
    CHECK(!client.publishDefinition(kUuid, "Halloween", kHash, definition, gNow));
    CHECK(!client.holdsDefinition(kUuid, kHash));

    // The coordinator would now accept it, but the plugin never asks
    // again: the next attempt is answered from the local negative cache,
    // without a second request.
    transport.responses.clear();
    transport.responses.push_back(FakeTransport::ok());
    CHECK(!client.publishDefinition(kUuid, "Halloween", kHash, definition, gNow));
    CHECK_EQ(transport.requests.size(), std::size_t{1});
    CHECK_EQ(client.status().definitionsRefused, std::uint64_t{2});
}

TEST(ADefinitionOverTheContractBoundIsRefusedLocallyWithoutARequest) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::ok());
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    std::string huge = "{\"name\":\"";
    huge.append(showmesh::kDefinitionBodyLimitBytes + 16, 'x');
    huge += "\"}";
    CHECK(!client.publishDefinition(kUuid, "Halloween", kHash, huge, gNow));
    CHECK(transport.requests.empty());
    CHECK_EQ(client.status().definitionsRefused, std::uint64_t{1});
}

TEST(ACredentialThatWillNotLoadIsAVisibleConfigurationFailureNotARetryLoop) {
    FakeTransport transport;
    FakeCredentials credentials;
    credentials.available = false;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    CHECK(!client.publish(resolvedObservation()));
    CHECK(transport.requests.empty());
    CHECK(gSleeps.empty());

    const CoordinatorStatus status = client.status();
    CHECK(!status.configured);
    CHECK_EQ(status.lastOutcome, std::string("no-credential"));
    CHECK(contains(status.configurationError, "credential file"));
    // The credential itself never reaches the local record.
    CHECK(!contains(status.configurationError, kToken));
    CHECK(!contains(status.lastError, kToken));
}

// finding 5: status_.configured was only ever set true in the
// constructor, and a recorded configurationError was never cleared on a
// later success. A plugin that started before the credential file
// existed kept reporting "configured: false" and the stale "credential
// file does not exist" text forever, even once posts were succeeding.
TEST(AConfigurationProblemThatClearsIsReflectedOnceAPostSucceeds) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::unreachable("connection refused"));
    FakeCredentials credentials;
    credentials.available = false;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    CHECK(!client.publish(resolvedObservation()));
    CHECK(!client.status().configured);
    CHECK(contains(client.status().configurationError, "credential file"));

    // The credential file appears; the next post succeeds.
    credentials.available = true;
    transport.responses.clear();
    transport.responses.push_back(FakeTransport::ok());
    CHECK(client.publish(resolvedObservation()));

    const CoordinatorStatus status = client.status();
    CHECK(status.configured);
    CHECK(status.configurationError.empty());
}

TEST(AClientWithNoCoordinatorUrlRefusesVisiblyRatherThanPosting) {
    FakeTransport transport;
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, std::string(), testClock, nullptr, recordSleep, fastPolicy());
    client.setConfigurationError("coordinator config file /tmp/config.json has no coordinatorUrl key");

    CHECK(!client.publish(resolvedObservation()));
    CHECK(transport.requests.empty());

    const CoordinatorStatus status = client.status();
    CHECK(!status.configured);
    CHECK_EQ(status.lastOutcome, std::string("not-configured"));
    CHECK(contains(status.configurationError, "coordinatorUrl"));
}

TEST(LocalStatusIsWrittenWhenTheOutcomeChangesAndCarriesNoCredential) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::ok());
    FakeCredentials credentials;
    RecordingStatusSink statusSink;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, &statusSink, recordSleep, fastPolicy());

    CHECK(client.publish(resolvedObservation()));
    CHECK(!statusSink.writes.empty());
    const std::string& first = statusSink.writes.back();
    CHECK(contains(first, "\"lastOutcome\":\"accepted\""));
    CHECK(!contains(first, kToken));

    transport.responses.clear();
    transport.responses.push_back(FakeTransport::refused(403, "forbidden"));
    CHECK(!client.publish(resolvedObservation()));
    const std::string& second = statusSink.writes.back();
    CHECK(contains(second, "forbidden-missing-fpp-observe-scope"));
    CHECK(contains(second, "\"forbidden\":1"));
}

TEST(AnObservationWithNoInstanceUuidIsRefusedLocallyRatherThanSent) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::ok());
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    PlaylistEntryObservation observation = resolvedObservation();
    observation.identity.instanceUuid.clear();
    CHECK(!client.publish(observation));
    CHECK(transport.requests.empty());
    CHECK_EQ(client.status().lastOutcome, std::string("payload-refused-locally"));
}

// This client decides nothing about a mismatch itself: it mirrors
// whatever reconciliation verdict the coordinator's own observation
// receipt carries. These tests exercise that mirroring directly through
// RecordingMismatchNotifier; the real WarningHolder call is only
// reachable with FPP's own headers and is proven by the container bench
// instead (see scripts/test-plugin-load-fpp.sh assertion A9).

TEST(TheNoticeIsRaisedWithTheCoordinatorsOwnOperatorInstructionWhileTheVerdictIsAMismatch) {
    FakeTransport transport;
    transport.responses.push_back(
        FakeTransport::okWithBody(receiptBody("stale-import", "Re-import the playlist in the coordinator.")));
    FakeCredentials credentials;
    RecordingMismatchNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             &notifier);

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});
    CHECK_EQ(notifier.raised[0].id, ShowMesh_PlaylistMismatch);
    CHECK_EQ(notifier.raised[0].message, std::string("Re-import the playlist in the coordinator."));
    CHECK(notifier.cleared.empty());
}

TEST(TheNoticeClearsWhenTheVerdictSaysResolved) {
    FakeTransport transport;
    transport.responses.push_back(
        FakeTransport::okWithBody(receiptBody("stale-import", "Re-import the playlist in the coordinator.")));
    transport.responses.push_back(FakeTransport::okWithBody(receiptBody("resolved", nullptr)));
    FakeCredentials credentials;
    RecordingMismatchNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             &notifier);

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(notifier.cleared.size(), std::size_t{1});
    CHECK_EQ(notifier.cleared[0].id, ShowMesh_PlaylistMismatch);
    CHECK_EQ(notifier.cleared[0].message, std::string("Re-import the playlist in the coordinator."));
}

// THE TRAP: reconciliation and operatorInstruction are both best effort
// and are omitted, with the receipt still a 200, whenever the
// coordinator's own lookup fails. Absent must never read as resolved: a
// client that cleared here would silently drop a warning that should
// still stand.
TEST(AnAbsentVerdictDoesNotClearAStandingMismatchNotice) {
    FakeTransport transport;
    transport.responses.push_back(
        FakeTransport::okWithBody(receiptBody("stale-import", "Re-import the playlist in the coordinator.")));
    transport.responses.push_back(FakeTransport::okWithBody(receiptBody(nullptr, nullptr)));
    FakeCredentials credentials;
    RecordingMismatchNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             &notifier);

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});

    CHECK(client.publish(resolvedObservation()));
    CHECK(notifier.cleared.empty());
}

TEST(TheNoticeAgesOutOnceTheCoordinatorHasBeenUnreachableLongEnough) {
    FakeTransport transport;
    transport.responses.push_back(
        FakeTransport::okWithBody(receiptBody("stale-import", "Re-import the playlist in the coordinator.")));
    FakeCredentials credentials;
    RecordingMismatchNotifier notifier;
    gNow = 1'800'000'000'000;
    RetryPolicy policy;
    policy.maxAttempts = 1;  // one attempt per publish() call below, so each advances gNow deterministically
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, policy, &notifier);

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});

    // The coordinator goes unreachable; every further attempt fails, but
    // not enough time has passed yet.
    transport.responses.clear();
    gNow += kMismatchVerdictAgeOutMillis - 1;
    CHECK(!client.publish(resolvedObservation()));
    CHECK(notifier.cleared.empty());

    // Now it has.
    gNow += 2;
    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.cleared.size(), std::size_t{1});
    CHECK_EQ(notifier.cleared[0].id, ShowMesh_PlaylistMismatch);
    CHECK_EQ(notifier.cleared[0].message, std::string("Re-import the playlist in the coordinator."));

    gNow = 1'800'000'000'000;  // restore the shared clock for later tests
}

// THE SECOND TRAP: WarningHolder matches the exact (id, message, plugin)
// triple. If the coordinator's operatorInstruction text ever differs
// between the raise and the clear, clearing with the newly received
// string would silently fail against the real WarningHolder and leave a
// permanent notice. This client must always clear with the message it
// itself raised.
TEST(TheNoticeClearsWithTheRaisedMessageEvenWhenTheInstructionTextChangedInBetween) {
    FakeTransport transport;
    transport.responses.push_back(
        FakeTransport::okWithBody(receiptBody("stale-import", "Re-import the playlist in the coordinator.")));
    // Still mismatched, but the coordinator now describes it differently
    // (a different reconciliation outcome, still one that carries an
    // instruction).
    transport.responses.push_back(
        FakeTransport::okWithBody(receiptBody("cross-show", "Restart FPP so its binding matches this show.")));
    FakeCredentials credentials;
    RecordingMismatchNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             &notifier);

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised[0].message, std::string("Re-import the playlist in the coordinator."));

    CHECK(client.publish(resolvedObservation()));
    // The old message is cleared before the new one is raised, so a
    // WarningHolder-backed notifier never shows two differently worded
    // notices for the same mismatch.
    CHECK_EQ(notifier.cleared.size(), std::size_t{1});
    CHECK_EQ(notifier.cleared[0].message, std::string("Re-import the playlist in the coordinator."));
    CHECK_EQ(notifier.raised.size(), std::size_t{2});
    CHECK_EQ(notifier.raised[1].message, std::string("Restart FPP so its binding matches this show."));
}

// The contract pairs a mismatch outcome with a non-empty
// operatorInstruction and omits both together otherwise, so this should
// not happen. But a receipt is operator-visible surface: raising with an
// empty message would be a blank, unexplained entry in FPP's own
// notification centre, worse than not raising at all.
TEST(AMismatchOutcomeWithNoOperatorInstructionIsNotRaised) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::okWithBody(receiptBody("stale-import", "")));
    FakeCredentials credentials;
    RecordingMismatchNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             &notifier);

    CHECK(client.publish(resolvedObservation()));
    CHECK(notifier.raised.empty());
    CHECK(notifier.cleared.empty());
}

TEST(TheReportsRefusedNoticeIsRaisedOnAConflict) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(409, "playlist observation sequence regression"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});
    CHECK_EQ(notifier.raised[0].id, ShowMesh_ReportsRefused);
    CHECK(contains(notifier.raised[0].message, "playlist observation sequence regression"));
    CHECK(contains(notifier.raised[0].message, "reset-observation-sequence"));
    CHECK(notifier.cleared.empty());

    const CoordinatorStatus status = client.status();
    CHECK_EQ(status.reportsRefusedReason, notifier.raised[0].message);
}

TEST(TheReportsRefusedNoticeIsNotRaisedTwiceForTheSameReason) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(409, "playlist observation sequence regression"));
    transport.responses.push_back(FakeTransport::refused(409, "playlist observation sequence regression"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});
    CHECK(notifier.cleared.empty());
}

TEST(TheReportsRefusedNoticeIsReplacedWhenTheReasonChanges) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(409, "playlist observation sequence regression"));
    transport.responses.push_back(FakeTransport::refused(401, "the bearer token was rejected"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.cleared.size(), std::size_t{1});
    CHECK_EQ(notifier.cleared[0].message, notifier.raised[0].message);
    CHECK_EQ(notifier.raised.size(), std::size_t{2});
    CHECK(notifier.raised[1].message != notifier.raised[0].message);
}

TEST(TheReportsRefusedNoticeClearsOnAcceptance) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(409, "playlist observation sequence regression"));
    transport.responses.push_back(FakeTransport::ok(202));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(notifier.cleared.size(), std::size_t{1});
    CHECK_EQ(notifier.cleared[0].message, notifier.raised[0].message);

    const CoordinatorStatus status = client.status();
    CHECK(status.reportsRefusedReason.empty());
}

// finding 1: a problem+json body's "detail" field is the reason, not the
// full raw body (which can carry an unbounded, machine-readable envelope
// around it).
TEST(TheReportsRefusedReasonIsTheProblemBodysDetailField) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(
        409,
        "{\"type\":\"https://showmesh.example/problems/sequence-regression\",\"title\":\"Conflict\","
        "\"detail\":\"playlist observation sequence regression\",\"instance\":\"req-1\"}"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});
    CHECK(contains(notifier.raised[0].message, "playlist observation sequence regression"));
    CHECK(!contains(notifier.raised[0].message, "req-1"));
}

// A body carrying no "detail" falls back to "title".
TEST(TheReportsRefusedReasonFallsBackToTheProblemBodysTitleField) {
    FakeTransport transport;
    transport.responses.push_back(
        FakeTransport::refused(403, "{\"type\":\"https://showmesh.example/problems/forbidden\",\"title\":"
                                     "\"missing fpp:observe scope\"}"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK(contains(notifier.raised[0].message, "missing fpp:observe scope"));
}

// A body carrying neither field falls back to the status code, so the
// notice is never blank.
TEST(TheReportsRefusedReasonFallsBackToTheStatusCodeWhenTheBodyHasNoUsableField) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(409, "{\"type\":\"opaque\"}"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK(contains(notifier.raised[0].message, "409"));
}

// A proxy's HTML error page is stripped of markup rather than dumped
// verbatim into the operator-visible notice.
TEST(TheReportsRefusedReasonStripsHtmlMarkupFromANonJsonBody) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(
        409, "<html><body><h1>409 Conflict</h1><p>nginx</p></body></html>"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK(!contains(notifier.raised[0].message, "<"));
    CHECK(!contains(notifier.raised[0].message, ">"));
    CHECK(contains(notifier.raised[0].message, "409 Conflict"));
    CHECK(contains(notifier.raised[0].message, "nginx"));
}

// A body over 200 characters is capped, so the notice stays a fixed,
// bounded size regardless of what the coordinator (or a proxy in front of
// it) sends.
TEST(TheReportsRefusedReasonIsCappedAt200Characters) {
    FakeTransport transport;
    const std::string longDetail(400, 'x');
    transport.responses.push_back(
        FakeTransport::refused(409, "{\"detail\":\"" + longDetail + "\"}"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    const std::string reasonPart = notifier.raised[0].message.substr(0, 200);
    CHECK_EQ(reasonPart, std::string(200, 'x'));
}

// The extracted reason is stable across retries even when the raw body
// varies (a different request id each attempt), so the notice is not
// cleared and re-raised on every post.
TEST(TheReportsRefusedNoticeIsNotRaisedTwiceWhenOnlyTheBodysEnvelopeVaries) {
    FakeTransport transport;
    transport.responses.push_back(
        FakeTransport::refused(409, "{\"detail\":\"playlist observation sequence regression\",\"instance\":\"a\"}"));
    transport.responses.push_back(
        FakeTransport::refused(409, "{\"detail\":\"playlist observation sequence regression\",\"instance\":\"b\"}"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});
    CHECK(notifier.cleared.empty());
}

TEST(TheReportsRefusedNoticeIsRaisedOnATransportFailure) {
    FakeTransport transport;  // no scripted responses: always unreachable
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    RetryPolicy policy;
    policy.maxAttempts = 1;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, policy, nullptr,
                             &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});
    CHECK_EQ(notifier.raised[0].id, ShowMesh_ReportsRefused);
    CHECK(contains(notifier.raised[0].message, "reset-observation-sequence"));
}

TEST(TheReportsRefusedNoticeIsRaisedOnAForbiddenRefusal) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(403, "{\"scope\":\"fpp:observe\"}"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(notifier.raised.size(), std::size_t{1});
    CHECK_EQ(notifier.raised[0].id, ShowMesh_ReportsRefused);
}

// stopped, no-credential, and schema-refused are outcomes this notice does
// not cover: an operator cannot resolve a schema refusal by clearing a
// sequence, and stopped/no-credential are local conditions, not the
// coordinator refusing anything.
TEST(TheReportsRefusedNoticeIsUntouchedByAStoppedOutcome) {
    FakeTransport transport;
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);
    client.requestStop();

    CHECK(!client.publish(resolvedObservation()));
    CHECK(notifier.raised.empty());
    CHECK(notifier.cleared.empty());
}

TEST(TheReportsRefusedNoticeIsUntouchedByANoCredentialOutcome) {
    FakeTransport transport;
    FakeCredentials credentials;
    credentials.available = false;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK(notifier.raised.empty());
    CHECK(notifier.cleared.empty());
}

TEST(TheReportsRefusedNoticeIsUntouchedByASchemaRefusedOutcome) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(400, "unsupported-observation-schema-version"));
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(!client.publish(resolvedObservation()));
    CHECK(notifier.raised.empty());
    CHECK(notifier.cleared.empty());
}

// finding 2: after an fppd restart with a refusal standing, the notice
// must come back from persisted status at construction, before any post
// happens, rather than waiting for the next refused post.
TEST(AConstructedClientRestoresAndRaisesTheReportsRefusedNoticeFromPersistedStatus) {
    FakeTransport transport;
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    RecordingStatusSink statusSink;
    statusSink.hasPersisted = true;
    statusSink.persisted =
        "{\"schemaVersion\":1,\"lastOutcome\":\"conflict\",\"reportsRefusedReason\":"
        "\"playlist observation sequence regression. Clear the playlist observation on the coordinator's "
        "Monitor screen, or run showmeshctl fpp reset-observation-sequence\"}";

    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, &statusSink, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK_EQ(notifier.raised.size(), std::size_t{1});
    CHECK_EQ(notifier.raised[0].id, ShowMesh_ReportsRefused);
    CHECK(contains(notifier.raised[0].message, "playlist observation sequence regression"));
    CHECK(client.status().reportsRefusedReason == notifier.raised[0].message);
    CHECK(client.status().lastOutcome == std::string("conflict"));

    // The next accepted post clears exactly the restored message.
    transport.responses.push_back(FakeTransport::ok());
    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(notifier.cleared.size(), std::size_t{1});
    CHECK_EQ(notifier.cleared[0].message, notifier.raised[0].message);
}

// A persisted accepted outcome restores no notice.
TEST(AConstructedClientRaisesNoNoticeFromAnAcceptedPersistedStatus) {
    FakeTransport transport;
    FakeCredentials credentials;
    RecordingReportsRefusedNotifier notifier;
    RecordingStatusSink statusSink;
    statusSink.hasPersisted = true;
    statusSink.persisted = "{\"schemaVersion\":1,\"lastOutcome\":\"accepted\"}";

    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, &statusSink, recordSleep, fastPolicy(),
                             nullptr, &notifier);

    CHECK(notifier.raised.empty());
    CHECK(client.status().reportsRefusedReason.empty());
}
