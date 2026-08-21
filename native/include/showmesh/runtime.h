#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "showmesh/brightness.h"
#include "showmesh/callback_handoff.h"
#include "showmesh/playlist_identity.h"

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
    virtual void publish(const PlaylistEntryObservation& observation) = 0;
    // Reports an observation whose identity could not be established. It
    // is never silently downgraded to filename identity.
    virtual void publishUnavailable(const PlaylistEntryObservation& observation) = 0;
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

// ShowMeshRuntime owns the brightness engine, the callback handoff, and
// the worker thread. An adapter creates one, forwards FPP's lifecycle and
// callbacks into it, and does nothing else.
class ShowMeshRuntime {
 public:
    ShowMeshRuntime(PlaylistDefinitionSource* definitions, ObservationSink* sink, Clock clock);
    ~ShowMeshRuntime();

    BrightnessEngine& brightness() { return engine_; }

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

    const CallbackHandoff& handoff() const { return handoff_; }
    std::uint64_t publishedCount() const { return published_.load(); }
    std::uint64_t unavailableCount() const { return unavailable_.load(); }

 private:
    void workerLoop();

    PlaylistDefinitionSource* definitions_;
    ObservationSink* sink_;
    Clock clock_;

    BrightnessEngine engine_;
    CallbackHandoff handoff_;
    SequenceState sequence_;

    std::thread worker_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::atomic<bool> running_{false};

    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> unavailable_{0};
    // Gap evidence that has been counted but not yet acknowledged by a
    // successful publish. It is carried forward rather than cleared, so a
    // failed publish does not erase the record of what was dropped.
    std::uint32_t unacknowledgedCoalesced_ = 0;
};

}  // namespace showmesh
