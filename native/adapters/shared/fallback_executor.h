#pragma once

// The plugin's ADR-048 fallback executor (Track J, J4 and J5). tick() runs on
// this class's own thread and observeEntryEvent() on the runtime worker;
// neither is FPP's callback thread. The README's "Fallback executor" section has the rest.

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
#include "fallback_state_report.h"
#include "fallback_state_store.h"
#include "showmesh/atomic_write.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/http_transport.h"
#include "showmesh/json.h"
#include "showmesh/pairing.h"
#include "showmesh/runtime.h"
#include "showmesh/sha256.h"

namespace showmesh {
namespace fallback {

// Section 5.12 fixes how often a plugin in normal asks for its program again.
constexpr int kProgramRefetchIntervalMillis = 60000;
// A registration that makes no progress is tried again after one refetch
// interval, then double that each time, up to this cap.
constexpr TimeMillis kRegistrationBackoffCapMillis = 600000;
constexpr int kExecutorRegistrationTimeoutMillis = 10000;
// 409: the coordinator has not read this player's identity yet. It clears by waiting.
constexpr int kRegistrationNotYetStatus = 409;
constexpr std::size_t kRecentActivationRecords = 50;
constexpr std::size_t kRecentProgramHandOffRecords = 20;
constexpr const char* kFallbackStatusFilename = "fallback-status.json";

// The four rule values section 5.13 fixes. A program with any other is not usable.
constexpr const char* kRuleFallbackBoundary = "safe-playback-boundary";
constexpr const char* kRuleRestHold = "hold";
constexpr const char* kRuleLocalShutdown = "local-shutdown";
constexpr const char* kRuleRecoveryBoundary = "next-scheduled-show-boundary";

// Outcome words this player records when it decided itself, in the same shape
// as the words a node sends.
constexpr const char* kOutcomeNoCoordinatorKey = "no-coordinator-key";
constexpr const char* kOutcomeNoProgram = "no-program";
constexpr const char* kOutcomeProgramNotVerified = "program-not-verified";
constexpr const char* kOutcomeProgramExpired = "program-expired";
constexpr const char* kOutcomeProgramRulesUnknown = "program-rules-unknown";
constexpr const char* kOutcomeUnknownEntry = "unknown-entry";
constexpr const char* kOutcomeAmbiguousEntry = "ambiguous-entry";
constexpr const char* kOutcomeNoTarget = "no-target";
constexpr const char* kOutcomeNoPlayerKey = "no-player-key";
constexpr const char* kOutcomePlayerKeyNotInProgram = "player-key-not-in-program";
constexpr const char* kOutcomeNoAddress = "no-address";
constexpr const char* kOutcomeIncompleteProgram = "incomplete-program";
constexpr const char* kOutcomeEntryNotIdentified = "entry-not-identified";
constexpr const char* kOutcomeSigningFailed = "signing-failed";
constexpr const char* kOutcomeResting = "resting";

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
    {kOutcomeProgramRulesUnknown,
     "The plan on this player asks for something this plugin version does not know, so it is not used. Update the "
     "ShowMesh plugin on this player.",
     "its plan asks for something this plugin version does not know"},
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
    {kOutcomeResting,
     "This player's plan ran out earlier in this playlist, so nothing was started for this entry. Restore the "
     "coordinator; cues start again after this playlist stops.",
     "its plan ran out earlier in this playlist"},
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

// "2026-10-05T12:15:00Z" as an operator reads it. Any other shape is shown as written.
inline std::string CutoffForOperator(const std::string& cutoffAt) {
    if (cutoffAt.size() != 20 || cutoffAt[10] != 'T' || cutoffAt[19] != 'Z') return cutoffAt;
    return cutoffAt.substr(11, 5) + " UTC on " + cutoffAt.substr(0, 10);
}

constexpr const char* kCoordinatorLostMessage =
    "The coordinator has stopped answering, and this player will start the planned cues on the nodes itself from "
    "the next playlist entry. Check the coordinator.";
constexpr const char* kStartedOnSomeNodesMessage =
    "The coordinator stopped answering, and this player started the last planned cue on only some of its nodes. "
    "Check the nodes and restore the coordinator.";
constexpr const char* kCannotStartPrefix =
    "The coordinator has stopped answering, and this player cannot start the planned cues itself because ";
constexpr const char* kNotStartingPrefix =
    "The coordinator stopped answering, and this player is not starting the planned cues because ";
constexpr const char* kRestoreCoordinatorAction = ". Restore the coordinator to start them again.";

// What the notice is built from.
struct NoticeFacts {
    FallbackExecutionState state;
    bool coordinatorLost = false;
    bool coordinatorReachable = false;
    // One of this player's outcome words, or empty when nothing stands in the way.
    std::string problem;
};

// The notice an operator sees: the state, the playlist it is held under, the
// cutoff, and what to do. Empty when there is nothing to say.
inline std::string FallbackNotice(const NoticeFacts& facts) {
    const FallbackExecutionState& state = facts.state;
    const std::string playlist = "playlist " + state.playlistName;
    const std::string cutoff = CutoffForOperator(state.cutoffAt);
    switch (state.mode) {
        case FallbackMode::kNormal:
            if (!facts.coordinatorLost) return std::string();
            if (facts.problem.empty()) return kCoordinatorLostMessage;
            return std::string(kCannotStartPrefix) + PlayerOutcomeCopyFor(facts.problem).noticeClause +
                   kRestoreCoordinatorAction;
        case FallbackMode::kResting:
            return "This player stopped starting cues for " + playlist + " at " + cutoff +
                   ", when its plan for running without the coordinator ran out. Restore the coordinator; cues "
                   "start again after this playlist stops.";
        case FallbackMode::kFallback:
            break;
    }
    if (!facts.problem.empty() || state.lastBoundary == BoundaryResult::kNotStarted) {
        return std::string(kNotStartingPrefix) + PlayerOutcomeCopyFor(facts.problem).noticeClause +
               kRestoreCoordinatorAction;
    }
    if (state.lastBoundary == BoundaryResult::kStartedOnSomeNodes) return kStartedOnSomeNodesMessage;
    if (facts.coordinatorReachable) {
        return "The coordinator is answering again, and this player keeps starting the planned cues for " + playlist +
               " until that playlist stops. Nothing to do: the coordinator takes over when it stops.";
    }
    return "The coordinator stopped answering, so this player is starting the planned cues for " + playlist +
           " itself until that playlist stops or until " + cutoff + ". Check the coordinator.";
}

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
    // Why the last state report did not land. Empty when it did, or none was due.
    std::string stateReportProblem;
    std::uint64_t stateReportsSent = 0;
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
    // Holds the pairing token, the executor key and the fallback state file.
    std::string credentialDir;
    std::string installPath;
    PinnedKeyLoadResult pinnedKey;
    OutageDetectorConfig detector;
    int programRefetchIntervalMillis = kProgramRefetchIntervalMillis;
    // The playlist FPP was playing when the plugin started, empty when none.
    // It decides whether a saved fallback or resting state is resumed.
    std::string playingPlaylistAtStart;
    showmesh::RandomBytesFn randomBytes = showmesh::readRandomBytes;
    // Receives one line per event. isError marks what an operator must act on.
    std::function<void(bool isError, const std::string& line)> log;
    FallbackStateNotifier* notifier = nullptr;
    // Replaces the real wait between delivery attempts. Tests only.
    DeliveryPause pause;
};

class FallbackExecutor : public showmesh::FallbackActivationRecorder {
 public:
    // Reads the saved state from disk and nothing from the network, so it is
    // safe on FPP's plugin load path.
    FallbackExecutor(FallbackExecutorOptions options, HttpTransport* transport)
        : options_(std::move(options)),
          transport_(transport),
          credentials_(options_.credentialDir),
          detector_(options_.detector) {
        uint8_t random[16] = {0};
        if (options_.randomBytes != nullptr) options_.randomBytes(random, sizeof(random));
        bootId_ = formatExecutionId(random);
        restoreSavedState(options_.clock != nullptr ? options_.clock() : 0);
    }

    ~FallbackExecutor() override { stop(); }

    void start() override {
        if (started_.exchange(true)) return;
        thread_ = std::thread([this] {
            while (!stopRequested_.load()) {
                tick(options_.clock());
                std::unique_lock<std::mutex> lock(wakeMutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopRequested_.load() || woken_; });
                woken_ = false;
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

    // True while the worker must not post to the coordinator: outside normal,
    // while loss is confirmed, and until the report that must come first is sent.
    bool coordinatorLost() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return postsSuspendedLocked();
    }

    // One step of the background thread. It probes when a probe is due,
    // checks the cutoff, sends a state report when one is due, and, only in
    // normal with the coordinator answering, registers and refetches.
    // A stop request is honored between every network call.
    void tick(TimeMillis now) {
        bool acted = false;
        std::string baseUrl;
        if (probeDue(now)) {
            acted = true;
            refreshCredentialAndKey();
            loadProgramSummaryOnce();
            const showmesh::CoordinatorUrlLoad url = showmesh::loadCoordinatorBaseUrl(options_.stateDir);
            // No coordinator is configured, so there is nothing to lose and nothing to fetch.
            if (url.ok) {
                baseUrl = url.baseUrl;
                probe(baseUrl, now);
            }
        }
        if (enterRestingAtCutoff(now)) acted = true;
        if (retryOccurrenceDue_.exchange(false)) {
            retryRecordedOccurrence(now);
            acted = true;
        }

        bool reachable = false;
        bool reportDue = false;
        bool handBackDue = false;
        bool normal = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reachable = lastProbeSucceeded_;
            // No report without a pairing token: there is nobody to report as.
            reportDue = !tokenHash_.empty() && (reportDue_ || now >= nextPeriodicReportAtMillis_ ||
                                                nextPeriodicReportAtMillis_ - now > kStateReportIntervalMillis);
            handBackDue = handBackFetchDue_;
            normal = state_.mode == FallbackMode::kNormal;
        }
        if (reachable && !stopRequested_.load()) {
            if (baseUrl.empty() && (reportDue || (handBackDue && normal))) {
                const showmesh::CoordinatorUrlLoad url = showmesh::loadCoordinatorBaseUrl(options_.stateDir);
                if (url.ok) baseUrl = url.baseUrl;
            }
            if (!baseUrl.empty()) {
                // The report is the first request after a recovery, before a fetch and before an observation.
                if (reportDue) {
                    sendStateReport(baseUrl, now);
                    acted = true;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    reportOwedBeforePosts_ = false;
                    normal = state_.mode == FallbackMode::kNormal;
                    handBackDue = handBackFetchDue_;
                }
                if (normal && !stopRequested_.load()) {
                    if (handBackDue) {
                        handBackFetch(baseUrl, now);
                        acted = true;
                    } else if (probedThisTick_) {
                        keepCurrent(baseUrl, now);
                    }
                }
            }
        }
        probedThisTick_ = false;
        if (!acted) return;
        refreshNotice(now);
        publishStatus(now);
    }

    bool observeEntryEvent(const showmesh::FallbackEntryEvent& event) override {
        const TimeMillis now = event.observedAtMillis;
        enterRestingAtCutoff(now);
        // The boundary of section 5.13: FPP stopped the playlist fallback was
        // entered under, or names a different one.
        bool endedFallback = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            endedFallback = state_.mode != FallbackMode::kNormal &&
                            (event.action == showmesh::PlaylistAction::kStop ||
                             (!event.playlistName.empty() && event.playlistName != state_.playlistName));
        }
        if (endedFallback) handBack(now);

        switch (event.action) {
            case showmesh::PlaylistAction::kStop:
                lastBoundary_.reset();
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
        // The stop that ended fallback belongs to an entry that began in it, so it is never posted.
        bool suspended = endedFallback && event.action == showmesh::PlaylistAction::kStop;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            suspended = suspended || postsSuspendedLocked();
            if (suspended) ++coordinatorPostsSkipped_;
        }
        refreshNotice(now);
        publishStatus(now);
        return suspended;
    }

    FallbackStatusSnapshot status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        FallbackStatusSnapshot snapshot;
        snapshot.state = state_;
        snapshot.coordinatorReachable = lastProbeSucceeded_;
        snapshot.coordinatorLost = detector_.confirmedLost();
        snapshot.executorKeyRegistered = registered_;
        snapshot.executorKeyRegistrationProblem = registrationProblem_;
        snapshot.executorPublicKey = key_.key.publicKeyBase64;
        snapshot.programPackageId = programPackageId_;
        snapshot.programRevision = programRevision_;
        snapshot.programExpiresAt = programExpiresAt_;
        snapshot.programEnrollsThisExecutor = programEnrollsThisExecutor_;
        snapshot.stateReportProblem = stateReportProblem_;
        snapshot.stateReportsSent = stateReportsSent_;
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

    const std::string& bootId() const { return bootId_; }

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
        std::string expiresAt;
        std::string executorPublicKey;
        bool rulesKnown = false;
        std::vector<std::string> addresses;
    };

    void log(bool isError, const std::string& line) const {
        if (options_.log) options_.log(isError, line);
    }

    bool postsSuspendedLocked() const {
        return state_.mode != FallbackMode::kNormal || detector_.confirmedLost() || reportOwedBeforePosts_;
    }

    void wakeBackgroundThread() {
        {
            std::lock_guard<std::mutex> lock(wakeMutex_);
            woken_ = true;
        }
        wake_.notify_all();
    }

    // On start: a saved state is resumed only when FPP is playing the playlist
    // it names and the program copy it was entered with has not expired. The
    // name alone cannot tell the same run from a later run of that playlist;
    // the expiry bounds it. Otherwise the hand-back steps are owed.
    void restoreSavedState(TimeMillis now) {
        state_.sinceMillis = now;
        FallbackExecutionState saved;
        if (!LoadFallbackState(options_.credentialDir, &saved)) return;
        std::int64_t cutoffSeconds = 0;
        const bool unexpired =
            detail::parseRfc3339ToEpochSeconds(saved.cutoffAt, &cutoffSeconds) && now < cutoffSeconds * 1000;
        if (saved.playlistName != options_.playingPlaylistAtStart || !unexpired) {
            log(false, "the saved fallback state is over: its playlist stopped or its plan ran out while the "
                       "plugin was down; handing back");
            SaveFallbackState(options_.credentialDir, state_);
            handBackFetchDue_ = true;
            reportOwedBeforePosts_ = true;
            return;
        }
        state_ = saved;
        if (saved.occurrence.present) {
            lastBoundary_ = Boundary{saved.occurrence.identityResolved, saved.occurrence.entryKey,
                                     saved.occurrence.playlistLoop};
            retryOccurrenceDue_.store(saved.mode == FallbackMode::kFallback && !saved.occurrence.delivered);
        }
        log(true, std::string("resumed ") + FallbackModeName(saved.mode) + " under playlist " + saved.playlistName);
    }

    bool saveState(const FallbackExecutionState& state) {
        const bool ok = SaveFallbackState(options_.credentialDir, state);
        if (!ok) log(true, "could not save the fallback state; a plugin restart would not resume it");
        return ok;
    }

    // While the coordinator is held as lost the probe runs at least every 10
    // seconds and is never backed off, whatever the configured interval.
    bool probeDue(TimeMillis now) {
        TimeMillis interval = options_.detector.probeIntervalMillis;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (detector_.confirmedLost()) interval = std::min<TimeMillis>(interval, kLostProbeIntervalCapMillis);
        }
        // The last test keeps a clock stepped backwards from stalling the probe.
        if (probedOnce_ && now < lastProbeAtMillis_ + interval && now >= lastProbeAtMillis_) return false;
        lastProbeAtMillis_ = now;
        probedOnce_ = true;
        return true;
    }

    void probe(const std::string& baseUrl, TimeMillis now) {
        HttpRequest request;
        request.url = joinUrlPath(baseUrl, kCoordinatorHealthPath);
        request.timeoutMillis = options_.detector.probeTimeoutMillis;
        const HttpResponse response = transport_->get(request);
        const bool reached = response.transportOk && response.statusCode >= 200 && response.statusCode <= 299;
        probedThisTick_ = true;
        bool wasLost = false;
        bool isLost = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wasLost = detector_.confirmedLost();
            detector_.recordProbe(reached, now);
            isLost = detector_.confirmedLost();
            // A probe that succeeds after one that failed: the report goes first.
            if (reached && !lastProbeSucceeded_) reportDue_ = true;
            if (isLost) reportOwedBeforePosts_ = true;
            lastProbeSucceeded_ = reached;
        }
        if (isLost && !wasLost) {
            log(true, "coordinator loss confirmed after " + std::to_string(options_.detector.failedProbesToConfirm) +
                          " or more failed probes");
        } else if (wasLost && !isLost) {
            log(false, "coordinator is answering again");
        }
    }

    // The cutoff is the expiresAt of the copy fallback was entered with.
    // Returns true when this call moved the plugin to resting.
    bool enterRestingAtCutoff(TimeMillis now) {
        FallbackExecutionState state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.mode != FallbackMode::kFallback) return false;
            std::int64_t cutoffSeconds = 0;
            const bool parsed = detail::parseRfc3339ToEpochSeconds(state_.cutoffAt, &cutoffSeconds);
            if (parsed && now < cutoffSeconds * 1000) return false;
            state_.rest(now);
            reportDue_ = true;
            state = state_;
        }
        saveState(state);
        log(true, "reached the cutoff " + state.cutoffAt + "; resting until playlist " + state.playlistName +
                      " stops");
        wakeBackgroundThread();
        return true;
    }

    // The hand-back, on the runtime worker: normal first, then the report
    // while the coordinator answers, then the fetch on the background thread.
    void handBack(TimeMillis now) {
        bool reachable = false;
        std::string playlist;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            playlist = state_.playlistName;
            state_.handBack(now);
            handBackFetchDue_ = true;
            reachable = lastProbeSucceeded_;
            reportDue_ = !reachable;
            reportOwedBeforePosts_ = !reachable;
        }
        lastBoundary_.reset();
        occurrenceFinished_ = false;
        saveState(FallbackExecutionState());
        log(false, "handed back: playlist " + playlist + " stopped at " + std::to_string(static_cast<long long>(now)));
        if (reachable) {
            const showmesh::CoordinatorUrlLoad url = showmesh::loadCoordinatorBaseUrl(options_.stateDir);
            if (url.ok) sendStateReport(url.baseUrl, now);
        }
        wakeBackgroundThread();
    }

    // Sends the state as it is at this moment. Reports are never queued.
    void sendStateReport(const std::string& baseUrl, TimeMillis now) {
        std::lock_guard<std::mutex> reportLock(reportMutex_);
        StateReport report;
        report.bootId = bootId_;
        report.sequence = ++reportSequence_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            report.state = state_;
            reportDue_ = false;
            nextPeriodicReportAtMillis_ = now + kStateReportIntervalMillis;
        }
        const StateReportAnswer answer =
            SendStateReport(transport_, &credentials_, baseUrl, options_.fppInstanceUuid, report);
        const std::string problem = StateReportProblem(answer);
        std::string previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = stateReportProblem_;
            stateReportProblem_ = problem;
            if (answer.kind != StateReportAnswerKind::kCredentialUnavailable) ++stateReportsSent_;
        }
        if (!problem.empty() && problem != previous) log(true, "state report: " + problem);
        if (problem.empty() && !previous.empty()) log(false, "state report: accepted again");
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
            // The first report goes out as soon as there is a pairing token.
            reportDue_ = true;
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
        bool fetchDue = !fetchedOnce_ || now >= nextFetchAtMillis_ ||
                        nextFetchAtMillis_ - now > options_.programRefetchIntervalMillis;
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

        const bool enrolled = fetchAndInstall(baseUrl, now, key, /*atHandBack=*/false);
        if (stopRequested_.load()) return;
        // Registered, yet the published program does not carry the key: the
        // coordinator may have lost it, so the flag in memory is not trusted.
        if (programPublished_ && key.usable() && registeredNow() && !enrolled && now >= nextRegistrationAtMillis_) {
            if (registerKey(baseUrl, key.key, now)) nextFetchAtMillis_ = now;
        }
    }

    // Hand-back step 3: fetch at once, acknowledge the copy then held whether
    // or not it changed, and hand it to the nodes. Owed until the coordinator answers.
    void handBackFetch(const std::string& baseUrl, TimeMillis now) {
        const ExecutorKeyResult key = keyCopy();
        fetchAndInstall(baseUrl, now, key, /*atHandBack=*/true);
        if (lastFetchReachedCoordinator_) {
            std::lock_guard<std::mutex> lock(mutex_);
            handBackFetchDue_ = false;
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
    bool fetchAndInstall(const std::string& baseUrl, TimeMillis now, const ExecutorKeyResult& key, bool atHandBack) {
        fetchedOnce_ = true;
        lastFetchReachedCoordinator_ = false;
        nextFetchAtMillis_ = now + options_.programRefetchIntervalMillis;
        const InstalledProgram before = readInstalledProgram();
        if (options_.pinnedKey.status != PinnedKeyLoadStatus::kLoaded) {
            if (!pinnedKeyProblemLogged_) {
                log(true, std::string("no usable fallback program: ") +
                              PinnedKeyLoadStatusName(options_.pinnedKey.status) + " (" + options_.pinnedKey.error +
                              ")");
            }
            pinnedKeyProblemLogged_ = true;
            // Nothing can be fetched or acknowledged without the key, so nothing is owed.
            lastFetchReachedCoordinator_ = true;
            return false;
        }

        const InstalledProgramIdentity identity{before.packageId, before.revision, before.expiresAt};
        const FallbackFetchOutcome outcome = FetchAndInstallFallbackProgram(
            transport_, &credentials_, baseUrl, options_.fppInstanceUuid, options_.pinnedKey.publicKey,
            options_.installPath, options_.clock, before.present ? &identity : nullptr);
        lastFetchReachedCoordinator_ = outcome.kind != FallbackFetchOutcomeKind::kTransportUnreachable &&
                                       outcome.kind != FallbackFetchOutcomeKind::kCredentialUnavailable;
        const std::string outcomeLine =
            std::string("program fetch: ") + FallbackFetchOutcomeKindName(outcome.kind) + ": " + outcome.detail;
        if (outcomeLine != lastFetchOutcomeLine_) log(false, outcomeLine);
        lastFetchOutcomeLine_ = outcomeLine;
        programPublished_ = outcome.kind == FallbackFetchOutcomeKind::kInstalled ||
                            outcome.kind == FallbackFetchOutcomeKind::kUnchanged;
        const bool installed = outcome.kind == FallbackFetchOutcomeKind::kInstalled;

        // One acknowledge per verdict on a program, not one per fetch of a
        // program still refused. An unchanged copy is acknowledged only at a hand-back.
        const std::string verdict = outcome.packageId + "/" + outcome.revision + "/" +
                                    FallbackFetchOutcomeVerificationResult(outcome.kind);
        const bool heldCopyAtHandBack = atHandBack && programPublished_;
        const bool newVerdict = installed || verdict != lastAcknowledged_;
        if ((heldCopyAtHandBack || (ShouldAcknowledgeFallbackFetchOutcome(outcome) && newVerdict)) &&
            !stopRequested_.load()) {
            acknowledge(baseUrl, outcome, verdict);
        }
        const InstalledProgram program = installed ? readInstalledProgram() : before;
        // At a hand-back the fetch may have failed while a copy is still held: that copy is acknowledged.
        if (atHandBack && !programPublished_ && lastFetchReachedCoordinator_ && program.present &&
            !stopRequested_.load()) {
            FallbackFetchOutcome held;
            held.kind = FallbackFetchOutcomeKind::kUnchanged;
            held.packageId = program.packageId;
            held.revision = program.revision;
            acknowledge(baseUrl, held, std::string());
        }
        const bool enrolled = rememberProgram(program, key);
        if (!installed && !heldCopyAtHandBack) return enrolled;

        if (installed) resetRegistrationBackoff();
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

    void acknowledge(const std::string& baseUrl, const FallbackFetchOutcome& outcome, const std::string& verdict) {
        const AcknowledgeResult ack = AcknowledgeFallbackProgram(transport_, &credentials_, baseUrl,
                                                                 options_.fppInstanceUuid, outcome, options_.clock);
        if (ack.ok && !verdict.empty()) lastAcknowledged_ = verdict;
        if (!ack.ok) log(true, "acknowledge failed: " + ack.error);
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
        program.expiresAt = text(*body, "expiresAt");
        program.executorPublicKey = text(*body, "executorPublicKey");
        const showmesh::json::Value* rules = detail::findMember(*body, "rules");
        program.rulesKnown = rules != nullptr && rules->type() == showmesh::json::Type::kObject &&
                             rules->members().size() == 4 &&
                             text(*rules, "fallbackBoundary") == kRuleFallbackBoundary &&
                             text(*rules, "restHold") == kRuleRestHold &&
                             text(*rules, "localShutdown") == kRuleLocalShutdown &&
                             text(*rules, "recoveryBoundary") == kRuleRecoveryBoundary;
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

    // Section 5.13's usable program: installed, verified, before its expiry,
    // carrying this player's key, with exactly the four known rule values.
    // Returns this player's outcome word for what is missing, or empty.
    std::string usableProgramProblem(TimeMillis nowMillis) const {
        if (options_.pinnedKey.status != PinnedKeyLoadStatus::kLoaded) return kOutcomeNoCoordinatorKey;
        const auto now = std::chrono::system_clock::time_point(std::chrono::milliseconds(nowMillis));
        const ActivationResolution installed =
            ResolveInstalledActivation(std::string(), options_.installPath, options_.pinnedKey.publicKey, now);
        if (installed.kind == ActivationResolveKind::kNoProgramInstalled ||
            installed.kind == ActivationResolveKind::kProgramFailedReverification ||
            installed.kind == ActivationResolveKind::kProgramExpired) {
            return PlayerOutcomeWord(installed.kind);
        }
        const ExecutorKeyResult key = keyCopy();
        if (!key.usable()) return kOutcomeNoPlayerKey;
        const InstalledProgram program = readInstalledProgram();
        if (program.executorPublicKey != key.key.publicKeyBase64) return kOutcomePlayerKeyNotInProgram;
        if (!program.rulesKnown) return kOutcomeProgramRulesUnknown;
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
        FallbackMode mode = FallbackMode::kNormal;
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode = state_.mode;
            lost = detector_.confirmedLost();
        }
        const ExecutorKeyResult key = keyCopy();

        ActivationResolution resolution;
        std::string problem = usableProgramProblem(event.observedAtMillis);
        std::string detailForLog = problem;
        if (mode == FallbackMode::kResting) {
            problem = kOutcomeResting;
        } else if (problem.empty() && !event.identityResolved) {
            problem = kOutcomeEntryNotIdentified;
            detailForLog = "identity not resolved";
        } else if (problem.empty()) {
            const auto now = std::chrono::system_clock::time_point(std::chrono::milliseconds(event.observedAtMillis));
            resolution =
                ResolveInstalledActivation(event.entryKey, options_.installPath, options_.pinnedKey.publicKey, now);
            problem = PlayerOutcomeWord(resolution.kind);
            detailForLog = std::string(ActivationResolveKindName(resolution.kind)) + " " + resolution.reason;
        }
        log(false, detailForLog + " entryKey=" + event.entryKey +
                       " observedAtMillis=" + std::to_string(static_cast<long long>(event.observedAtMillis)));
        // In normal with the coordinator answering, or not confirmed lost, the entry is the coordinator's.
        if (mode == FallbackMode::kNormal && !lost) return;

        if (!problem.empty()) {
            // Outside fallback this is why it was not entered. Inside, why this entry got nothing.
            const bool nothingMapped = problem == kOutcomeUnknownEntry;
            recordPlayerDecision(event, std::string(), std::string(), problem);
            setBoundaryResult(nothingMapped ? BoundaryResult::kNothingToStart : BoundaryResult::kNotStarted,
                              nothingMapped ? std::string() : problem);
            return;
        }
        const ActivationMatch& match = *resolution.match;
        if (mode == FallbackMode::kNormal) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.enter(event.observedAtMillis, event.playlistName, match.packageId(), match.revision(),
                             match.programExpiresAt());
                reportDue_ = true;
            }
            log(true, "entered fallback under playlist " + event.playlistName + " at entryKey=" + event.entryKey);
        }
        deliverMatch(event, match, key.key, nullptr);
    }

    void setBoundaryResult(BoundaryResult result, const std::string& problem) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.lastBoundary = result;
        state_.lastBoundaryProblem = problem;
    }

    // recordedIds, when given, are the execution ids a restart found on disk
    // for this occurrence: only those nodes are asked, with those ids.
    void deliverMatch(const showmesh::FallbackEntryEvent& event, const ActivationMatch& match, const ExecutorKey& key,
                      const std::vector<std::pair<std::string, std::string>>* recordedIds) {
        std::lock_guard<std::mutex> deliveryLock(deliveryMutex_);
        struct Delivery {
            FallbackRecord record;
            std::string body;
        };
        std::vector<Delivery> deliveries;
        std::string firstProblem;
        RecordedOccurrence occurrence;
        occurrence.present = true;
        occurrence.identityResolved = event.identityResolved;
        occurrence.entryKey = event.entryKey;
        occurrence.playlistLoop = event.playlistLoop;
        for (const ActivationTarget& target : match.targets()) {
            std::string problem;
            std::string signature;
            std::string executionId;
            ActivationRequestBuild build;
            if (recordedIds != nullptr) {
                for (const auto& id : *recordedIds) {
                    if (id.first == target.nodeId) executionId = id.second;
                }
                if (executionId.empty()) continue;
            }
            if (!target.address.has_value()) {
                problem = kOutcomeNoAddress;
            } else {
                uint8_t random[16];
                if (executionId.empty() && options_.randomBytes != nullptr &&
                    options_.randomBytes(random, sizeof(random))) {
                    executionId = formatExecutionId(random);
                }
                build = BuildActivationRequest(match, target, executionId);
                if (!build.ok) {
                    problem = kOutcomeIncompleteProgram;
                } else if (executionId.empty() || !signWithExecutorKey(key, build.canonical, &signature)) {
                    problem = kOutcomeSigningFailed;
                }
            }
            if (!problem.empty()) {
                if (firstProblem.empty()) firstProblem = problem;
                recordPlayerDecision(event, target.nodeId, target.address.value_or(std::string()), problem);
                continue;
            }
            occurrence.executionIds.emplace_back(target.nodeId, executionId);
            Delivery delivery;
            delivery.record.atMillis = event.observedAtMillis;
            delivery.record.entryKey = event.entryKey;
            delivery.record.nodeId = target.nodeId;
            delivery.record.address = *target.address;
            delivery.record.executionId = executionId;
            delivery.body = ActivationRequestBody(build.canonical, signature);
            deliveries.push_back(std::move(delivery));
        }

        // On disk before the first activation leaves, so a restart can tell
        // what was already sent and with which ids.
        persistOccurrence(occurrence);

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
        bool stopped = false;
        for (const Delivery& delivery : deliveries) {
            if (delivery.record.outcome == "authorized") ++started;
            if (delivery.record.outcome == kOutcomeStopped) stopped = true;
            appendRecord(delivery.record, "activation", &recentActivations_, kRecentActivationRecords);
        }
        // A delivery cut short by a plugin stop stays unfinished on disk, so the next start retries it.
        occurrence.delivered = !stopped;
        persistOccurrence(occurrence);
        const std::size_t asked = recordedIds != nullptr ? recordedIds->size() : match.targets().size();
        setBoundaryResult(started == asked ? BoundaryResult::kStarted
                          : started > 0    ? BoundaryResult::kStartedOnSomeNodes
                                           : BoundaryResult::kNotStarted,
                          deliveries.empty() ? firstProblem : std::string());
    }

    void persistOccurrence(const RecordedOccurrence& occurrence) {
        FallbackExecutionState state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.mode != FallbackMode::kFallback) return;
            state_.occurrence = occurrence;
            state = state_;
        }
        saveState(state);
    }

    // After a restart that found an occurrence written but not finished: the
    // same nodes are asked again with the same execution ids, so a node that
    // already ran the cue answers replayed-execution instead of running it twice.
    void retryRecordedOccurrence(TimeMillis now) {
        FallbackExecutionState state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state = state_;
        }
        if (state.mode != FallbackMode::kFallback || !state.occurrence.present || state.occurrence.delivered) return;
        showmesh::FallbackEntryEvent event;
        event.action = showmesh::PlaylistAction::kPlaying;
        event.playlistName = state.playlistName;
        event.identityResolved = state.occurrence.identityResolved;
        event.entryKey = state.occurrence.entryKey;
        event.playlistLoop = state.occurrence.playlistLoop;
        event.observedAtMillis = now;
        const ExecutorKeyResult key = keyCopy();
        std::string problem = usableProgramProblem(now);
        ActivationResolution resolution;
        if (problem.empty()) {
            const auto at = std::chrono::system_clock::time_point(std::chrono::milliseconds(now));
            resolution =
                ResolveInstalledActivation(event.entryKey, options_.installPath, options_.pinnedKey.publicKey, at);
            problem = PlayerOutcomeWord(resolution.kind);
        }
        if (!problem.empty()) {
            recordPlayerDecision(event, std::string(), std::string(), problem);
            return;
        }
        log(false, "retrying the entry a restart interrupted, with its recorded execution ids");
        deliverMatch(event, *resolution.match, key.key, &state.occurrence.executionIds);
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

    // Keeps exactly one notice raised: the one that is true now, or none.
    // The state is read while holding the lock that guards the raise, so the
    // last caller to finish is the one with the newest state.
    void refreshNotice(TimeMillis now) {
        std::lock_guard<std::mutex> noticeLock(noticeMutex_);
        NoticeFacts facts;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            facts.state = state_;
            facts.coordinatorLost = detector_.confirmedLost();
            facts.coordinatorReachable = lastProbeSucceeded_;
        }
        if (facts.state.mode == FallbackMode::kFallback || (facts.state.mode == FallbackMode::kNormal && facts.coordinatorLost)) {
            facts.problem = usableProgramProblem(now);
            if (facts.problem.empty() && facts.state.mode == FallbackMode::kFallback &&
                facts.state.lastBoundary == BoundaryResult::kNotStarted) {
                facts.problem = facts.state.lastBoundaryProblem;
            }
        }
        const std::string wanted = FallbackNotice(facts);
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
            {"state", Value::makeString(FallbackModeName(snapshot.state.mode))},
            {"message", Value::makeString(notice())},
            {"sinceMillis", Value::makeNumber(static_cast<double>(snapshot.state.sinceMillis))},
            {"playlistName", Value::makeString(snapshot.state.playlistName)},
            {"cutoffAt", Value::makeString(snapshot.state.cutoffAt)},
            {"coordinatorReachable", Value::makeBool(snapshot.coordinatorReachable)},
            {"coordinatorLost", Value::makeBool(snapshot.coordinatorLost)},
            {"stateReportProblem", Value::makeString(snapshot.stateReportProblem)},
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
    std::string bootId_;

    // Guards everything both threads read: the detector, the state object,
    // the key, the report and registration flags, the program summary and the records.
    mutable std::mutex mutex_;
    CoordinatorOutageDetector detector_;
    FallbackExecutionState state_;
    ExecutorKeyResult key_;
    bool lastProbeSucceeded_ = false;
    // A state report is owed now: on start, on a state change, after a recovered probe.
    bool reportDue_ = true;
    TimeMillis nextPeriodicReportAtMillis_ = 0;
    // The report that must be the first request after a recovery has not gone out yet.
    bool reportOwedBeforePosts_ = false;
    // Hand-back step 3 is owed: fetch at once and acknowledge the copy then held.
    bool handBackFetchDue_ = false;
    std::string stateReportProblem_;
    std::uint64_t stateReportsSent_ = 0;
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

    // One report at a time, so sequence numbers leave in order.
    std::mutex reportMutex_;
    std::int64_t reportSequence_ = 0;
    // One boundary's deliveries at a time, and never beside a restart's retry.
    std::mutex deliveryMutex_;
    std::atomic<bool> retryOccurrenceDue_{false};

    // Background thread only.
    bool probedOnce_ = false;
    bool probedThisTick_ = false;
    TimeMillis lastProbeAtMillis_ = 0;
    bool fetchedOnce_ = false;
    // True when the last fetch found a published program, new or unchanged.
    bool programPublished_ = false;
    bool lastFetchReachedCoordinator_ = false;
    bool programSummaryLoaded_ = false;
    TimeMillis nextFetchAtMillis_ = 0;
    TimeMillis nextRegistrationAtMillis_ = 0;
    TimeMillis registrationBackoffMillis_ = 0;
    std::string lastRegistrationAnswer_;
    std::string lastFetchOutcomeLine_;
    std::string lastAcknowledged_;
    std::string tokenHash_;
    std::string keyProblem_;
    bool pinnedKeyProblemLogged_ = false;

    // Runtime worker thread only, after construction.
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
    bool woken_ = false;
};

}  // namespace fallback
}  // namespace showmesh
