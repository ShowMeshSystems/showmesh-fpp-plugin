#pragma once

#include <atomic>
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

// stopRequested is polled during the sleep, not just before and after it,
// so a real implementation can wake promptly instead of sleeping out its
// full argument once a caller asks the retry loop to give up. Never
// null: postWithRetry always passes its own stop flag.
using Sleeper = void (*)(int millis, const std::atomic<bool>* stopRequested);

void sleepMillis(int millis, const std::atomic<bool>* stopRequested);

// How long an active mismatch notice survives with no further word from
// the coordinator before this client clears it on its own. A genuine
// mismatch's notice then disappears while the coordinator is unreachable,
// rather than standing forever on stale evidence; an operator who checks
// FPP's own warnings list during a long outage sees the notice go away
// even though nothing was actually fixed. The next observation that
// reaches the coordinator re-evaluates and can re-raise it.
constexpr TimeMillis kMismatchVerdictAgeOutMillis = 300000;

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
    // Non-empty exactly while the reports-refused notice is raised: the
    // same text given to ReportsRefusedNotifier. Empty otherwise.
    std::string reportsRefusedReason;
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
    // Returns the last written record, or false when none exists yet.
    // Defaulted so an existing test sink that only writes still compiles.
    virtual bool readStatus(std::string*) { return false; }
};

// Writes <stateDir>/observation-status.json, replacing it atomically.
// The credential never appears in it.
class FileStatusSink : public StatusSink {
 public:
    explicit FileStatusSink(std::string stateDir);
    void writeStatus(const std::string& json) override;
    bool readStatus(std::string* json) override;

 private:
    std::string path_;
};

std::string renderCoordinatorStatus(const CoordinatorStatus& status);

class CoordinatorClient : public ObservationSink, public DefinitionPublisher {
 public:
    // baseUrl empty, or a null credential source, is a configured-wrong
    // client rather than a crash: every post then fails visibly with the
    // configuration error, which is what an operator needs to see.
    // mismatchNotifier is where the coordinator's own reconciliation
    // verdict on this instance is mirrored; nullptr keeps the previous
    // behavior (no notification). See applyMismatchVerdict() below for
    // why this lives here rather than in ShowMeshRuntime: the verdict
    // arrives on the observation POST's own response body, which only
    // this client ever sees.
    CoordinatorClient(HttpTransport* transport, CredentialSource* credentials, std::string baseUrl, Clock clock,
                      StatusSink* statusSink = nullptr, Sleeper sleeper = sleepMillis,
                      RetryPolicy policy = RetryPolicy(), PlaylistMismatchNotifier* mismatchNotifier = nullptr,
                      ReportsRefusedNotifier* reportsRefusedNotifier = nullptr);

    // Records why the client cannot post, for a configuration failure the
    // caller detected (a config.json that would not load, for instance).
    void setConfigurationError(std::string error);

    bool publish(const PlaylistEntryObservation& observation) override;
    bool publishUnavailable(const PlaylistEntryObservation& observation) override;
    bool publishDefinition(const std::string& instanceUuid, const std::string& playlistName,
                           const std::string& playlistHash, const std::string& canonicalDefinition,
                           TimeMillis capturedAtMillis) override;

    // Overrides both ObservationSink's and DefinitionPublisher's
    // requestStop(): one flag, checked by postWithRetry between attempts
    // and by the sleeper during a backoff wait.
    void requestStop() override { stopRequested_.store(true, std::memory_order_relaxed); }

    // Contract section 3.9's republish, called from fppd's own web thread
    // rather than the worker: both take mutex_, which every other read and
    // write of these two sets already takes.
    DefinitionHoldings clearHeldDefinitions() override;
    DefinitionHoldings definitionHoldings() const override;

    CoordinatorStatus status() const;
    bool holdsDefinition(const std::string& instanceUuid, const std::string& playlistHash) const;
    // True once a definition with this exact content has been refused
    // for a reason that will not change without a plugin restart (its
    // own JSON was rejected, or the coordinator refused the hash it
    // declares). Content addressed, exactly like heldDefinitions_.
    bool definitionIsRefusedTerminally(const std::string& instanceUuid, const std::string& playlistHash) const;

 private:
    struct Outcome {
        bool accepted = false;
        int statusCode = 0;
        std::string label;
        std::string error;
        // The raw response body, captured only on acceptance: that is
        // the only case the observation-receipt schema (and its
        // reconciliation/operatorInstruction fields) applies to.
        std::string body;
    };

    bool sendObservation(const PlaylistEntryObservation& observation);
    Outcome postWithRetry(const char* path, const std::string& body);
    void publishStatus();

    // Called once per accepted observation receipt. Parses reconciliation
    // and operatorInstruction out of body and raises, clears, or leaves
    // the notice untouched. Absent is not resolved: a receipt the
    // coordinator could not compute a verdict for (a failed lookup,
    // reported best effort by an otherwise-successful 200) must never be
    // read as "mismatch cleared", so a missing or unrecognized
    // reconciliation value leaves whatever notice state already stands.
    void applyMismatchVerdict(const std::string& body, TimeMillis now);
    // Called once per refused or unreachable post attempt. The notice
    // ages out only from here, never from applyMismatchVerdict(): an
    // accepted receipt with no verdict already proves the coordinator is
    // reachable, so it must not itself count toward the age-out clock.
    void checkMismatchAgeOut(TimeMillis now);
    // Clears the previous raised message first if it differs from
    // instruction, so a WarningHolder-backed notifier is never left
    // holding two differently worded notices for the same triple.
    void raiseMismatchNotice(const std::string& instruction);
    // Always clears with lastRaisedMessage_, the text this client itself
    // raised, never with any newly received text: an implementation
    // backed by WarningHolder's exact-triple match would otherwise leave
    // a permanent notice if the coordinator's wording ever changed
    // between the raise and the clear.
    void clearMismatchNotice();

    // Called once per sendObservation() outcome and once at construction
    // from persisted status. Raises on a conflict, an unauthorized or
    // forbidden refusal, or a transport failure; clears on acceptance.
    void raiseReportsRefusedNotice(const std::string& message);
    void clearReportsRefusedNotice();
    // Restores lastOutcome and reportsRefusedReason from statusSink_'s
    // persisted record, and re-raises the notice if the restored outcome
    // is still a refusal. Called once, from the constructor.
    void restoreFromPersistedStatus();

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
    // A definition hash the coordinator has terminally refused (its own
    // JSON was unusable, or the coordinator rejected the declared hash,
    // §3.5's definition-hash-mismatch). Neither reason changes on a
    // retry of the same bytes, so without this a definition that will
    // never be accepted was re-sent, in full, on every subsequent
    // callback for its playlist, ahead of the observation it blocked. In
    // memory only, like heldDefinitions_: a restart is a fresh attempt.
    std::set<std::pair<std::string, std::string>> refusedDefinitions_;
    std::string lastWrittenStatus_;
    // Set by requestStop(), read by postWithRetry() and the sleeper. See
    // requestStop() above.
    std::atomic<bool> stopRequested_{false};

    // Worker-thread only, like everything else in this class: publish()
    // and publishUnavailable() are only ever called from ShowMeshRuntime's
    // resident worker thread.
    PlaylistMismatchNotifier* mismatchNotifier_;
    // Whether mismatchNotifier_ currently believes the notice is raised.
    bool mismatchActive_ = false;
    // The exact message last given to raiseMismatch(); clearMismatch()
    // must always be called with this, not with any later text. See
    // clearMismatchNotice().
    std::string lastRaisedMessage_;
    // The last time an observation POST reached the coordinator and got
    // an accepted receipt back, whatever the receipt said. Age-out
    // compares against this, not against the last time a verdict field
    // was actually present, so a reachable coordinator whose own verdict
    // lookup keeps failing does not itself trigger an age-out clear.
    TimeMillis lastVerdictAtMillis_ = 0;

    // Worker-thread only, like mismatchNotifier_ above.
    ReportsRefusedNotifier* reportsRefusedNotifier_;
    // Whether reportsRefusedNotifier_ currently believes the notice is
    // raised, and the exact message it was last raised with. See
    // mismatchActive_ / lastRaisedMessage_ above.
    bool reportsRefusedActive_ = false;
    std::string lastRaisedReportsRefusedMessage_;
};

}  // namespace showmesh
