#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "showmesh/coordinator_config.h"
#include "showmesh/http_transport.h"
#include "showmesh/runtime.h"

// The plugin's sending half: the authenticated outbound client that
// carries a resolved playlist-entry observation and the playlist
// definition behind its hash to the coordinator.
//
// Everything here runs on the resident worker thread. Nothing here is
// reachable from FPP's callback thread, which copies bounded evidence
// into the handoff and returns.

namespace showmesh {

// Bounded backoff. Bounded in both directions: a capped delay so a long
// outage does not push the next attempt hours out, and a capped attempt
// count so one unreachable coordinator cannot hold the worker off the
// queue indefinitely. When the attempts run out the observation is
// abandoned, its gap evidence stays unacknowledged, and the handoff
// coalesces to the newest state behind it, which is the designed
// behavior rather than a failure of it.
struct RetryPolicy {
    int maxAttempts = 5;
    int initialBackoffMillis = 500;
    int maxBackoffMillis = 30000;
};

using Sleeper = void (*)(int millis);

void sleepMillis(int millis);

// What an operator can see locally without the coordinator's help. A
// 401, a 403, a schema refusal, and an unreachable coordinator are four
// different problems with four different fixes, so they are counted
// apart rather than summed into one failure total.
struct CoordinatorStatus {
    bool configured = false;
    // Why the client cannot post at all: a missing coordinator URL, or a
    // credential file that does not exist or carries the wrong mode.
    // Never contains the credential.
    std::string configurationError;

    std::uint64_t attempts = 0;
    std::uint64_t retries = 0;
    std::uint64_t observationsAccepted = 0;
    std::uint64_t observationsRefused = 0;
    std::uint64_t definitionsAccepted = 0;
    std::uint64_t definitionsRefused = 0;
    std::uint64_t definitionsAlreadyHeld = 0;

    std::uint64_t unauthorized = 0;
    std::uint64_t forbidden = 0;
    std::uint64_t schemaRefused = 0;
    std::uint64_t conflicts = 0;
    std::uint64_t payloadTooLarge = 0;
    std::uint64_t transportFailures = 0;

    // Gap evidence the coordinator has acknowledged. A nonzero value here
    // is the local half of the same record the coordinator holds: it says
    // observations were coalesced away under pressure, not that they were
    // delivered.
    std::uint64_t coalescedAcknowledged = 0;

    int lastStatusCode = 0;
    std::string lastOutcome;
    // Operator-facing text for the last failure. Never the credential.
    std::string lastError;
    TimeMillis lastSuccessAtMillis = 0;
    TimeMillis lastFailureAtMillis = 0;
};

// StatusSink is where the local status record goes. Separate from the
// client so a test reads the record without touching a filesystem, and
// so an adapter can point it at the plugin's own state directory.
class StatusSink {
 public:
    virtual ~StatusSink() = default;
    virtual void writeStatus(const std::string& json) = 0;
};

// Writes <stateDir>/observation-status.json, replacing it atomically.
// The credential never appears in it.
class FileStatusSink : public StatusSink {
 public:
    explicit FileStatusSink(std::string stateDir);
    void writeStatus(const std::string& json) override;

 private:
    std::string path_;
};

std::string renderCoordinatorStatus(const CoordinatorStatus& status);

class CoordinatorClient : public ObservationSink, public DefinitionPublisher {
 public:
    // baseUrl empty, or a null credential source, is a configured-wrong
    // client rather than a crash: every post then fails visibly with the
    // configuration error, which is what an operator needs to see.
    CoordinatorClient(HttpTransport* transport, CredentialSource* credentials, std::string baseUrl, Clock clock,
                      StatusSink* statusSink = nullptr, Sleeper sleeper = sleepMillis,
                      RetryPolicy policy = RetryPolicy());

    // Records why the client cannot post, for a configuration failure the
    // caller detected (a config.json that would not load, for instance).
    void setConfigurationError(std::string error);

    bool publish(const PlaylistEntryObservation& observation) override;
    bool publishUnavailable(const PlaylistEntryObservation& observation) override;
    bool publishDefinition(const std::string& instanceUuid, const std::string& playlistName,
                           const std::string& playlistHash, const std::string& canonicalDefinition,
                           TimeMillis capturedAtMillis) override;

    CoordinatorStatus status() const;
    bool holdsDefinition(const std::string& instanceUuid, const std::string& playlistHash) const;

 private:
    struct Outcome {
        bool accepted = false;
        int statusCode = 0;
        std::string label;
        std::string error;
    };

    bool sendObservation(const PlaylistEntryObservation& observation);
    Outcome postWithRetry(const char* path, const std::string& body);
    void publishStatus();

    HttpTransport* transport_;
    CredentialSource* credentials_;
    std::string baseUrl_;
    Clock clock_;
    StatusSink* statusSink_;
    Sleeper sleeper_;
    RetryPolicy policy_;

    mutable std::mutex mutex_;
    CoordinatorStatus status_;
    // Which definitions the coordinator already holds. In memory only, on
    // purpose: the route is content addressed and idempotent, so a
    // restart re-posting costs one request per playlist and nothing else.
    std::set<std::pair<std::string, std::string>> heldDefinitions_;
    std::string lastWrittenStatus_;
};

}  // namespace showmesh
