#include "showmesh/coordinator_client.h"

#include <string>
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
using showmesh::PlaylistAction;
using showmesh::PlaylistEntryObservation;
using showmesh::RetryPolicy;
using showmesh::StatusSink;
using showmesh::TimeMillis;

namespace {

TimeMillis gNow = 1'800'000'000'000;
TimeMillis testClock() { return gNow; }

// The Sleeper seam is a function pointer, so the recorded delays live
// here rather than in a fake object. Every test that installs it clears
// it first.
std::vector<int> gSleeps;
void recordSleep(int millis) { gSleeps.push_back(millis); }

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
    std::vector<std::string> writes;
};

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

TEST(ADefinitionTheCoordinatorRefusedIsPostedAgainOnTheNextSweep) {
    FakeTransport transport;
    transport.responses.push_back(FakeTransport::refused(400, "definition-hash-mismatch"));
    FakeCredentials credentials;
    gSleeps.clear();
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, recordSleep, fastPolicy());

    const std::string definition = "{\"name\":\"Halloween\"}";
    CHECK(!client.publishDefinition(kUuid, "Halloween", kHash, definition, gNow));
    CHECK(!client.holdsDefinition(kUuid, kHash));

    transport.responses.clear();
    transport.responses.push_back(FakeTransport::ok());
    CHECK(client.publishDefinition(kUuid, "Halloween", kHash, definition, gNow));
    CHECK_EQ(transport.requests.size(), std::size_t{2});
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
