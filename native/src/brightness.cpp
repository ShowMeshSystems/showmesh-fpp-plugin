#include "showmesh/brightness.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "showmesh/json.h"
#include "showmesh/sha256.h"

namespace showmesh {
namespace {

std::string describeRange(const ChannelRange& r) {
    return "channel " + std::to_string(r.startChannel) + " count " + std::to_string(r.channelCount);
}

ValidationResult validateOneList(const std::vector<ChannelRange>& ranges, std::uint32_t totalChannels,
                                 const char* listName) {
    for (const ChannelRange& r : ranges) {
        if (r.channelCount == 0) {
            return ValidationResult::failure(std::string(listName) + " range covers no channels (" + describeRange(r) + ")");
        }
        if (r.startChannel == 0) {
            return ValidationResult::failure(std::string(listName) + " range starts below channel 1 (" + describeRange(r) + ")");
        }
        if (totalChannels != 0 && r.endExclusive() > totalChannels + 1) {
            return ValidationResult::failure(std::string(listName) + " range runs past the configured channel count (" +
                                             describeRange(r) + ")");
        }
    }
    std::vector<ChannelRange> sorted = ranges;
    std::sort(sorted.begin(), sorted.end(),
              [](const ChannelRange& a, const ChannelRange& b) { return a.startChannel < b.startChannel; });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i].startChannel < sorted[i - 1].endExclusive()) {
            return ValidationResult::failure(std::string(listName) + " ranges overlap (" + describeRange(sorted[i - 1]) +
                                             " and " + describeRange(sorted[i]) + ")");
        }
    }
    return ValidationResult{};
}

double clampPercent(double v) {
    if (v < static_cast<double>(kMinPercent)) return static_cast<double>(kMinPercent);
    if (v > static_cast<double>(kMaxPercent)) return static_cast<double>(kMaxPercent);
    return v;
}

// stateChangedAtMillis is the ordering key, not a fade endpoint, but a
// hostile or corrupted value here is the same class of wedge: zero is the
// "never changed locally" sentinel a fresh engine reports, and everything
// else must fall inside the plausible epoch band.
bool timestampIsPlausible(TimeMillis t) {
    if (t == 0) return true;
    return t >= kEarliestPlausibleEpochMillis && t <= kLatestPlausibleEpochMillis;
}

// The third tier of the MultiSync ordering key. Two states can share an
// equal (stateChangedAtMillis, instanceId), most commonly two nodes that
// both have no instanceId yet; hashing the actual value fields is what
// still gives them a total order instead of each rejecting the other.
// revision, schemaVersion, instanceId, stateChangedAtMillis, and
// persistedAtMillis are deliberately excluded: they are either
// bookkeeping or already compared by the first two tiers, and including
// persistedAtMillis in particular would make the hash differ on every
// write of otherwise-identical content. lastAppliedCeiling and
// lastAppliedGain are also excluded: applyToFrame rewrites both on every
// output frame and adoptState never adopts them, so they are private
// per-node render history, not shared state, and including them means no
// two nodes' hashes of "the same" state ever settle on one value.
std::string orderingContentHash(const BrightnessState& state) {
    std::vector<json::Value::Member> members;
    members.emplace_back("ceilingStart", json::Value::makeNumber(state.ceilingStart));
    members.emplace_back("ceilingTarget", json::Value::makeNumber(state.ceilingTarget));
    members.emplace_back("ceilingFadeStartMillis", json::Value::makeNumber(static_cast<double>(state.ceilingFadeStartMillis)));
    members.emplace_back("ceilingFadeEndMillis", json::Value::makeNumber(static_cast<double>(state.ceilingFadeEndMillis)));
    members.emplace_back("gainStart", json::Value::makeNumber(state.gainStart));
    members.emplace_back("gainTarget", json::Value::makeNumber(state.gainTarget));
    members.emplace_back("gainFadeStartMillis", json::Value::makeNumber(static_cast<double>(state.gainFadeStartMillis)));
    members.emplace_back("gainFadeEndMillis", json::Value::makeNumber(static_cast<double>(state.gainFadeEndMillis)));
    json::CanonicalResult canonical = json::canonicalize(json::Value::makeObject(std::move(members)));
    return sha256Hex(canonical.ok ? canonical.text : std::string());
}

}  // namespace

ValidationResult validateRanges(const RangeConfig& config, std::uint32_t totalChannels) {
    ValidationResult applyResult = validateOneList(config.apply, totalChannels, "apply");
    if (!applyResult.ok) return applyResult;
    return validateOneList(config.exclude, totalChannels, "exclude");
}

ValidationResult validateActionInput(int targetPercent, std::int64_t fadeSeconds) {
    if (targetPercent < kMinPercent || targetPercent > kMaxPercent) {
        return ValidationResult::failure("target percent must be between 0 and 100, got " + std::to_string(targetPercent));
    }
    if (fadeSeconds < 0 || fadeSeconds > kMaxFadeSeconds) {
        return ValidationResult::failure("fade seconds must be between 0 and 86400, got " + std::to_string(fadeSeconds));
    }
    return ValidationResult{};
}

// A local change always orders strictly after whatever it replaces:
// max(now, stateChangedAtMillis_ + 1) rather than plain now, so a local
// command landing in the same millisecond as an already-adopted peer
// state is never lost to it, and a host whose clock is stepped backwards
// by NTP still outranks what it is replacing. The stored ordering key is
// recorded here too, from the state actually being published (own
// instanceId_, hash of the value fields excluding private render
// history), rather than recomputed on every future comparison.
void BrightnessEngine::bumpRevision(TimeMillis now) {
    ++revision_;
    stateChangedAtMillis_ = std::max(now, stateChangedAtMillis_ + 1);
    orderingInstanceId_ = instanceId_;
    orderingHash_ = orderingContentHash(captureState(now));
}

ValidationResult BrightnessEngine::configureRanges(const RangeConfig& config, std::uint32_t totalChannels) {
    ValidationResult result = validateRanges(config, totalChannels);
    if (!result.ok) return result;
    ranges_ = config;
    totalChannels_ = totalChannels;
    recomputeScaledSpans();
    return ValidationResult{};
}

ValidationResult BrightnessEngine::setCeiling(int targetPercent, std::int64_t fadeSeconds, TimeMillis now) {
    ValidationResult result = validateActionInput(targetPercent, fadeSeconds);
    if (!result.ok) return result;
    ceiling_.fadeTo(static_cast<double>(targetPercent), fadeSeconds * 1000, now);
    bumpRevision(now);
    return ValidationResult{};
}

ValidationResult BrightnessEngine::setGain(int targetPercent, std::int64_t fadeSeconds, TimeMillis now) {
    ValidationResult result = validateActionInput(targetPercent, fadeSeconds);
    if (!result.ok) return result;
    gain_.fadeTo(static_cast<double>(targetPercent), fadeSeconds * 1000, now);
    bumpRevision(now);
    return ValidationResult{};
}

int BrightnessEngine::effectivePercentAt(TimeMillis now) const {
    const double composed = ceiling_.valueAt(now) * gain_.valueAt(now) / 100.0;
    const long rounded = std::lround(clampPercent(composed));
    return static_cast<int>(rounded);
}

void BrightnessEngine::recomputeScaledSpans() {
    scaledSpans_.clear();
    std::vector<ChannelRange> spans = ranges_.apply;
    if (spans.empty()) {
        // No configured apply range means the whole universe, expressed as
        // one span so the frame loop shape is the same either way.
        spans.push_back(ChannelRange{1, totalChannels_ != 0 ? totalChannels_ : 0xFFFFFFFFu - 1});
    }
    std::sort(spans.begin(), spans.end(),
              [](const ChannelRange& a, const ChannelRange& b) { return a.startChannel < b.startChannel; });

    std::vector<ChannelRange> excludes = ranges_.exclude;
    std::sort(excludes.begin(), excludes.end(),
              [](const ChannelRange& a, const ChannelRange& b) { return a.startChannel < b.startChannel; });

    for (const ChannelRange& span : spans) {
        std::uint32_t cursor = span.startChannel;
        const std::uint32_t end = span.endExclusive();
        for (const ChannelRange& ex : excludes) {
            if (ex.endExclusive() <= cursor) continue;
            if (ex.startChannel >= end) break;
            if (ex.startChannel > cursor) {
                scaledSpans_.push_back(ChannelRange{cursor, ex.startChannel - cursor});
            }
            cursor = std::max(cursor, ex.endExclusive());
            if (cursor >= end) break;
        }
        if (cursor < end) {
            scaledSpans_.push_back(ChannelRange{cursor, end - cursor});
        }
    }
}

void BrightnessEngine::applyToFrame(std::uint8_t* channelData, std::size_t channelCount, TimeMillis now) {
    lastAppliedCeiling_ = ceiling_.valueAt(now);
    lastAppliedGain_ = gain_.valueAt(now);
    if (channelData == nullptr || channelCount == 0) return;

    const int percent = effectivePercentAt(now);
    if (percent == kMaxPercent) return;

    if (scaledSpans_.empty()) recomputeScaledSpans();

    const unsigned scale = static_cast<unsigned>(percent);
    for (const ChannelRange& span : scaledSpans_) {
        if (span.startChannel == 0) continue;
        const std::size_t begin = span.startChannel - 1;
        if (begin >= channelCount) continue;
        std::size_t end = begin + span.channelCount;
        if (end > channelCount) end = channelCount;
        for (std::size_t i = begin; i < end; ++i) {
            channelData[i] = static_cast<std::uint8_t>((channelData[i] * scale + 50u) / 100u);
        }
    }
}

BrightnessState BrightnessEngine::captureState(TimeMillis now) const {
    BrightnessState s;
    s.schemaVersion = kBrightnessStateSchemaVersion;
    s.revision = revision_;
    s.stateChangedAtMillis = stateChangedAtMillis_;
    s.instanceId = instanceId_;
    s.ceilingStart = ceiling_.start();
    s.ceilingTarget = ceiling_.target();
    s.ceilingFadeStartMillis = ceiling_.startMillis();
    s.ceilingFadeEndMillis = ceiling_.endMillis();
    s.gainStart = gain_.start();
    s.gainTarget = gain_.target();
    s.gainFadeStartMillis = gain_.startMillis();
    s.gainFadeEndMillis = gain_.endMillis();
    s.lastAppliedCeiling = lastAppliedCeiling_;
    s.lastAppliedGain = lastAppliedGain_;
    s.persistedAtMillis = now;
    return s;
}

namespace {

// A fade window is plausible when it is not inverted, its endpoints fall
// in a sane epoch band, and its span does not exceed the registered
// action's own maximum. The zero/zero sentinel means no fade is recorded.
// Spans are computed in double so no adopted magnitude can overflow this
// check itself.
bool fadeWindowIsPlausible(TimeMillis start, TimeMillis end) {
    if (start == 0 && end == 0) return true;
    if (end < start) return false;
    if (start < kEarliestPlausibleEpochMillis || start > kLatestPlausibleEpochMillis) return false;
    if (end < kEarliestPlausibleEpochMillis || end > kLatestPlausibleEpochMillis) return false;
    const double spanMillis = static_cast<double>(end) - static_cast<double>(start);
    return spanMillis <= static_cast<double>(kMaxFadeSeconds) * 1000.0;
}

}  // namespace

StateAdoption BrightnessEngine::adoptState(const BrightnessState& state, TimeMillis now) {
    if (state.schemaVersion != kBrightnessStateSchemaVersion) {
        return StateAdoption::kRejectedUnsupportedVersion;
    }
    if (!timestampIsPlausible(state.stateChangedAtMillis)) {
        return StateAdoption::kRejectedImplausibleTimestamp;
    }
    // A value inside the absolute epoch band can still be implausibly far
    // ahead of this node's own clock: one unauthenticated datagram parked
    // near the top of that band would otherwise be adopted once and then
    // outrank every legitimate peer state, and this node's own later
    // commands, for the rest of the process's life. now is the receiver's
    // own clock, not the sender's, which is why this check lives here
    // rather than in the sender-agnostic timestampIsPlausible above.
    if (state.stateChangedAtMillis > now + kMaxOrderingKeyAheadOfNowMillis) {
        return StateAdoption::kRejectedImplausibleTimestamp;
    }
    if (!fadeWindowIsPlausible(state.ceilingFadeStartMillis, state.ceilingFadeEndMillis) ||
        !fadeWindowIsPlausible(state.gainFadeStartMillis, state.gainFadeEndMillis)) {
        return StateAdoption::kRejectedInvalidFadeWindow;
    }
    // Full state is ordered by (stateChangedAtMillis, instanceId,
    // canonicalStateHash) compared lexicographically as a total order, not
    // by the sender's local revision counter, so two nodes that each ran
    // one command converge on the same state regardless of which one
    // adopts first. The hash tier matters because two nodes with an empty
    // instanceId and an equal timestamp would otherwise tie and each
    // reject the other's state forever.
    //
    // The current side of the comparison is the stored ordering key, not
    // a value recomputed from this node's live state: recomputing it from
    // instanceId_ and a fresh captureState() is what let per-node private
    // fields (and the sender's own instanceId, once adoption stopped
    // taking it) leak into the comparison, so a byte-identical replayed
    // payload never settled to equal and kept re-adopting itself.
    const std::string incomingHash = orderingContentHash(state);
    bool newer;
    if (state.stateChangedAtMillis != stateChangedAtMillis_) {
        newer = state.stateChangedAtMillis > stateChangedAtMillis_;
    } else if (state.instanceId != orderingInstanceId_) {
        newer = state.instanceId > orderingInstanceId_;
    } else {
        newer = incomingHash > orderingHash_;
    }
    if (!newer) {
        return StateAdoption::kRejectedStaleRevision;
    }
    ceiling_.restore(clampPercent(state.ceilingStart), clampPercent(state.ceilingTarget), state.ceilingFadeStartMillis,
                     state.ceilingFadeEndMillis);
    gain_.restore(clampPercent(state.gainStart), clampPercent(state.gainTarget), state.gainFadeStartMillis,
                  state.gainFadeEndMillis);
    stateChangedAtMillis_ = state.stateChangedAtMillis;
    // The stored ordering key becomes exactly the key received, not one
    // recomputed from this node's own state after adopting: see the
    // comment above and BrightnessEngine::bumpRevision for the local-
    // change side of the same rule.
    orderingInstanceId_ = state.instanceId;
    orderingHash_ = incomingHash;
    // instanceId_ is this node's own persistent identity, never assigned
    // from an adopted payload: doing so would make this node permanently
    // impersonate the peer it just adopted from, including across a
    // restart once captureState() persists it.
    // revision_ is local-only: bumped so this node knows its own state
    // changed and republishes, never copied from the sender.
    ++revision_;
    return StateAdoption::kAdopted;
}

namespace {

// A persisted fade is placeable only if its window is coherent and the
// current time is not before the window began. A clock that moved
// backwards across the restart makes the recorded window meaningless.
// Callers must not reach this for the start==end==0 "no fade recorded"
// sentinel or for an inverted window; both are handled before this runs.
bool fadeTimingIsTrustworthy(TimeMillis startMillis, TimeMillis endMillis, TimeMillis persistedAt, TimeMillis now) {
    if (endMillis <= startMillis) return false;
    if (now < startMillis) return false;
    if (persistedAt != 0 && now < persistedAt) return false;
    return true;
}

// Resolves one persisted fade window to one of three outcomes: no fade was
// recorded, the window is inverted or its magnitude is implausible (never
// trustworthy, must settle dark; this is the same fadeWindowIsPlausible
// check adoptState applies, which restoreFromPersisted must not skip), or
// it may be a real fade that fadeTimingIsTrustworthy still has to clear.
enum class FadeWindowShape { kNone, kImplausible, kCandidate };

FadeWindowShape classifyFadeWindow(TimeMillis startMillis, TimeMillis endMillis) {
    if (startMillis == 0 && endMillis == 0) return FadeWindowShape::kNone;
    if (!fadeWindowIsPlausible(startMillis, endMillis)) return FadeWindowShape::kImplausible;
    return FadeWindowShape::kCandidate;
}

}  // namespace

StateAdoption BrightnessEngine::restoreFromPersisted(const BrightnessState& state, TimeMillis now) {
    const double safeCeiling = std::min(clampPercent(state.lastAppliedCeiling), clampPercent(state.ceilingTarget));
    const double safeGain = std::min(clampPercent(state.lastAppliedGain), clampPercent(state.gainTarget));

    if (state.schemaVersion != kBrightnessStateSchemaVersion) {
        // The field meanings may have changed, so no fade is resumed and
        // no revision is adopted. The two values are still settled at the
        // darker of the numbers on file: that can only be dimmer than
        // either intent, never brighter than a value this host already
        // applied.
        ceiling_.settle(safeCeiling);
        gain_.settle(safeGain);
        lastAppliedCeiling_ = safeCeiling;
        lastAppliedGain_ = safeGain;
        return StateAdoption::kRejectedUnsupportedVersion;
    }

    if (!timestampIsPlausible(state.stateChangedAtMillis)) {
        // A corrupted or hostile persisted stateChangedAtMillis is the
        // same class of wedge as one arriving over MultiSync: adopting it
        // verbatim would make every future legitimate peer state compare
        // older forever. Settle darker and refuse to adopt the ordering
        // key, exactly as an unsupported schema version does.
        ceiling_.settle(safeCeiling);
        gain_.settle(safeGain);
        lastAppliedCeiling_ = safeCeiling;
        lastAppliedGain_ = safeGain;
        return StateAdoption::kRejectedImplausibleTimestamp;
    }

    revision_ = state.revision;
    stateChangedAtMillis_ = state.stateChangedAtMillis;
    instanceId_ = state.instanceId;
    // This is the node's own prior state, resumed after a restart, so the
    // stored ordering key's instanceId matches instanceId_ here, unlike
    // adoptState where the two deliberately diverge.
    orderingInstanceId_ = state.instanceId;
    orderingHash_ = orderingContentHash(state);
    lastAppliedCeiling_ = clampPercent(state.lastAppliedCeiling);
    lastAppliedGain_ = clampPercent(state.lastAppliedGain);

    const FadeWindowShape ceilingShape = classifyFadeWindow(state.ceilingFadeStartMillis, state.ceilingFadeEndMillis);
    if (ceilingShape == FadeWindowShape::kNone) {
        ceiling_.settle(clampPercent(state.ceilingTarget));
    } else if (ceilingShape == FadeWindowShape::kImplausible) {
        // An inverted or implausibly-magnituded window is never
        // trustworthy timing: settle darker rather than at the target,
        // which RES-018 section 1 requires.
        ceiling_.settle(safeCeiling);
    } else if (fadeTimingIsTrustworthy(state.ceilingFadeStartMillis, state.ceilingFadeEndMillis, state.persistedAtMillis, now)) {
        ceiling_.restore(clampPercent(state.ceilingStart), clampPercent(state.ceilingTarget), state.ceilingFadeStartMillis,
                         state.ceilingFadeEndMillis);
    } else {
        ceiling_.settle(safeCeiling);
    }

    const FadeWindowShape gainShape = classifyFadeWindow(state.gainFadeStartMillis, state.gainFadeEndMillis);
    if (gainShape == FadeWindowShape::kNone) {
        gain_.settle(clampPercent(state.gainTarget));
    } else if (gainShape == FadeWindowShape::kImplausible) {
        gain_.settle(safeGain);
    } else if (fadeTimingIsTrustworthy(state.gainFadeStartMillis, state.gainFadeEndMillis, state.persistedAtMillis, now)) {
        gain_.restore(clampPercent(state.gainStart), clampPercent(state.gainTarget), state.gainFadeStartMillis,
                      state.gainFadeEndMillis);
    } else {
        gain_.settle(safeGain);
    }

    return StateAdoption::kAdopted;
}

}  // namespace showmesh
