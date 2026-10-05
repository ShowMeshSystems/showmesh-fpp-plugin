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

// The program is refetched after this fraction of its own validity
// (expiresAt minus compiledAt) has passed since it was installed.
constexpr int kRefetchValidityNumerator = 1;
constexpr int kRefetchValidityDenominator = 3;
// How soon to ask again when a fetch installed nothing, or installed a
// program that does not yet carry this host's executor key.
constexpr TimeMillis kRefetchRetryMillis = 30000;
constexpr int kExecutorRegistrationTimeoutMillis = 10000;
constexpr std::size_t kRecentFallbackRecords = 50;
constexpr const char* kFallbackStatusFilename = "fallback-status.json";

// Words this plugin records for a target it sent nothing to.
constexpr const char* kOutcomeNoAddress = "no-address";
constexpr const char* kOutcomeIncompleteProgram = "incomplete-program";
constexpr const char* kOutcomeNoExecutorKey = "no-executor-key";
constexpr const char* kOutcomeExecutorKeyNotInProgram = "executor-key-not-in-program";
constexpr const char* kOutcomeEntryNotIdentified = "entry-not-identified";
constexpr const char* kOutcomeSigningFailed = "signing-failed";

constexpr const char* kCoordinatorLostMessage =
    "The coordinator has stopped answering, and this player will start the planned cues on the nodes itself from "
    "the next playlist entry. Check the coordinator.";
constexpr const char* kFallbackActiveMessage =
    "The coordinator stopped answering, so this player is starting the planned cues on the nodes itself until this "
    "playlist stops. Check the coordinator.";

enum class FallbackMode { kNormal, kFallback };

inline const char* FallbackModeName(FallbackMode mode) { return mode == FallbackMode::kFallback ? "fallback" : "normal"; }

// The one record of where this host stands in ADR-048's Normal, Fallback and
// Resting states. The cutoff, rest or hold rules and the hand-back at the
// next scheduled-show boundary extend this object; they are not built yet.
struct FallbackExecutionState {
    FallbackMode mode = FallbackMode::kNormal;
    TimeMillis enteredAtMillis = 0;
    std::string enteredAtEntryKey;

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
    std::string kind;  // "activation" or "program"
    std::string entryKey;
    std::string nodeId;
    std::string address;
    std::string executionId;
    std::string outcome;
    std::string reason;
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
    std::vector<FallbackRecord> recent;
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
    void tick(TimeMillis now) {
        // The second test keeps a clock stepped backwards from stalling the probe.
        const TimeMillis interval = options_.detector.probeIntervalMillis;
        if (probedOnce_ && now < nextProbeAtMillis_ && nextProbeAtMillis_ - now <= interval) return;
        nextProbeAtMillis_ = now + interval;
        probedOnce_ = true;
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
        refreshNotice();

        refreshCredentialAndKey();
        if (reached && !inFallback) keepCurrent(url.baseUrl, now);
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
        snapshot.recent.assign(recent_.begin(), recent_.end());
        return snapshot;
    }

 private:
    struct Boundary {
        bool identityResolved = false;
        std::string entryKey;
        std::optional<int> playlistLoop;
    };

    // What a refetch needs from the program it just installed.
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
            std::lock_guard<std::mutex> lock(mutex_);
            registered_ = false;
            registrationAttemptDue_ = true;
        }
        if (keyUsable()) return;
        ExecutorKeyResult loaded = LoadOrCreateExecutorKey(options_.credentialDir, options_.randomBytes);
        if (loaded.status == ExecutorKeyStatus::kCreated) log(false, "created this player's executor key");
        if (loaded.status == ExecutorKeyStatus::kUnusable && loaded.detail != keyProblem_) log(true, loaded.detail);
        keyProblem_ = loaded.detail;
        std::lock_guard<std::mutex> lock(mutex_);
        key_ = std::move(loaded);
    }

    bool keyUsable() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return key_.usable();
    }

    void keepCurrent(const std::string& baseUrl, TimeMillis now) {
        bool fetchDue = !fetchedOnce_ || now >= nextFetchAtMillis_ || nextFetchAtMillis_ - now > latestFetchDelayMillis_;
        ExecutorKeyResult key;
        bool attemptRegistration = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            key = key_;
            attemptRegistration = key_.usable() && !registered_ && (registrationAttemptDue_ || fetchDue);
            registrationAttemptDue_ = false;
        }
        if (attemptRegistration && registerKey(baseUrl, key.key)) fetchDue = true;
        if (fetchDue) fetchInstallAndDistribute(baseUrl, now, key);
    }

    // True when the coordinator stored a first or different key, so its program changes at once.
    bool registerKey(const std::string& baseUrl, const ExecutorKey& key) {
        const ExecutorRegistration registration =
            RegisterExecutorKey(transport_, &credentials_, baseUrl, options_.fppInstanceUuid, key.publicKeyBase64,
                                kExecutorRegistrationTimeoutMillis);
        std::string problem;
        switch (registration.kind) {
            case ExecutorRegistrationKind::kRegistered:
                log(false, std::string("executor key registered") + (registration.changed ? " (new)" : ""));
                break;
            case ExecutorRegistrationKind::kRefused:
                problem = "The coordinator answered " + std::to_string(registration.statusCode) +
                          (registration.detail.empty() ? std::string(".") : ": " + registration.detail);
                break;
            case ExecutorRegistrationKind::kCredentialUnavailable:
            case ExecutorRegistrationKind::kUnreachable:
                problem = registration.detail;
                break;
        }
        if (!problem.empty()) {
            log(true, "executor key not registered, tried again at the next program fetch: " + problem);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        registered_ = registration.kind == ExecutorRegistrationKind::kRegistered;
        registrationProblem_ = problem;
        return registered_ && registration.changed;
    }

    void fetchInstallAndDistribute(const std::string& baseUrl, TimeMillis now, const ExecutorKeyResult& key) {
        fetchedOnce_ = true;
        nextFetchAtMillis_ = now + kRefetchRetryMillis;
        latestFetchDelayMillis_ = kRefetchRetryMillis;
        if (options_.pinnedKey.status != PinnedKeyLoadStatus::kLoaded) {
            if (!pinnedKeyProblemLogged_) {
                log(true, std::string("no usable fallback program: ") +
                              PinnedKeyLoadStatusName(options_.pinnedKey.status) + " (" + options_.pinnedKey.error +
                              ")");
            }
            pinnedKeyProblemLogged_ = true;
            return;
        }

        const FallbackFetchOutcome outcome =
            FetchAndInstallFallbackProgram(transport_, &credentials_, baseUrl, options_.fppInstanceUuid,
                                           options_.pinnedKey.publicKey, options_.installPath, options_.clock);
        log(false, std::string("program fetch: ") + FallbackFetchOutcomeKindName(outcome.kind) + ": " + outcome.detail);
        if (ShouldAcknowledgeFallbackFetchOutcome(outcome)) {
            const AcknowledgeResult ack = AcknowledgeFallbackProgram(transport_, &credentials_, baseUrl,
                                                                     options_.fppInstanceUuid, outcome, options_.clock);
            if (!ack.ok) log(true, "acknowledge failed: " + ack.error);
        }
        if (outcome.kind != FallbackFetchOutcomeKind::kInstalled) return;

        const InstalledProgram program = readInstalledProgram();
        if (!program.present) return;
        const bool enrolled = key.usable() && program.executorPublicKey == key.key.publicKeyBase64;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            programPackageId_ = program.packageId;
            programRevision_ = program.revision;
            programExpiresAt_ = program.expiresAt;
            programEnrollsThisExecutor_ = enrolled;
        }
        if (enrolled || !key.usable()) nextFetchAtMillis_ = now + refetchDelayMillis(program);
        latestFetchDelayMillis_ = nextFetchAtMillis_ - now;
        for (const std::string& address : program.addresses) {
            if (stopRequested_.load()) return;
            const NodeAnswer answer =
                HandProgramToNode(transport_, address, options_.fppInstanceUuid, program.signedDocument);
            FallbackRecord record;
            record.atMillis = now;
            record.kind = "program";
            record.address = address;
            record.outcome = !answer.outcome.empty() ? answer.outcome
                             : answer.responded      ? kOutcomeUnrecognizedAnswer
                                                     : kOutcomeNoResponse;
            record.reason = answer.reason;
            record.attempts = 1;
            appendRecord(record);
        }
    }

    TimeMillis refetchDelayMillis(const InstalledProgram& program) const {
        std::int64_t compiledAt = 0;
        std::int64_t expiresAt = 0;
        if (!detail::parseRfc3339ToEpochSeconds(program.compiledAt, &compiledAt) ||
            !detail::parseRfc3339ToEpochSeconds(program.expiresAt, &expiresAt) || expiresAt <= compiledAt) {
            return kRefetchRetryMillis;
        }
        const TimeMillis validityMillis = static_cast<TimeMillis>(expiresAt - compiledAt) * 1000;
        const TimeMillis delay = validityMillis * kRefetchValidityNumerator / kRefetchValidityDenominator;
        return std::max<TimeMillis>(delay, options_.detector.probeIntervalMillis);
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
        ExecutorKeyResult key;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.mode == FallbackMode::kNormal && detector_.confirmedLost()) {
                state_.enter(event.observedAtMillis, event.entryKey);
                entered = true;
            }
            mode = state_.mode;
            key = key_;
        }
        if (entered) {
            log(true, "entered fallback at entryKey=" + event.entryKey);
            refreshNotice();
        }

        const bool havePinnedKey = options_.pinnedKey.status == PinnedKeyLoadStatus::kLoaded;
        ActivationResolution resolution;
        std::string resolutionName = kOutcomeEntryNotIdentified;
        std::string resolutionReason = "FPP's playlist entry could not be identified";
        if (event.identityResolved && !havePinnedKey) {
            resolutionName = PinnedKeyLoadStatusName(options_.pinnedKey.status);
            resolutionReason = options_.pinnedKey.error;
        } else if (event.identityResolved) {
            const auto now = std::chrono::system_clock::time_point(std::chrono::milliseconds(event.observedAtMillis));
            resolution =
                ResolveInstalledActivation(event.entryKey, options_.installPath, options_.pinnedKey.publicKey, now);
            resolutionName = ActivationResolveKindName(resolution.kind);
            resolutionReason = resolution.reason;
        }
        log(false, resolutionName + " entryKey=" + event.entryKey +
                       " observedAtMillis=" + std::to_string(static_cast<long long>(event.observedAtMillis)));
        if (mode != FallbackMode::kFallback) return;

        if (resolution.kind != ActivationResolveKind::kMatch || !resolution.match.has_value()) {
            recordRefusal(event, std::string(), std::string(), resolutionName, resolutionReason);
            return;
        }
        const ActivationMatch& match = *resolution.match;
        if (!key.usable()) {
            recordRefusal(event, std::string(), std::string(), kOutcomeNoExecutorKey,
                          "this player holds no executor key");
            return;
        }
        if (match.executorPublicKey() != key.key.publicKeyBase64) {
            recordRefusal(event, std::string(), std::string(), kOutcomeExecutorKeyNotInProgram,
                          "the installed program does not carry this player's executor key");
            return;
        }
        deliverMatch(event, match, key.key);
    }

    void deliverMatch(const showmesh::FallbackEntryEvent& event, const ActivationMatch& match,
                      const ExecutorKey& key) {
        struct Delivery {
            FallbackRecord record;
            std::string body;
        };
        std::vector<Delivery> deliveries;
        for (const ActivationTarget& target : match.targets()) {
            if (!target.address.has_value()) {
                recordRefusal(event, target.nodeId, std::string(), kOutcomeNoAddress,
                              "the installed program gives no address for this node");
                continue;
            }
            uint8_t random[16];
            std::string signature;
            const bool haveId = options_.randomBytes != nullptr && options_.randomBytes(random, sizeof(random));
            const std::string executionId = haveId ? formatExecutionId(random) : std::string();
            const ActivationRequestBuild build = BuildActivationRequest(match, target, executionId);
            if (!build.ok) {
                recordRefusal(event, target.nodeId, *target.address, kOutcomeIncompleteProgram, build.refusal);
                continue;
            }
            if (!haveId || !signWithExecutorKey(key, build.canonical, &signature)) {
                recordRefusal(event, target.nodeId, *target.address, kOutcomeSigningFailed,
                              "the request could not be signed");
                continue;
            }
            Delivery delivery;
            delivery.record.atMillis = event.observedAtMillis;
            delivery.record.kind = "activation";
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
            delivery->record.attempts = result.attempts;
        };
        std::vector<std::thread> threads;
        for (std::size_t i = 1; i < deliveries.size(); ++i) threads.emplace_back(run, &deliveries[i]);
        if (!deliveries.empty()) run(&deliveries[0]);
        for (std::thread& thread : threads) thread.join();
        for (const Delivery& delivery : deliveries) appendRecord(delivery.record);
    }

    bool waitOrStop(int millis) {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        return !wake_.wait_for(lock, std::chrono::milliseconds(millis), [this] { return stopRequested_.load(); });
    }

    void recordRefusal(const showmesh::FallbackEntryEvent& event, const std::string& nodeId,
                       const std::string& address, const std::string& outcome, const std::string& reason) {
        FallbackRecord record;
        record.atMillis = event.observedAtMillis;
        record.kind = "activation";
        record.entryKey = event.entryKey;
        record.nodeId = nodeId;
        record.address = address;
        record.outcome = outcome;
        record.reason = reason;
        appendRecord(record);
    }

    void appendRecord(const FallbackRecord& record) {
        const bool authorized = record.kind == "activation" && record.outcome == "authorized";
        log(record.outcome != "authorized" && record.outcome != "installed",
            record.kind + " entryKey=" + record.entryKey + " nodeId=" + record.nodeId + " address=" + record.address +
                " executionId=" + record.executionId + " outcome=" + record.outcome +
                " attempts=" + std::to_string(record.attempts) + " reason=" + record.reason);
        std::lock_guard<std::mutex> lock(mutex_);
        if (record.kind == "activation") ++(authorized ? activationsAuthorized_ : activationsNotDelivered_);
        recent_.push_back(record);
        while (recent_.size() > kRecentFallbackRecords) recent_.pop_front();
    }

    void leaveFallbackOnPlaylistStop(TimeMillis now) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.mode != FallbackMode::kFallback) return;
            state_.leaveOnPlaylistStop();
        }
        log(false, "left fallback: the playlist stopped at " + std::to_string(static_cast<long long>(now)));
        refreshNotice();
    }

    // Keeps exactly one notice raised: the one for the current state, or none.
    void refreshNotice() {
        std::string wanted;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.mode == FallbackMode::kFallback) {
                wanted = kFallbackActiveMessage;
            } else if (detector_.confirmedLost()) {
                wanted = kCoordinatorLostMessage;
            }
        }
        std::lock_guard<std::mutex> lock(noticeMutex_);
        if (wanted == raisedNotice_) return;
        if (options_.notifier != nullptr) {
            if (!raisedNotice_.empty()) options_.notifier->clear(raisedNotice_);
            if (!wanted.empty()) options_.notifier->raise(wanted);
        }
        raisedNotice_ = wanted;
    }

    void publishStatus(TimeMillis now) {
        if (options_.stateDir.empty()) return;
        const FallbackStatusSnapshot snapshot = status();
        using showmesh::json::Value;
        auto number = [](std::uint64_t n) { return Value::makeNumber(static_cast<double>(n)); };
        std::vector<Value> recent;
        for (const FallbackRecord& record : snapshot.recent) {
            recent.push_back(Value::makeObject({
                {"atMillis", Value::makeNumber(static_cast<double>(record.atMillis))},
                {"kind", Value::makeString(record.kind)},
                {"entryKey", Value::makeString(record.entryKey)},
                {"nodeId", Value::makeString(record.nodeId)},
                {"address", Value::makeString(record.address)},
                {"executionId", Value::makeString(record.executionId)},
                {"outcome", Value::makeString(record.outcome)},
                {"reason", Value::makeString(record.reason)},
                {"attempts", Value::makeNumber(record.attempts)},
            }));
        }
        const bool inFallback = snapshot.state.mode == FallbackMode::kFallback;
        const std::string message = inFallback                 ? kFallbackActiveMessage
                                    : snapshot.coordinatorLost ? kCoordinatorLostMessage
                                                               : "";
        const showmesh::json::CanonicalResult body = showmesh::json::canonicalize(Value::makeObject({
            {"mode", Value::makeString(FallbackModeName(snapshot.state.mode))},
            {"message", Value::makeString(message)},
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
            {"recent", Value::makeArray(std::move(recent))},
        }));
        if (!body.ok) return;
        std::lock_guard<std::mutex> lock(statusFileMutex_);
        if (body.text == writtenStatus_) return;
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
    std::deque<FallbackRecord> recent_;

    // Background thread only.
    bool probedOnce_ = false;
    TimeMillis nextProbeAtMillis_ = 0;
    bool fetchedOnce_ = false;
    TimeMillis nextFetchAtMillis_ = 0;
    // How far ahead the last fetch scheduled the next one. A longer wait means the clock stepped back.
    TimeMillis latestFetchDelayMillis_ = 0;
    std::string tokenHash_;
    std::string keyProblem_;
    bool pinnedKeyProblemLogged_ = false;

    // Runtime worker thread only.
    std::optional<Boundary> lastBoundary_;
    bool occurrenceFinished_ = false;

    std::mutex noticeMutex_;
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
