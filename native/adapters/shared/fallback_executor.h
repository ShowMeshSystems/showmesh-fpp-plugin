#pragma once

// The plugin's ADR-048 fallback executor (Track J, J4). tick() runs on this
// class's own thread and observeEntryEvent() on the runtime worker; neither is
// FPP's callback thread. The README's "Fallback executor" section has the rest.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fallback_activation_resolver.h"
#include "fallback_executor_key.h"
#include "fallback_node_delivery.h"
#include "fallback_outage_detector.h"
#include "fallback_pinned_key_loader.h"
#include "fallback_program_fetch.h"
#include "fallback_program_installer.h"
#include "showmesh/atomic_write.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/http_transport.h"
#include "showmesh/json.h"
#include "showmesh/pairing.h"
#include "showmesh/runtime.h"
#include "showmesh/sha256.h"

namespace showmesh {
namespace fallback {

// HYPOTHESIS, not a measurement: how often the program is asked for again.
constexpr int kHypothesisProgramRefetchIntervalMillis = 60000;
constexpr const char* kProgramRefetchIntervalSettingName = "ShowMeshFallbackProgramRefetchIntervalMillis";
// The refetch is never later than this fraction of the installed program's
// own validity (expiresAt minus compiledAt).
constexpr int kRefetchValidityNumerator = 1;
constexpr int kRefetchValidityDenominator = 3;
// A registration that makes no progress is tried again after one refetch
// interval, then double that each time, up to this cap.
constexpr TimeMillis kRegistrationBackoffCapMillis = 600000;
constexpr int kExecutorRegistrationTimeoutMillis = 10000;
// 409: the coordinator has not read this player's identity yet. It clears by waiting.
constexpr int kRegistrationNotYetStatus = 409;
constexpr std::size_t kRecentActivationRecords = 50;
constexpr std::size_t kRecentProgramHandOffRecords = 20;
constexpr const char* kFallbackStatusFilename = "fallback-status.json";

// Outcome words this player records when it decided itself, in the same shape
// as the words a node sends.
constexpr const char* kOutcomeNoCoordinatorKey = "no-coordinator-key";
constexpr const char* kOutcomeNoProgram = "no-program";
constexpr const char* kOutcomeProgramNotVerified = "program-not-verified";
constexpr const char* kOutcomeProgramExpired = "program-expired";
constexpr const char* kOutcomeUnknownEntry = "unknown-entry";
constexpr const char* kOutcomeAmbiguousEntry = "ambiguous-entry";
constexpr const char* kOutcomeNoTarget = "no-target";
constexpr const char* kOutcomeNoPlayerKey = "no-player-key";
constexpr const char* kOutcomePlayerKeyNotInProgram = "player-key-not-in-program";
constexpr const char* kOutcomeNoAddress = "no-address";
constexpr const char* kOutcomeIncompleteProgram = "incomplete-program";
constexpr const char* kOutcomeEntryNotIdentified = "entry-not-identified";
constexpr const char* kOutcomeSigningFailed = "signing-failed";

// What an operator reads for one of this player's own outcome words: a whole
// sentence for the status file, and the clause the notice puts after "because".
struct PlayerOutcomeCopy {
    const char* word;
    const char* reason;
    const char* noticeClause;
};

constexpr PlayerOutcomeCopy kPlayerOutcomeCopy[] = {
    {kOutcomeNoCoordinatorKey,
     "This player has no coordinator key to check a plan with. Install the coordinator's key on this player.",
     "it has no coordinator key to check a plan with"},
    {kOutcomeNoProgram,
     "This player holds no plan for running the show without the coordinator. Check this player on the coordinator "
     "once it is back.",
     "it holds no plan for running the show without the coordinator"},
    {kOutcomeProgramNotVerified,
     "The saved plan on this player no longer passes its check. Restore the coordinator so it can send a fresh one.",
     "its saved plan no longer passes its check"},
    {kOutcomeProgramExpired,
     "This player's plan for running the show without the coordinator has run out. Restore the coordinator to start "
     "the planned cues again.",
     "its plan for running the show without the coordinator has run out"},
    {kOutcomeUnknownEntry,
     "The plan has no cue for this playlist entry, so nothing was started for it. No action is needed if the entry "
     "has no cue in the show.",
     "the plan has no cue for the last playlist entry"},
    {kOutcomeAmbiguousEntry,
     "The plan names more than one cue for this playlist entry, so none was started. Check the show's playlist on "
     "the coordinator.",
     "the plan names more than one cue for the last playlist entry"},
    {kOutcomeNoTarget, "The plan names no node for this playlist entry's cue. Check the cue on the coordinator.",
     "the plan names no node for the last cue"},
    {kOutcomeNoPlayerKey,
     "This player has no key of its own for starting cues on nodes. Check that it is paired with the coordinator.",
     "it has no key of its own for the nodes"},
    {kOutcomePlayerKeyNotInProgram,
     "The plan does not carry this player's key, so no node would accept a cue from it. Check that the coordinator "
     "can reach this player.",
     "its plan does not carry this player's key"},
    {kOutcomeNoAddress,
     "The plan gives no address for this node. Check that the node is connected to the coordinator.",
     "the plan gives no address for a node"},
    {kOutcomeIncompleteProgram,
     "The plan is missing details for this node. Restore the coordinator so it can send a fresh one.",
     "the plan is missing details for a node"},
    {kOutcomeEntryNotIdentified,
     "FPP's playlist entry could not be identified. Check that the playlist still exists on this player.",
     "FPP's playlist entry could not be identified"},
    {kOutcomeSigningFailed, "The request for this node could not be signed. Check FPP's log on this player.",
     "a request could not be signed"},
};
// For a boundary where nodes were asked and none started the cue.
constexpr PlayerOutcomeCopy kNoNodeStartedCopy = {"", "", "no node started the last planned cue"};

inline const PlayerOutcomeCopy& PlayerOutcomeCopyFor(const std::string& word) {
    for (const PlayerOutcomeCopy& copy : kPlayerOutcomeCopy) {
        if (word == copy.word) return copy;
    }
    return kNoNodeStartedCopy;
}

inline const char* PlayerOutcomeWord(ActivationResolveKind kind) {
    switch (kind) {
        case ActivationResolveKind::kNoProgramInstalled:
            return kOutcomeNoProgram;
        case ActivationResolveKind::kProgramFailedReverification:
            return kOutcomeProgramNotVerified;
        case ActivationResolveKind::kProgramExpired:
            return kOutcomeProgramExpired;
        case ActivationResolveKind::kUnknownEntry:
            return kOutcomeUnknownEntry;
        case ActivationResolveKind::kAmbiguousEntry:
            return kOutcomeAmbiguousEntry;
        case ActivationResolveKind::kNoActivatableTarget:
            return kOutcomeNoTarget;
        case ActivationResolveKind::kMatch:
            return "";
    }
    return "";
}

enum class FallbackMode { kNormal, kFallback };

inline const char* FallbackModeName(FallbackMode mode) { return mode == FallbackMode::kFallback ? "fallback" : "normal"; }

// What the last entry boundary in fallback came to.
enum class BoundaryResult {
    kNone,
    kStarted,
    // The plan maps this entry to no cue. Ordinary, and not a failure.
    kNothingToStart,
    kStartedOnSomeNodes,
    kNotStarted,
};

constexpr const char* kCoordinatorLostMessage =
    "The coordinator has stopped answering, and this player will start the planned cues on the nodes itself from "
    "the next playlist entry. Check the coordinator.";
constexpr const char* kFallbackActiveMessage =
    "The coordinator stopped answering, so this player is starting the planned cues on the nodes itself until this "
    "playlist stops. Check the coordinator.";
constexpr const char* kStartedOnSomeNodesMessage =
    "The coordinator stopped answering, and this player started the last planned cue on only some of its nodes. "
    "Check the nodes and restore the coordinator.";
constexpr const char* kCannotStartPrefix =
    "The coordinator has stopped answering, and this player cannot start the planned cues itself because ";
constexpr const char* kNotStartingPrefix =
    "The coordinator stopped answering, and this player is not starting the planned cues because ";
constexpr const char* kRestoreCoordinatorAction = ". Restore the coordinator to start them again.";

// The notice an operator sees, true to what this player can do and last did.
// problem is one of this player's outcome words, or empty when nothing
// stands in the way.
inline std::string FallbackNotice(FallbackMode mode, bool coordinatorLost, const std::string& problem,
                                  BoundaryResult lastBoundary) {
    if (mode == FallbackMode::kNormal) {
        if (!coordinatorLost) return std::string();
        if (problem.empty()) return kCoordinatorLostMessage;
        return std::string(kCannotStartPrefix) + PlayerOutcomeCopyFor(problem).noticeClause +
               kRestoreCoordinatorAction;
    }
    if (!problem.empty() || lastBoundary == BoundaryResult::kNotStarted) {
        return std::string(kNotStartingPrefix) + PlayerOutcomeCopyFor(problem).noticeClause +
               kRestoreCoordinatorAction;
    }
    if (lastBoundary == BoundaryResult::kStartedOnSomeNodes) return kStartedOnSomeNodesMessage;
    return kFallbackActiveMessage;
}

// The one record of where this host stands in ADR-048's Normal, Fallback and
// Resting states. The cutoff, rest or hold rules and the hand-back at the
// next scheduled-show boundary extend this object; they are not built yet.
struct FallbackExecutionState {
    FallbackMode mode = FallbackMode::kNormal;
    TimeMillis enteredAtMillis = 0;
    std::string enteredAtEntryKey;
    BoundaryResult lastBoundary = BoundaryResult::kNone;
    // This player's outcome word for a boundary that started nothing. Empty when a node refused.
    std::string lastBoundaryProblem;

    void enter(TimeMillis now, const std::string& entryKey) {
        mode = FallbackMode::kFallback;
        enteredAtMillis = now;
        enteredAtEntryKey = entryKey;
    }
    // The only exits in this step: the playlist it entered under stops, or the plugin restarts.
    void leaveOnPlaylistStop() { *this = FallbackExecutionState(); }
};

// Where the operator-facing notice goes. The FPP adapters back it with WarningHolder.
class FallbackStateNotifier {
 public:
    virtual ~FallbackStateNotifier() = default;
    virtual void raise(const std::string& message) = 0;
    virtual void clear(const std::string& message) = 0;
};

// One delivery, refusal or program hand-off, as an operator reads it afterwards.
struct FallbackRecord {
    TimeMillis atMillis = 0;
    std::string entryKey;
    std::string nodeId;
    std::string address;
    std::string executionId;
    // A node's word when nodeAnswered, otherwise one of this player's own.
    std::string outcome;
    std::string reason;
    bool nodeAnswered = false;
    int attempts = 0;
};

struct FallbackStatusSnapshot {
    FallbackExecutionState state;
    bool coordinatorReachable = false;
    bool coordinatorLost = false;
    bool executorKeyRegistered = false;
    // Why the last registration did not succeed, in the coordinator's words when it gave any.
    std::string executorKeyRegistrationProblem;
    std::string executorPublicKey;
    std::string programPackageId;
    std::string programRevision;
    std::string programExpiresAt;
    bool programEnrollsThisExecutor = false;
    std::uint64_t activationsAuthorized = 0;
    std::uint64_t activationsNotDelivered = 0;
    std::uint64_t coordinatorPostsSkipped = 0;
    std::vector<FallbackRecord> recentActivations;
    std::vector<FallbackRecord> recentProgramHandOffs;
};

struct FallbackExecutorOptions {
    Clock clock = nullptr;
    std::string fppInstanceUuid;
    // Holds config.json and receives fallback-status.json.
    std::string stateDir;
    // Holds the pairing token and the executor key.
    std::string credentialDir;
    std::string installPath;
    PinnedKeyLoadResult pinnedKey;
    OutageDetectorConfig detector;
    int programRefetchIntervalMillis = kHypothesisProgramRefetchIntervalMillis;
    showmesh::RandomBytesFn randomBytes = showmesh::readRandomBytes;
    // Receives one line per event. isError marks what an operator must act on.
    std::function<void(bool isError, const std::string& line)> log;
    FallbackStateNotifier* notifier = nullptr;
    // Replaces the real wait between delivery attempts. Tests only.
    DeliveryPause pause;
};

class FallbackExecutor : public showmesh::FallbackActivationRecorder {
 public:
    FallbackExecutor(FallbackExecutorOptions options, HttpTransport* transport)
        : options_(std::move(options)),
          transport_(transport),
          credentials_(options_.credentialDir),
          detector_(options_.detector) {}

    ~FallbackExecutor() override { stop(); }

    void start() override {
        if (started_.exchange(true)) return;
        thread_ = std::thread([this] {
            while (!stopRequested_.load()) {
                tick(options_.clock());
                std::unique_lock<std::mutex> lock(wakeMutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopRequested_.load(); });
            }
        });
    }

    void requestStop() override {
        stopRequested_.store(true);
        wake_.notify_all();
    }

    void stop() override {
        requestStop();
        if (thread_.joinable()) thread_.join();
    }

    bool coordinatorLost() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_.mode == FallbackMode::kFallback || detector_.confirmedLost();
    }

    // One step of the background thread: probe when due, then, only while the
    // coordinator answers and this host is not in fallback, register and refetch.
    // A stop request is honored between every network call.
    void tick(TimeMillis now) {
        // The second test keeps a clock stepped backwards from stalling the probe.
        const TimeMillis interval = options_.detector.probeIntervalMillis;
        if (probedOnce_ && now < nextProbeAtMillis_ && nextProbeAtMillis_ - now <= interval) return;
        nextProbeAtMillis_ = now + interval;
        probedOnce_ = true;
        refreshCredentialAndKey();
        loadProgramSummaryOnce();
        const showmesh::CoordinatorUrlLoad url = showmesh::loadCoordinatorBaseUrl(options_.stateDir);
        // No coordinator is configured, so there is nothing to lose and nothing to fetch.
        if (!url.ok) return;

        const bool reached = probeCoordinator(url.baseUrl);
        bool wasLost = false;
        bool isLost = false;
        bool inFallback = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wasLost = detector_.confirmedLost();
            detector_.recordProbe(reached, now);
            isLost = detector_.confirmedLost();
            inFallback = state_.mode == FallbackMode::kFallback;
        }
        if (isLost && !wasLost) {
            log(true, "coordinator loss confirmed after " + std::to_string(options_.detector.failedProbesToConfirm) +
                          " or more failed probes");
        } else if (wasLost && !isLost) {
            log(false, "coordinator is answering again");
        }
        if (reached && !inFallback && !stopRequested_.load()) keepCurrent(url.baseUrl, now);
        refreshNotice(now);
        publishStatus(now);
    }

    bool observeEntryEvent(const showmesh::FallbackEntryEvent& event) override {
        switch (event.action) {
            case showmesh::PlaylistAction::kStop:
                lastBoundary_.reset();
                leaveFallbackOnPlaylistStop(event.observedAtMillis);
                break;
            case showmesh::PlaylistAction::kQueryNext:
                occurrenceFinished_ = true;
                break;
            case showmesh::PlaylistAction::kStart:
            case showmesh::PlaylistAction::kPlaying:
                if (isNewOccurrence(event)) onEntryBoundary(event);
                break;
            case showmesh::PlaylistAction::kUnknown:
                break;
        }
        const bool lost = coordinatorLost();
        if (lost) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++coordinatorPostsSkipped_;
        }
        refreshNotice(event.observedAtMillis);
        publishStatus(event.observedAtMillis);
        return lost;
    }

    FallbackStatusSnapshot status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        FallbackStatusSnapshot snapshot;
        snapshot.state = state_;
        snapshot.coordinatorReachable = detector_.reachable();
        snapshot.coordinatorLost = detector_.confirmedLost();
        snapshot.executorKeyRegistered = registered_;
        snapshot.executorKeyRegistrationProblem = registrationProblem_;
        snapshot.executorPublicKey = key_.key.publicKeyBase64;
        snapshot.programPackageId = programPackageId_;
        snapshot.programRevision = programRevision_;
        snapshot.programExpiresAt = programExpiresAt_;
        snapshot.programEnrollsThisExecutor = programEnrollsThisExecutor_;
        snapshot.activationsAuthorized = activationsAuthorized_;
        snapshot.activationsNotDelivered = activationsNotDelivered_;
        snapshot.coordinatorPostsSkipped = coordinatorPostsSkipped_;
        snapshot.recentActivations.assign(recentActivations_.begin(), recentActivations_.end());
        snapshot.recentProgramHandOffs.assign(recentProgramHandOffs_.begin(), recentProgramHandOffs_.end());
        return snapshot;
    }

    // The notice currently raised, empty when none is.
    std::string notice() const {
        std::lock_guard<std::mutex> lock(noticeMutex_);
        return raisedNotice_;
    }

 private:
    struct Boundary {
        bool identityResolved = false;
        std::string entryKey;
        std::optional<int> playlistLoop;
    };

    struct InstalledProgram {
        bool present = false;
        std::string signedDocument;
        std::string packageId;
        std::string revision;
        std::string compiledAt;
        std::string expiresAt;
        std::string executorPublicKey;
        std::vector<std::string> addresses;
    };

    void log(bool isError, const std::string& line) const {
        if (options_.log) options_.log(isError, line);
    }

    bool probeCoordinator(const std::string& baseUrl) {
        HttpRequest request;
        request.url = joinUrlPath(baseUrl, kCoordinatorHealthPath);
        request.timeoutMillis = options_.detector.probeTimeoutMillis;
        const HttpResponse response = transport_->get(request);
        return response.transportOk && response.statusCode >= 200 && response.statusCode <= 299;
    }

    // A changed pairing token means a new pairing: the key is registered again under it.
    void refreshCredentialAndKey() {
        const showmesh::CredentialLoad credential = showmesh::loadCoordinatorCredential(options_.credentialDir);
        const std::string tokenHash = credential.ok ? showmesh::sha256Hex(credential.token) : std::string();
        if (tokenHash != tokenHash_) {
            tokenHash_ = tokenHash;
            credentials_.invalidate();
            resetRegistrationBackoff();
            std::lock_guard<std::mutex> lock(mutex_);
            registered_ = false;
            registrationAttemptDue_ = true;
        }
        if (keyCopy().usable()) return;
        ExecutorKeyResult loaded = LoadOrCreateExecutorKey(options_.credentialDir, options_.randomBytes);
        if (loaded.status == ExecutorKeyStatus::kCreated) log(false, "created this player's executor key");
        if (loaded.status == ExecutorKeyStatus::kUnusable && loaded.detail != keyProblem_) log(true, loaded.detail);
        keyProblem_ = loaded.detail;
        std::lock_guard<std::mutex> lock(mutex_);
        key_ = std::move(loaded);
    }

    ExecutorKeyResult keyCopy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return key_;
    }

    // What is on disk from before this start, so the status is true before the first fetch.
    void loadProgramSummaryOnce() {
        if (programSummaryLoaded_) return;
        programSummaryLoaded_ = true;
        rememberProgram(readInstalledProgram(), keyCopy());
    }

    // Returns whether the program carries this host's key.
    bool rememberProgram(const InstalledProgram& program, const ExecutorKeyResult& key) {
        const bool enrolled =
            program.present && key.usable() && program.executorPublicKey == key.key.publicKeyBase64;
        std::lock_guard<std::mutex> lock(mutex_);
        programPackageId_ = program.packageId;
        programRevision_ = program.revision;
        programExpiresAt_ = program.expiresAt;
        programEnrollsThisExecutor_ = enrolled;
        return enrolled;
    }

    bool registeredNow() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return registered_;
    }

    void keepCurrent(const std::string& baseUrl, TimeMillis now) {
        // The last test asks again at once when the clock stepped backwards.
        bool fetchDue =
            !fetchedOnce_ || now >= nextFetchAtMillis_ || nextFetchAtMillis_ - now > latestFetchDelayMillis_;
        const ExecutorKeyResult key = keyCopy();
        bool attemptDue = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            attemptDue = registrationAttemptDue_;
            registrationAttemptDue_ = false;
        }
        if (key.usable() && !registeredNow() && (attemptDue || fetchDue) && now >= nextRegistrationAtMillis_) {
            if (registerKey(baseUrl, key.key, now)) fetchDue = true;
            if (stopRequested_.load()) return;
        }
        if (!fetchDue) return;

        const bool enrolled = fetchAndInstall(baseUrl, now, key);
        if (stopRequested_.load()) return;
        // Registered, yet the published program does not carry the key: the
        // coordinator may have lost it, so the flag in memory is not trusted.
        if (programPublished_ && key.usable() && registeredNow() && !enrolled && now >= nextRegistrationAtMillis_) {
            if (registerKey(baseUrl, key.key, now)) nextFetchAtMillis_ = now;
        }
    }

    void resetRegistrationBackoff() {
        registrationBackoffMillis_ = 0;
        nextRegistrationAtMillis_ = 0;
    }

    // True when the coordinator stored a first or different key, so its program changes at once.
    bool registerKey(const std::string& baseUrl, const ExecutorKey& key, TimeMillis now) {
        const ExecutorRegistration registration =
            RegisterExecutorKey(transport_, &credentials_, baseUrl, options_.fppInstanceUuid, key.publicKeyBase64,
                                kExecutorRegistrationTimeoutMillis);
        const bool ok = registration.kind == ExecutorRegistrationKind::kRegistered;
        std::string problem;
        if (registration.kind == ExecutorRegistrationKind::kRefused) {
            problem = "The coordinator answered " + std::to_string(registration.statusCode) +
                      (registration.detail.empty() ? std::string(".") : ": " + registration.detail);
        } else if (!ok) {
            problem = registration.detail;
        }

        // Any change in the answer starts the backoff over. A 409 is asked
        // again at every program fetch, because it clears by waiting.
        const std::string answer = std::to_string(static_cast<int>(registration.kind)) + "/" +
                                   std::to_string(registration.statusCode) + "/" + (registration.changed ? "c" : "");
        if (answer != lastRegistrationAnswer_) resetRegistrationBackoff();
        lastRegistrationAnswer_ = answer;
        if (registration.kind == ExecutorRegistrationKind::kRefused &&
            registration.statusCode == kRegistrationNotYetStatus) {
            resetRegistrationBackoff();
        } else {
            registrationBackoffMillis_ =
                registrationBackoffMillis_ == 0
                    ? static_cast<TimeMillis>(options_.programRefetchIntervalMillis)
                    : std::min<TimeMillis>(registrationBackoffMillis_ * 2, kRegistrationBackoffCapMillis);
            nextRegistrationAtMillis_ = now + registrationBackoffMillis_;
        }

        std::string previousProblem;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previousProblem = registrationProblem_;
            registered_ = ok;
            registrationProblem_ = problem;
        }
        if (ok && registration.changed) log(false, "executor key registered");
        if (!problem.empty() && problem != previousProblem) log(true, "executor key not registered: " + problem);
        return ok && registration.changed;
    }

    // Returns whether the installed program carries this host's key.
    bool fetchAndInstall(const std::string& baseUrl, TimeMillis now, const ExecutorKeyResult& key) {
        fetchedOnce_ = true;
        const InstalledProgram before = readInstalledProgram();
        scheduleNextFetch(now, before);
        if (options_.pinnedKey.status != PinnedKeyLoadStatus::kLoaded) {
            if (!pinnedKeyProblemLogged_) {
                log(true, std::string("no usable fallback program: ") +
                              PinnedKeyLoadStatusName(options_.pinnedKey.status) + " (" + options_.pinnedKey.error +
                              ")");
            }
            pinnedKeyProblemLogged_ = true;
            return false;
        }

        const FallbackFetchOutcome outcome = FetchAndInstallFallbackProgram(
            transport_, &credentials_, baseUrl, options_.fppInstanceUuid, options_.pinnedKey.publicKey,
            options_.installPath, options_.clock, before.present ? &before.signedDocument : nullptr);
        const std::string outcomeLine =
            std::string("program fetch: ") + FallbackFetchOutcomeKindName(outcome.kind) + ": " + outcome.detail;
        if (outcomeLine != lastFetchOutcomeLine_) log(false, outcomeLine);
        lastFetchOutcomeLine_ = outcomeLine;
        programPublished_ = outcome.kind == FallbackFetchOutcomeKind::kInstalled ||
                            outcome.kind == FallbackFetchOutcomeKind::kUnchanged;
        // The same program again: nothing is rewritten, acknowledged or handed out.
        if (outcome.kind == FallbackFetchOutcomeKind::kUnchanged) return rememberProgram(before, key);

        // One acknowledge per verdict on a program, not one per fetch of a program still refused.
        const std::string verdict = outcome.packageId + "/" + outcome.revision + "/" +
                                    FallbackFetchOutcomeVerificationResult(outcome.kind);
        const bool newVerdict = outcome.kind == FallbackFetchOutcomeKind::kInstalled || verdict != lastAcknowledged_;
        if (ShouldAcknowledgeFallbackFetchOutcome(outcome) && newVerdict && !stopRequested_.load()) {
            const AcknowledgeResult ack = AcknowledgeFallbackProgram(transport_, &credentials_, baseUrl,
                                                                     options_.fppInstanceUuid, outcome, options_.clock);
            if (ack.ok) lastAcknowledged_ = verdict;
            if (!ack.ok) log(true, "acknowledge failed: " + ack.error);
        }
        if (outcome.kind != FallbackFetchOutcomeKind::kInstalled) return rememberProgram(before, key);

        const InstalledProgram program = readInstalledProgram();
        const bool enrolled = rememberProgram(program, key);
        scheduleNextFetch(now, program);
        resetRegistrationBackoff();
        for (const std::string& address : program.addresses) {
            if (stopRequested_.load()) break;
            const NodeAnswer answer =
                HandProgramToNode(transport_, address, options_.fppInstanceUuid, program.signedDocument);
            FallbackRecord record;
            record.atMillis = now;
            record.address = address;
            record.nodeAnswered = !answer.outcome.empty();
            DescribeNodeAnswer(answer, &record.outcome, &record.reason);
            record.attempts = 1;
            appendRecord(record, "program", &recentProgramHandOffs_, kRecentProgramHandOffRecords);
        }
        return enrolled;
    }

    // The configured interval, or one third of the program's validity when that is sooner.
    void scheduleNextFetch(TimeMillis now, const InstalledProgram& program) {
        TimeMillis delay = options_.programRefetchIntervalMillis;
        std::int64_t compiledAt = 0;
        std::int64_t expiresAt = 0;
        if (program.present && detail::parseRfc3339ToEpochSeconds(program.compiledAt, &compiledAt) &&
            detail::parseRfc3339ToEpochSeconds(program.expiresAt, &expiresAt) && expiresAt > compiledAt) {
            const TimeMillis validityMillis = static_cast<TimeMillis>(expiresAt - compiledAt) * 1000;
            delay = std::min(delay, validityMillis * kRefetchValidityNumerator / kRefetchValidityDenominator);
        }
        latestFetchDelayMillis_ = std::max<TimeMillis>(delay, options_.detector.probeIntervalMillis);
        nextFetchAtMillis_ = now + latestFetchDelayMillis_;
    }

    InstalledProgram readInstalledProgram() const {
        InstalledProgram program;
        const ReadInstalledResult read = ReadInstalledFallbackProgram(options_.installPath);
        if (!read.ok) return program;
        const showmesh::json::ParseResult parsed = showmesh::json::parse(read.rawDocument);
        const showmesh::json::Value* body = parsed.ok ? detail::findMember(parsed.value, "program") : nullptr;
        if (body == nullptr || body->type() != showmesh::json::Type::kObject) return program;
        auto text = [](const showmesh::json::Value& object, const char* name) {
            const showmesh::json::Value* v = detail::findMember(object, name);
            return v != nullptr && v->type() == showmesh::json::Type::kString ? v->string() : std::string();
        };
        program.present = true;
        program.signedDocument = read.rawDocument;
        program.packageId = text(*body, "packageId");
        program.revision = text(*body, "revision");
        program.compiledAt = text(*body, "compiledAt");
        program.expiresAt = text(*body, "expiresAt");
        program.executorPublicKey = text(*body, "executorPublicKey");
        const showmesh::json::Value* entries = detail::findMember(*body, "entries");
        if (entries == nullptr || entries->type() != showmesh::json::Type::kArray) return program;
        for (const showmesh::json::Value& entry : entries->items()) {
            if (entry.type() != showmesh::json::Type::kObject) continue;
            const showmesh::json::Value* targets = detail::findMember(entry, "targets");
            if (targets == nullptr || targets->type() != showmesh::json::Type::kArray) continue;
            for (const showmesh::json::Value& target : targets->items()) {
                if (target.type() != showmesh::json::Type::kObject) continue;
                const std::string address = text(target, "address");
                if (!address.empty() && std::find(program.addresses.begin(), program.addresses.end(), address) ==
                                            program.addresses.end()) {
                    program.addresses.push_back(address);
                }
            }
        }
        return program;
    }

    // This player's outcome word for what stops it starting any cue right
    // now, or empty when it holds everything a delivery needs.
    std::string readinessProblem(TimeMillis nowMillis) const {
        if (options_.pinnedKey.status != PinnedKeyLoadStatus::kLoaded) return kOutcomeNoCoordinatorKey;
        const auto now = std::chrono::system_clock::time_point(std::chrono::milliseconds(nowMillis));
        const ActivationResolution probe =
            ResolveInstalledActivation(std::string(), options_.installPath, options_.pinnedKey.publicKey, now);
        if (probe.kind == ActivationResolveKind::kNoProgramInstalled ||
            probe.kind == ActivationResolveKind::kProgramFailedReverification ||
            probe.kind == ActivationResolveKind::kProgramExpired) {
            return PlayerOutcomeWord(probe.kind);
        }
        const ExecutorKeyResult key = keyCopy();
        if (!key.usable()) return kOutcomeNoPlayerKey;
        if (readInstalledProgram().executorPublicKey != key.key.publicKeyBase64) return kOutcomePlayerKeyNotInProgram;
        return std::string();
    }

    // A repeated `playing` for the entry already playing is FPP resuming it,
    // not a new occurrence, unless FPP reported the entry finished in between.
    bool isNewOccurrence(const showmesh::FallbackEntryEvent& event) {
        const bool same = lastBoundary_.has_value() && !occurrenceFinished_ &&
                          lastBoundary_->identityResolved == event.identityResolved &&
                          lastBoundary_->entryKey == event.entryKey &&
                          lastBoundary_->playlistLoop == event.playlistLoop;
        if (same) return false;
        lastBoundary_ = Boundary{event.identityResolved, event.entryKey, event.playlistLoop};
        occurrenceFinished_ = false;
        return true;
    }

    void onEntryBoundary(const showmesh::FallbackEntryEvent& event) {
        bool entered = false;
        FallbackMode mode = FallbackMode::kNormal;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.mode == FallbackMode::kNormal && detector_.confirmedLost()) {
                state_.enter(event.observedAtMillis, event.entryKey);
                entered = true;
            }
            mode = state_.mode;
        }
        if (entered) log(true, "entered fallback at entryKey=" + event.entryKey);
        const ExecutorKeyResult key = keyCopy();

        ActivationResolution resolution;
        std::string problem = kOutcomeEntryNotIdentified;
        std::string detailForLog = "identity not resolved";
        if (event.identityResolved && options_.pinnedKey.status != PinnedKeyLoadStatus::kLoaded) {
            problem = kOutcomeNoCoordinatorKey;
            detailForLog =
                std::string(PinnedKeyLoadStatusName(options_.pinnedKey.status)) + " " + options_.pinnedKey.error;
        } else if (event.identityResolved) {
            const auto now = std::chrono::system_clock::time_point(std::chrono::milliseconds(event.observedAtMillis));
            resolution =
                ResolveInstalledActivation(event.entryKey, options_.installPath, options_.pinnedKey.publicKey, now);
            problem = PlayerOutcomeWord(resolution.kind);
            detailForLog = std::string(ActivationResolveKindName(resolution.kind)) + " " + resolution.reason;
        }
        log(false, detailForLog + " entryKey=" + event.entryKey +
                       " observedAtMillis=" + std::to_string(static_cast<long long>(event.observedAtMillis)));
        if (mode != FallbackMode::kFallback) return;

        if (problem.empty() && !key.usable()) problem = kOutcomeNoPlayerKey;
        if (problem.empty() && resolution.match->executorPublicKey() != key.key.publicKeyBase64) {
            problem = kOutcomePlayerKeyNotInProgram;
        }
        if (!problem.empty()) {
            const bool nothingMapped = problem == kOutcomeUnknownEntry;
            recordPlayerDecision(event, std::string(), std::string(), problem);
            setBoundaryResult(nothingMapped ? BoundaryResult::kNothingToStart : BoundaryResult::kNotStarted,
                              nothingMapped ? std::string() : problem);
            return;
        }
        deliverMatch(event, *resolution.match, key.key);
    }

    void setBoundaryResult(BoundaryResult result, const std::string& problem) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.lastBoundary = result;
        state_.lastBoundaryProblem = problem;
    }

    void deliverMatch(const showmesh::FallbackEntryEvent& event, const ActivationMatch& match,
                      const ExecutorKey& key) {
        struct Delivery {
            FallbackRecord record;
            std::string body;
        };
        std::vector<Delivery> deliveries;
        std::string firstProblem;
        for (const ActivationTarget& target : match.targets()) {
            std::string problem;
            uint8_t random[16];
            std::string signature;
            std::string executionId;
            ActivationRequestBuild build;
            if (!target.address.has_value()) {
                problem = kOutcomeNoAddress;
            } else {
                const bool haveId = options_.randomBytes != nullptr && options_.randomBytes(random, sizeof(random));
                executionId = haveId ? formatExecutionId(random) : std::string();
                build = BuildActivationRequest(match, target, executionId);
                if (!build.ok) {
                    problem = kOutcomeIncompleteProgram;
                } else if (!haveId || !signWithExecutorKey(key, build.canonical, &signature)) {
                    problem = kOutcomeSigningFailed;
                }
            }
            if (!problem.empty()) {
                if (firstProblem.empty()) firstProblem = problem;
                recordPlayerDecision(event, target.nodeId, target.address.value_or(std::string()), problem);
                continue;
            }
            Delivery delivery;
            delivery.record.atMillis = event.observedAtMillis;
            delivery.record.entryKey = event.entryKey;
            delivery.record.nodeId = target.nodeId;
            delivery.record.address = *target.address;
            delivery.record.executionId = executionId;
            delivery.body = ActivationRequestBody(build.canonical, signature);
            deliveries.push_back(std::move(delivery));
        }

        // One thread per node, so a node that does not answer never delays another node's activation.
        auto run = [&](Delivery* delivery) {
            const NodeDeliveryResult result = DeliverActivation(
                transport_, delivery->record.address, delivery->body, match.fppInstanceUuid(), match.signedDocument(),
                options_.pause ? options_.pause : DeliveryPause([this](int millis) { return waitOrStop(millis); }));
            delivery->record.outcome = result.outcome;
            delivery->record.reason = result.reason;
            delivery->record.nodeAnswered = result.nodeAnswered();
            delivery->record.attempts = result.attempts;
        };
        std::vector<std::thread> threads;
        for (std::size_t i = 1; i < deliveries.size(); ++i) threads.emplace_back(run, &deliveries[i]);
        if (!deliveries.empty()) run(&deliveries[0]);
        for (std::thread& thread : threads) thread.join();

        std::size_t started = 0;
        for (const Delivery& delivery : deliveries) {
            if (delivery.record.outcome == "authorized") ++started;
            appendRecord(delivery.record, "activation", &recentActivations_, kRecentActivationRecords);
        }
        setBoundaryResult(started == match.targets().size() ? BoundaryResult::kStarted
                          : started > 0                     ? BoundaryResult::kStartedOnSomeNodes
                                                            : BoundaryResult::kNotStarted,
                          deliveries.empty() ? firstProblem : std::string());
    }

    bool waitOrStop(int millis) {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        return !wake_.wait_for(lock, std::chrono::milliseconds(millis), [this] { return stopRequested_.load(); });
    }

    void recordPlayerDecision(const showmesh::FallbackEntryEvent& event, const std::string& nodeId,
                              const std::string& address, const std::string& outcome) {
        FallbackRecord record;
        record.atMillis = event.observedAtMillis;
        record.entryKey = event.entryKey;
        record.nodeId = nodeId;
        record.address = address;
        record.outcome = outcome;
        record.reason = PlayerOutcomeCopyFor(outcome).reason;
        appendRecord(record, "activation", &recentActivations_, kRecentActivationRecords);
    }

    void appendRecord(const FallbackRecord& record, const char* kind, std::deque<FallbackRecord>* list,
                      std::size_t limit) {
        const bool good = record.outcome == "authorized" || record.outcome == "installed";
        log(!good, std::string(kind) + " entryKey=" + record.entryKey + " nodeId=" + record.nodeId +
                       " address=" + record.address + " executionId=" + record.executionId +
                       " outcome=" + record.outcome + " attempts=" + std::to_string(record.attempts) +
                       " reason=" + record.reason);
        std::lock_guard<std::mutex> lock(mutex_);
        if (list == &recentActivations_) ++(good ? activationsAuthorized_ : activationsNotDelivered_);
        list->push_back(record);
        while (list->size() > limit) list->pop_front();
    }

    void leaveFallbackOnPlaylistStop(TimeMillis now) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.mode != FallbackMode::kFallback) return;
            state_.leaveOnPlaylistStop();
        }
        log(false, "left fallback: the playlist stopped at " + std::to_string(static_cast<long long>(now)));
    }

    // Keeps exactly one notice raised: the one that is true now, or none.
    // The state is read while holding the lock that guards the raise, so the
    // last caller to finish is the one with the newest state.
    void refreshNotice(TimeMillis now) {
        std::lock_guard<std::mutex> noticeLock(noticeMutex_);
        FallbackExecutionState state;
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state = state_;
            lost = detector_.confirmedLost();
        }
        std::string problem;
        if (state.mode == FallbackMode::kFallback || lost) {
            problem = readinessProblem(now);
            if (problem.empty() && state.lastBoundary == BoundaryResult::kNotStarted) {
                problem = state.lastBoundaryProblem;
            }
        }
        const std::string wanted = FallbackNotice(state.mode, lost, problem, state.lastBoundary);
        if (wanted == raisedNotice_) return;
        if (options_.notifier != nullptr) {
            if (!raisedNotice_.empty()) options_.notifier->clear(raisedNotice_);
            if (!wanted.empty()) options_.notifier->raise(wanted);
        }
        raisedNotice_ = wanted;
    }

    static showmesh::json::Value recordsJson(const std::vector<FallbackRecord>& records) {
        using showmesh::json::Value;
        std::vector<Value> items;
        for (const FallbackRecord& record : records) {
            items.push_back(Value::makeObject({
                {"atMillis", Value::makeNumber(static_cast<double>(record.atMillis))},
                {"entryKey", Value::makeString(record.entryKey)},
                {"nodeId", Value::makeString(record.nodeId)},
                {"address", Value::makeString(record.address)},
                {"executionId", Value::makeString(record.executionId)},
                {"outcome", Value::makeString(record.outcome)},
                {"answeredBy", Value::makeString(record.nodeAnswered ? "node" : "player")},
                {"reason", Value::makeString(record.reason)},
                {"attempts", Value::makeNumber(record.attempts)},
            }));
        }
        return Value::makeArray(std::move(items));
    }

    // The snapshot is taken while holding the lock that guards the write, for
    // the same reason refreshNotice() reads its state under its own.
    void publishStatus(TimeMillis now) {
        if (options_.stateDir.empty()) return;
        std::lock_guard<std::mutex> fileLock(statusFileMutex_);
        const FallbackStatusSnapshot snapshot = status();
        using showmesh::json::Value;
        auto number = [](std::uint64_t n) { return Value::makeNumber(static_cast<double>(n)); };
        const showmesh::json::CanonicalResult body = showmesh::json::canonicalize(Value::makeObject({
            {"mode", Value::makeString(FallbackModeName(snapshot.state.mode))},
            {"message", Value::makeString(notice())},
            {"coordinatorReachable", Value::makeBool(snapshot.coordinatorReachable)},
            {"coordinatorLost", Value::makeBool(snapshot.coordinatorLost)},
            {"enteredFallbackAtMillis", Value::makeNumber(static_cast<double>(snapshot.state.enteredAtMillis))},
            {"executorKeyRegistered", Value::makeBool(snapshot.executorKeyRegistered)},
            {"executorKeyRegistrationProblem", Value::makeString(snapshot.executorKeyRegistrationProblem)},
            {"executorPublicKey", Value::makeString(snapshot.executorPublicKey)},
            {"programPackageId", Value::makeString(snapshot.programPackageId)},
            {"programRevision", Value::makeString(snapshot.programRevision)},
            {"programExpiresAt", Value::makeString(snapshot.programExpiresAt)},
            {"programEnrollsThisExecutor", Value::makeBool(snapshot.programEnrollsThisExecutor)},
            {"activationsAuthorized", number(snapshot.activationsAuthorized)},
            {"activationsNotDelivered", number(snapshot.activationsNotDelivered)},
            {"coordinatorPostsSkipped", number(snapshot.coordinatorPostsSkipped)},
            {"recentActivations", recordsJson(snapshot.recentActivations)},
            {"recentProgramHandOffs", recordsJson(snapshot.recentProgramHandOffs)},
        }));
        if (!body.ok || body.text == writtenStatus_) return;
        // updatedAtMillis rides outside the compared text so an unchanged status is not rewritten.
        const std::string rendered =
            body.text.substr(0, body.text.size() - 1) + ",\"updatedAtMillis\":" + std::to_string(now) + "}";
        if (showmesh::writeFileAtomically(showmesh::joinPath(options_.stateDir, kFallbackStatusFilename), rendered)) {
            writtenStatus_ = body.text;
        }
    }

    FallbackExecutorOptions options_;
    HttpTransport* transport_;
    showmesh::FileCredentialSource credentials_;

    // Guards everything both threads read: the detector, the state object,
    // the key, the registration flags, the program summary and the records.
    mutable std::mutex mutex_;
    CoordinatorOutageDetector detector_;
    FallbackExecutionState state_;
    ExecutorKeyResult key_;
    bool registered_ = false;
    std::string registrationProblem_;
    bool registrationAttemptDue_ = true;
    std::string programPackageId_;
    std::string programRevision_;
    std::string programExpiresAt_;
    bool programEnrollsThisExecutor_ = false;
    std::uint64_t activationsAuthorized_ = 0;
    std::uint64_t activationsNotDelivered_ = 0;
    std::uint64_t coordinatorPostsSkipped_ = 0;
    std::deque<FallbackRecord> recentActivations_;
    std::deque<FallbackRecord> recentProgramHandOffs_;

    // Background thread only.
    bool probedOnce_ = false;
    TimeMillis nextProbeAtMillis_ = 0;
    bool fetchedOnce_ = false;
    // True when the last fetch found a published program, new or unchanged.
    bool programPublished_ = false;
    bool programSummaryLoaded_ = false;
    TimeMillis nextFetchAtMillis_ = 0;
    // How far ahead the last fetch scheduled the next one. A longer wait means the clock stepped back.
    TimeMillis latestFetchDelayMillis_ = 0;
    TimeMillis nextRegistrationAtMillis_ = 0;
    TimeMillis registrationBackoffMillis_ = 0;
    std::string lastRegistrationAnswer_;
    std::string lastFetchOutcomeLine_;
    std::string lastAcknowledged_;
    std::string tokenHash_;
    std::string keyProblem_;
    bool pinnedKeyProblemLogged_ = false;

    // Runtime worker thread only.
    std::optional<Boundary> lastBoundary_;
    bool occurrenceFinished_ = false;

    mutable std::mutex noticeMutex_;
    std::string raisedNotice_;
    std::mutex statusFileMutex_;
    std::string writtenStatus_;

    std::thread thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopRequested_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
};

}  // namespace fallback
}  // namespace showmesh
