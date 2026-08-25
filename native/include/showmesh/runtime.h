#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "showmesh/brightness.h"
#include "showmesh/callback_handoff.h"
#include "showmesh/playlist_identity.h"
#include "showmesh/sequence_store.h"

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

// ObservationSink is where a resolved playlist-entry observation goes.
//
// The coordinator sink is deliberately absent: the ingestion payload,
// endpoint, and scope are frozen by the coordinator's own contract, which
// does not exist yet. A sink invented here would have to be reconciled
// later against the real one, which is worse than not having it. The
// worker builds the complete observation and hands it to whatever sink is
// installed, so adding the real one is one class, not a rewrite.
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
};

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
    ShowMeshRuntime(PlaylistDefinitionSource* definitions, ObservationSink* sink, Clock clock,
                    SequenceFileStore* sequenceStore = nullptr);
    ~ShowMeshRuntime();

    // Guarded engine access. The returned accessor holds engineMutex_ for
    // its own lifetime; do not store it past the expression that uses it.
    EngineAccessor brightness() { return EngineAccessor(engine_, engineMutex_); }

    // Parses and applies the registered action's two string arguments.
    // Reports a message an operator can act on rather than throwing: this
    // is reached from FPP's command path, where an exception escaping the
    // plugin takes down more than the command.
    CommandOutcome applyBrightnessCommand(const std::string& targetPercent, const std::string& fadeSeconds);

    // Called from FPP's own callback thread. Bounded work only: copy and
    // return.
    void observeCallback(const char* playlistName, const char* action, const char* section, int item,
                         const char* sequenceFilename, const char* mediaFilename);

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

    // Persists the current sequence value immediately, independent of
    // drainOnce()'s own per-observation persistence. Every accepted post
    // already persists its own sequence number, so this is not needed for
    // that path to be durable; it exists as the seam a future explicit
    // shutdown callback can call for an extra, cheap guarantee before the
    // process exits. A no-op returning true when no sequence store is
    // configured.
    bool flushSequenceState();

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

    std::mutex engineMutex_;
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
    std::atomic<bool> running_{false};

    // Test seam only; see setTestHookBeforeWait().
    std::function<void()> testHookBeforeWait_;

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
