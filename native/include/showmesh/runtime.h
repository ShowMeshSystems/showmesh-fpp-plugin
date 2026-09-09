#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "showmesh/brightness.h"
#include "showmesh/brightness_store.h"
#include "showmesh/callback_handoff.h"
#include "showmesh/definition_republish.h"
#include "showmesh/playlist_identity.h"
#include "showmesh/sequence_store.h"
#include "showmesh/transition_gain.h"

// The adapter-facing runtime. Everything here is shared by the FPP 9 and
// FPP 10 adapters and knows nothing about either one's plugin lifecycle or
// HTTP framework: it takes already-extracted values, never an FPP type.
// The two adapters compile it separately into their own shared objects, so
// no FPP 9 and FPP 10 surface ever meets in one binary.

namespace showmesh {

// The registered FPP Action's name and its two arguments.
extern const char* const kBrightnessCommandName;
extern const char* const kTargetPercentArgument;
extern const char* const kFadeSecondsArgument;

// The identifier the plugin registers itself under, and the tag its
// MultiSync payload carries.
extern const char* const kPluginName;

// The floor on how often the worker re-reads every playlist definition on
// the host looking for one the coordinator does not hold. An operator who
// edits a playlist and does not play it would otherwise leave the
// coordinator on the previous revision until the next plugin restart.
constexpr TimeMillis kDefinitionRescanIntervalMillis = 60000;

// A playlist name arrives from FPP and is interpolated into a file path by
// the adapters. Anything that could climb out of the playlist directory is
// refused rather than sanitized, so a deformed name reads as an
// unavailable observation instead of reaching a file somewhere else on the
// host.
bool playlistNameIsPathSafe(const std::string& name);

struct CommandOutcome {
    bool ok = false;
    std::string message;
};

// What the constructor found on disk for brightness state, reported so
// the adapter can announce a safe settle loudly rather than let it pass
// silently. kTrustedOrNoRecord covers both a trusted primary (normal
// restart) and a true first run (nothing ever written): neither is
// anything an operator needs to hear about. The other two both mean
// BrightnessEngine::settleSafeAfterUntrustedRestart() ran.
enum class BrightnessRestartTrust {
    kTrustedOrNoRecord,
    // The primary record failed to parse; only a backup -- the state the
    // primary superseded -- was recoverable, and it was not trusted.
    kPrimaryUnreadableBackupRecovered,
    // Neither the primary nor the backup record could be parsed, despite
    // at least one existing on disk.
    kNeitherRecordReadable,
};

// ObservationSink is where a resolved playlist-entry observation goes.
// CoordinatorClient (coordinator_client.h) is the shipped implementation;
// the interface stays here so the runtime can be exercised without a
// transport and so no HTTP surface reaches the callback boundary.
class ObservationSink {
 public:
    virtual ~ObservationSink() = default;
    // Returns true when the observation was accepted. The caller must not
    // treat gap evidence as acknowledged, or count a publication as having
    // happened, on a false return.
    virtual bool publish(const PlaylistEntryObservation& observation) = 0;
    // Reports an observation whose identity could not be established. It
    // is never silently downgraded to filename identity. Returns true when
    // accepted; see publish().
    virtual bool publishUnavailable(const PlaylistEntryObservation& observation) = 0;
    // Asks an in-flight publish() or publishUnavailable() to give up its
    // retry budget and return promptly. A sink with nothing to interrupt
    // does nothing. ShowMeshRuntime::stop() calls this before joining the
    // worker thread, because a publish stuck in backoff against an
    // unreachable coordinator can otherwise hold the join for the retry
    // policy's full worst case, which FPP 10's shutdown deadline does not
    // allow.
    virtual void requestStop() {}
};

// PlaylistMismatchNotifier is where a mid-show playlist mismatch is
// reported: the coordinator's own reconciliation verdict on the plugin's
// last observation says the currently playing playlist no longer matches
// what the coordinator has bound. The plugin decides nothing about this
// itself; it mirrors whatever the coordinator's response said. Optional;
// nullptr keeps the previous behavior (no notification).
class PlaylistMismatchNotifier {
 public:
    virtual ~PlaylistMismatchNotifier() = default;
    // Called only on a transition into the mismatched state, or when the
    // instruction text itself changes while already mismatched. message
    // is the coordinator's own operatorInstruction for this verdict.
    virtual void raiseMismatch(int id, const std::string& message) = 0;
    // Called only on a transition out of the mismatched state (resolved,
    // or aged out). Always given the identical id and message the most
    // recent raiseMismatch() call gave: an implementation backed by
    // WarningHolder clears only on an exact (id, message, plugin) match.
    virtual void clearMismatch(int id, const std::string& message) = 0;
};

// The identity this notice is raised and cleared under. Defined once,
// here, so both FPP adapters and this repository's own tests reference
// the identical value instead of each holding their own copy that could
// drift apart.
//
// 0 rather than a curated FPP warning id: ShowMesh owns no entry in FPP's
// own www/warnings-definitions.json, and WarningHolder's own
// UNKNOWN_WARNING_ID is exactly this value, used the same way by FPP's
// own ad hoc, plugin-sourced warnings. RemoveWarning's exact-triple match
// (id, message, plugin) still makes this notice unambiguous.
constexpr int ShowMesh_PlaylistMismatch = 0;

// PlaylistDefinitionSource resolves a playlist's complete definition. The
// worker calls it, never the callback thread: on FPP this reads the
// playlist-definition API, and in a test it returns a fixture.
class PlaylistDefinitionSource {
 public:
    virtual ~PlaylistDefinitionSource() = default;
    // Returns the raw JSON definition, or an empty string when it cannot
    // be resolved.
    virtual std::string definitionFor(const std::string& playlistName) = 0;
    // The persistent instance UUID, or an empty string when unavailable.
    virtual std::string instanceUuid() = 0;
    // Every playlist definition on the host, by name. Empty by default so
    // a source that cannot enumerate simply publishes nothing at worker
    // start rather than failing; the FPP adapters list the playlist
    // directory they already read one file at a time.
    virtual std::vector<std::string> playlistNames() { return {}; }
};

// DefinitionPublisher carries the complete playlist definition the plugin
// hashed, plus its hash, to the coordinator. It is separate from
// ObservationSink because a definition is not an observation: it carries
// no sequence, is ordered against nothing, and is content addressed, so a
// failed publication can never wedge the observation path.
class DefinitionPublisher {
 public:
    virtual ~DefinitionPublisher() = default;
    // Returns true when the coordinator holds this hash, whether this
    // call put it there or a previous one did. An implementation is
    // expected to skip a hash it has already posted successfully.
    virtual bool publishDefinition(const std::string& instanceUuid, const std::string& playlistName,
                                   const std::string& playlistHash, const std::string& canonicalDefinition,
                                   TimeMillis capturedAtMillis) = 0;
    // See ObservationSink::requestStop(); the same reasoning applies to a
    // definition post stuck in backoff during sweepDefinitions().
    virtual void requestStop() {}

    // Drops every hash this publisher believes the coordinator holds and
    // reports all three counts from the one critical section that clears
    // it. Called from fppd's web thread, so the set must be guarded. The
    // terminally refused set is deliberately not cleared: those refusals
    // cannot change until the plugin restarts.
    virtual DefinitionHoldings clearHeldDefinitions() = 0;
    // The same counts without clearing anything, for section 3.9's
    // idempotent repeat. cleared is not meaningful here and is left 0.
    virtual DefinitionHoldings definitionHoldings() const = 0;
};

// Clock is injected so the whole runtime is testable without waiting.
using Clock = TimeMillis (*)();

// EngineAccessor is the only way to reach the shared BrightnessEngine. It
// holds the runtime's engine mutex for its own lifetime, so a caller can
// never retain an unguarded reference and read or write the engine off
// the lock: the mutex releases only when the accessor's temporary is
// destroyed at the end of the calling expression or statement.
class EngineAccessor {
 public:
    EngineAccessor(BrightnessEngine& engine, std::mutex& mutex) : engine_(engine), lock_(mutex) {}
    BrightnessEngine* operator->() { return &engine_; }
    const BrightnessEngine* operator->() const { return &engine_; }
    BrightnessEngine& operator*() { return engine_; }
    const BrightnessEngine& operator*() const { return engine_; }

 private:
    BrightnessEngine& engine_;
    std::lock_guard<std::mutex> lock_;
};

// ShowMeshRuntime owns the brightness engine, the callback handoff, and
// the worker thread. An adapter creates one, forwards FPP's lifecycle and
// callbacks into it, and does nothing else.
//
// engine_ is reached from three fppd threads: the output thread
// (modifyChannelData, encodeFullState), FPP's command thread
// (applyBrightnessCommand), and the MultiSync thread
// (adoptEncodedFullState). engineMutex_ is held across every mutation and
// every read of engine_, including through EngineAccessor, because
// FadingValue::fadeTo writes its target before its window, and a frame
// landing between those two writes would read the new target against the
// old, already-expired window.
class ShowMeshRuntime {
 public:
    // sequenceStore is optional. When non-null, the constructor restores
    // the in-memory sequence from sequenceStore->loadDetailed() (so a
    // restarted plugin resumes above the highest value it ever issued
    // instead of wedging every observation behind the coordinator's
    // monotonicity check), and every drained observation's freshly issued
    // sequence number is persisted before the observation is handed to
    // the sink. Passing nullptr keeps the previous behavior (always
    // starts at 0, nothing persisted), which existing tests rely on.
    //
    // brightnessStore is likewise optional. When non-null and it holds a
    // checksum-valid record, the constructor calls
    // BrightnessEngine::restoreFromPersisted with it, under engineMutex_,
    // before start() ever runs: see brightness_store.h for what that
    // restores and the darker-only guarantee it makes. Passing nullptr
    // leaves the engine at its built-in defaults, the previous behavior.
    // safeCeilingPercent governs only the untrusted-restart settle below;
    // it is read once here, at construction, because the restore happens
    // inside this constructor before an adapter could ever call a
    // setter, and it only matters at restore time -- no settings listener
    // is needed for it after that. Defaulted to the built-in constant so
    // every existing caller and test compiles unchanged; both adapters
    // pass the "ShowMeshSafeCeilingPercent" setting's value instead.
    ShowMeshRuntime(PlaylistDefinitionSource* definitions, ObservationSink* sink, Clock clock,
                    SequenceFileStore* sequenceStore = nullptr, DefinitionPublisher* definitions_publisher = nullptr,
                    BrightnessFileStore* brightnessStore = nullptr,
                    int safeCeilingPercent = kDefaultSafeCeilingPercent);
    ~ShowMeshRuntime();

    // Guarded engine access. The returned accessor holds engineMutex_ for
    // its own lifetime; do not store it past the expression that uses it.
    EngineAccessor brightness() { return EngineAccessor(engine_, engineMutex_); }

    // Parses and applies the registered action's two string arguments.
    // Reports a message an operator can act on rather than throwing: this
    // is reached from FPP's command path, where an exception escaping the
    // plugin takes down more than the command.
    CommandOutcome applyBrightnessCommand(const std::string& targetPercent, const std::string& fadeSeconds);

    // Serves one contract section 2.2 transition-gain write. Called from
    // fppd's own web thread, and synchronous by requirement rather than by
    // convenience: on FPP 10 the route is withdrawn with
    // unregisterPluginApi(), whose guarantee covers inbound HTTP only, so
    // a handler that handed this work to another thread would step outside
    // it and make the plugin unsafe to unload. Keep this synchronous.
    TransitionGainResponse applyTransitionGain(const std::string& body);

    // Serves one contract section 3.9 republish. Called from fppd's own
    // web thread, and synchronous for exactly the reason
    // applyTransitionGain() is. It clears the publisher's held set and
    // records that a sweep is owed; the sweep itself runs on the worker
    // thread, because it reads every definition on the host and posts
    // each one with the retry policy's full backoff budget.
    DefinitionRepublishResponse applyDefinitionRepublish(const std::string& body);

    // Called from FPP's own callback thread. Bounded work only: copy and
    // return.
    // playlistLoop is FPP's mainPlaylist pass counter when the callback
    // reported one, and std::nullopt when it did not. It defaults to
    // nullopt so a caller that has no counter (a test, or an FPP whose
    // callback JSON lacks the member) states absence by saying nothing.
    void observeCallback(const char* playlistName, const char* action, const char* section, int item,
                         const char* sequenceFilename, const char* mediaFilename,
                         std::optional<int> playlistLoop = std::nullopt);

    // Scales one output frame. Called on FPP's output thread.
    void modifyChannelData(std::uint8_t* channelData, std::size_t channelCount);

    // Full brightness state in and out, for MultiSync. Encoded rather than
    // sent as a struct so a node running a different build cannot
    // misinterpret a raw layout.
    std::string encodeFullState();
    StateAdoption adoptEncodedFullState(const std::uint8_t* data, int length);

    void start();
    // Stops and joins the worker. Safe to call more than once, and called
    // before the object is destroyed rather than from its destructor: a
    // thread still running while the object is torn down reads a
    // half-destroyed runtime.
    void stop();

    // Drains one pending observation on the caller's thread. The worker
    // loop is this in a loop; tests call it directly.
    bool drainOnce();

    // Publishes every playlist definition on the host, unconditionally.
    // Runs on the worker thread; returns false when there is nothing to
    // publish to or the instance UUID is not available yet, in which case
    // the re-scan clock is not started and the next worker pass tries
    // again.
    bool sweepDefinitions();

    // sweepDefinitions() the first time it is reached, and no more often
    // than kDefinitionRescanIntervalMillis after that. The start-up sweep
    // is what lets an operator author against a playlist while FPP is
    // idle and has played nothing.
    bool maybeSweepDefinitions();

    // Persists the current sequence value immediately, independent of
    // drainOnce()'s own per-observation persistence. Every accepted post
    // already persists its own sequence number, so this is not needed for
    // that path to be durable; it exists as the seam a future explicit
    // shutdown callback can call for an extra, cheap guarantee before the
    // process exits. A no-op returning true when no sequence store is
    // configured.
    bool flushSequenceState();

    // Persists the engine's current full state immediately, unless the
    // engine's revision has not changed since the last successful flush,
    // in which case this is a no-op that still returns true. A no-op
    // returning true when no brightness store is configured. engineMutex_
    // is held only long enough to read the revision and capture the
    // state; the store's actual write -- a read, a hash, two fsyncs, and
    // a rename -- runs with the lock released, so a caller never blocks
    // modifyChannelData, or another flush caller, for the duration of a
    // slow write. Safe to call from any thread; callers do not need to
    // serialize against each other, only against engine_ itself the way
    // every other engine_ access already does.
    bool flushBrightnessState();

    // Marks the engine's brightness state as needing to be persisted,
    // without touching disk itself: sets a flag and wakes the worker
    // thread, the same handoff observeCallback() already uses. Called
    // from FPP's per-frame output thread (via the adapters'
    // publishFullStateIfChanged) so the write flushBrightnessState()
    // actually does -- a full read, a SHA-256, two fsyncs, a rename --
    // never runs inside the output thread's frame budget. The worker
    // thread performs the write; see flushBrightnessIfDirty().
    void markBrightnessDirty();

    // What the worker thread calls once per loop iteration: flushes if
    // markBrightnessDirty() was called since the last flush, otherwise a
    // no-op that returns true. Exposed so a test can drive the dirty-flag
    // handoff deterministically without starting the worker thread.
    bool flushBrightnessIfDirty();

    // Invoked from the worker thread when an automatic (dirty-flag
    // triggered) brightness flush fails. Unset by default, so a caller
    // that never configures one -- every existing test -- sees no
    // behavior change. The FPP adapters set this to log through
    // LogErr, which native/src must never call directly: this is the
    // seam that lets a host-neutral write report failure through an
    // FPP-specific channel without including an FPP header here.
    void setBrightnessFlushFailureHandler(std::function<void()> handler) {
        brightnessFlushFailureHandler_ = std::move(handler);
    }

    // Set once, at construction, from what brightnessStore_->load() found
    // (or from BrightnessFileStore's default when no store is
    // configured). Read by the adapter constructor right after runtime_
    // itself finishes constructing, so it can log a
    // settleSafeAfterUntrustedRestart() event exactly once, loudly, at
    // startup, rather than let a settle to the safe ceiling pass with
    // nothing anywhere saying why. This is the seam: native/src cannot
    // call LogErr itself.
    BrightnessRestartTrust brightnessRestartTrust() const { return brightnessRestartTrust_; }

    const CallbackHandoff& handoff() const { return handoff_; }
    std::uint64_t publishedCount() const { return published_.load(); }
    std::uint64_t unavailableCount() const { return unavailable_.load(); }
    // Count of drainOnce() calls whose freshly minted sequence number
    // could not be persisted (sequenceStore->store() returned false). The
    // observation still publishes; only the durability guarantee is
    // broken, and a nonzero count here is the operator-visible sign of
    // it, since a restart before the underlying condition (missing or
    // unwritable state directory) is fixed resumes below what was
    // actually issued.
    std::uint64_t sequencePersistFailureCount() const { return sequencePersistFailures_.load(); }
    // True when construction found the primary or backup sequence-state
    // file present but neither one valid: a value was certainly issued
    // before this restart, its height is simply unknown, unlike a
    // genuine first run where neither file exists yet. Latched once at
    // construction; see SequenceFileStore::loadDetailed().
    bool sequenceFilesWereAllInvalidAtStartup() const { return sequenceFilesWereAllInvalidAtStartup_; }

    // Test seam only, never called in production. Runs on the worker
    // thread immediately after its last failed drainOnce() and
    // immediately before it takes wakeMutex_ to enter wait_for. This is
    // the exact gap a lost wakeup happens in: a notify landing here, before
    // the predicate is (re-)armed under the lock, must still be observed
    // by wait_for rather than requiring the 250ms poll fallback. A test
    // can block here on its own signal so a racing observeCallback() is
    // driven deterministically instead of depending on a sleep to usually
    // land in the gap.
    void setTestHookBeforeWait(std::function<void()> hook) { testHookBeforeWait_ = std::move(hook); }

 private:
    void workerLoop();

    PlaylistDefinitionSource* definitions_;
    ObservationSink* sink_;
    Clock clock_;
    SequenceFileStore* sequenceStore_;
    DefinitionPublisher* definitionPublisher_;
    BrightnessFileStore* brightnessStore_;
    // Set once in the constructor; see brightnessRestartTrust().
    BrightnessRestartTrust brightnessRestartTrust_ = BrightnessRestartTrust::kTrustedOrNoRecord;

    std::mutex engineMutex_;
    // The transition-gain route's idempotency memory, guarded by
    // engineMutex_ alongside the engine it gates writes to, so the seen
    // check and the write it guards cannot interleave with another
    // request. In memory only: a restart forgets it, which is correct
    // rather than a gap, because a restart has already lost the fade the
    // key was protecting.
    std::string lastTransitionGainRequestId_;
    BrightnessEngine engine_;
    CallbackHandoff handoff_;
    SequenceState sequence_;

    std::thread worker_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    // Guarded by wakeMutex_. Set before notify_one() and checked by the
    // worker's wait predicate, so a notification that arrives before the
    // worker starts waiting is not lost: the predicate already sees it
    // true instead of the worker blocking for up to 250ms regardless.
    bool hasWork_ = false;
    // Guarded by wakeMutex_, same as hasWork_ and for the same reason:
    // markBrightnessDirty() (any thread) sets it before notifying, and
    // flushBrightnessIfDirty() (worker thread only) clears it before
    // deciding whether to flush, so a dirty mark landing between those
    // two is never lost.
    bool brightnessDirty_ = false;
    // See setBrightnessFlushFailureHandler().
    std::function<void()> brightnessFlushFailureHandler_;
    // Guarded by engineMutex_; touched only inside flushBrightnessState().
    // Lets a redundant flush at an unchanged revision -- the operator-
    // command flush followed by the next frame's now-stale dirty mark --
    // skip the write instead of rotating an identical record into the
    // backup slot for nothing.
    std::uint64_t flushedBrightnessRevision_ = 0;
    bool brightnessEverFlushed_ = false;
    std::atomic<bool> running_{false};
    // True only while workerLoop() is on the stack. It is what lets a
    // sweep abandon its remaining definitions when stop() is waiting to
    // join, without making a direct sweepDefinitions() call from a test
    // (where no worker is running) look like a stop request.
    std::atomic<bool> workerActive_{false};

    // Test seam only; see setTestHookBeforeWait().
    std::function<void()> testHookBeforeWait_;

    // Worker-thread only; see sweepDefinitions(). The inbound republish
    // route never touches these: see sweepRequested_.
    bool sweptOnce_ = false;
    TimeMillis lastSweepMillis_ = 0;

    // The owed-sweep record contract section 3.9 requires, and the only
    // state the HTTP thread and the worker thread share for it. The route
    // increments sweepRequested_; the worker sweeps whenever the two
    // disagree, regardless of the cadence above, and then stores the
    // value it observed at the start of that sweep into sweepCompleted_.
    // Counters rather than a flag so a republish arriving mid-sweep is
    // not swallowed by the sweep that was already running.
    std::atomic<std::uint64_t> sweepRequested_{0};
    std::atomic<std::uint64_t> sweepCompleted_{0};

    // The republish route's idempotency memory and the SweepRecord it
    // writes through. republishMutex_ guards the id alone; the held-set
    // clear runs under the publisher's own lock.
    class RuntimeSweepRecord : public SweepRecord {
     public:
        explicit RuntimeSweepRecord(ShowMeshRuntime* runtime) : runtime_(runtime) {}
        void requestSweep() override;
        bool sweepPending() const override;

     private:
        ShowMeshRuntime* runtime_;
    };
    RuntimeSweepRecord sweepRecord_{this};
    std::mutex republishMutex_;
    std::string lastRepublishRequestId_;

    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> unavailable_{0};
    std::atomic<std::uint64_t> sequencePersistFailures_{0};
    // Written once from the constructor, before start() runs the worker
    // thread; read-only afterward, so no lock is needed.
    bool sequenceFilesWereAllInvalidAtStartup_ = false;
    // Gap evidence that has been counted but not yet acknowledged by a
    // successful publish. It is carried forward rather than cleared, so a
    // failed publish does not erase the record of what was dropped.
    std::uint32_t unacknowledgedCoalesced_ = 0;
};

}  // namespace showmesh
