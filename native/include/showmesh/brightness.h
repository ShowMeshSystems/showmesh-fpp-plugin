#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "showmesh/fading_value.h"

namespace showmesh {

// The ceiling is written by FPP's own schedule and operator command path;
// the transition gain is written only by the coordinator's night-session
// contract. Both are 0-100 and default to 100.
constexpr int kMinPercent = 0;
constexpr int kMaxPercent = 100;

// The registered action's own bound on a fade, one day in seconds.
constexpr std::int64_t kMaxFadeSeconds = 86400;

// A half-open channel span, one-based to match how FPP addresses channels
// in its own configuration.
struct ChannelRange {
    std::uint32_t startChannel = 1;
    std::uint32_t channelCount = 0;

    std::uint32_t endExclusive() const { return startChannel + channelCount; }
};

// RangeConfig selects which channels the engine may scale. Channels
// outside every apply range, and channels inside any exclude range, are
// left byte-identical. An empty apply list means the whole universe.
struct RangeConfig {
    std::vector<ChannelRange> apply;
    std::vector<ChannelRange> exclude;
};

struct ValidationResult {
    bool ok = true;
    std::string error;

    static ValidationResult failure(std::string message) { return ValidationResult{false, std::move(message)}; }
};

// Rejects a zero-length range, a range running past totalChannels, and any
// two ranges in the same list that overlap. An apply range and an exclude
// range are expected to overlap: that is what an exclusion is.
ValidationResult validateRanges(const RangeConfig& config, std::uint32_t totalChannels);

ValidationResult validateActionInput(int targetPercent, std::int64_t fadeSeconds);

// The schema version of the full-state payload exchanged between nodes and
// written to disk. Bumped when the payload's meaning changes; a node
// receiving a version it does not understand rejects the payload rather
// than guessing at it.
constexpr int kBrightnessStateSchemaVersion = 1;

// BrightnessState is the complete state of both values, including any
// active fade, plus a monotonic revision. It is what MultiSync carries and
// what persistence stores: never a relative adjustment, so applying it
// twice is indistinguishable from applying it once.
struct BrightnessState {
    int schemaVersion = kBrightnessStateSchemaVersion;
    std::uint64_t revision = 0;

    double ceilingStart = 100.0;
    double ceilingTarget = 100.0;
    TimeMillis ceilingFadeStartMillis = 0;
    TimeMillis ceilingFadeEndMillis = 0;

    double gainStart = 100.0;
    double gainTarget = 100.0;
    TimeMillis gainFadeStartMillis = 0;
    TimeMillis gainFadeEndMillis = 0;

    // The effective ceiling this node last actually applied to channel
    // data, recorded so a restart that cannot trust the fade timing can
    // still choose the darker of what was applied and what was intended.
    double lastAppliedCeiling = 100.0;
    double lastAppliedGain = 100.0;
    TimeMillis persistedAtMillis = 0;
};

enum class StateAdoption {
    kAdopted,
    kRejectedStaleRevision,
    kRejectedUnsupportedVersion,
};

// BrightnessEngine owns the composition, the two fades, and the channel
// ranges. It knows nothing about FPP: an adapter feeds it a frame buffer
// and the current time.
class BrightnessEngine {
 public:
    BrightnessEngine() = default;

    ValidationResult configureRanges(const RangeConfig& config, std::uint32_t totalChannels);
    const RangeConfig& ranges() const { return ranges_; }

    // Sets the ceiling, the value FPP's scheduler owns. Rejects input
    // outside the registered action's own declared bounds rather than
    // clamping it, so a mistyped scheduler entry is visible instead of
    // silently rounded into range.
    ValidationResult setCeiling(int targetPercent, std::int64_t fadeSeconds, TimeMillis now);

    // Sets the transition gain, the value the coordinator owns. Not
    // reachable from any FPP action; exposing it there would add a second
    // writer to a value with one owner.
    ValidationResult setGain(int targetPercent, std::int64_t fadeSeconds, TimeMillis now);

    double ceilingAt(TimeMillis now) const { return ceiling_.valueAt(now); }
    double gainAt(TimeMillis now) const { return gain_.valueAt(now); }

    // The composed percentage actually applied to channel data.
    int effectivePercentAt(TimeMillis now) const;

    bool fadingAt(TimeMillis now) const { return ceiling_.fadingAt(now) || gain_.fadingAt(now); }

    // Scales one frame in place. Channels outside the configured ranges,
    // and channels inside an exclusion, are not written at all.
    void applyToFrame(std::uint8_t* channelData, std::size_t channelCount, TimeMillis now);

    // Full-state exchange. captureState is what this node publishes and
    // persists; adoptState is what it does with another node's or a
    // previous process's state.
    BrightnessState captureState(TimeMillis now) const;
    StateAdoption adoptState(const BrightnessState& state);

    // Restores persisted state after a restart. When the persisted timing
    // cannot be trusted (a clock that moved backwards, an inverted or
    // impossible fade window, or an unsupported schema version) the engine
    // settles on the darker of the last applied value and the fade target
    // rather than resuming a fade it cannot place, and never on a brighter
    // value than either.
    StateAdoption restoreFromPersisted(const BrightnessState& state, TimeMillis now);

    std::uint64_t revision() const { return revision_; }

 private:
    void bumpRevision() { ++revision_; }
    void recomputeScaledSpans();

    FadingValue ceiling_{100.0};
    FadingValue gain_{100.0};
    RangeConfig ranges_;
    // The apply ranges with the exclusions removed, sorted and merged.
    // FPP hands the plugin its whole channel buffer, which is megabytes, so
    // a per-channel range test would cost the frame budget on every frame
    // regardless of how few channels are actually configured.
    std::vector<ChannelRange> scaledSpans_;
    std::uint32_t totalChannels_ = 0;
    std::uint64_t revision_ = 0;
    double lastAppliedCeiling_ = 100.0;
    double lastAppliedGain_ = 100.0;
};

}  // namespace showmesh
