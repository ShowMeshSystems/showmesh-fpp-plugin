#include "showmesh/brightness.h"

#include <algorithm>
#include <cmath>

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
    bumpRevision();
    return ValidationResult{};
}

ValidationResult BrightnessEngine::setGain(int targetPercent, std::int64_t fadeSeconds, TimeMillis now) {
    ValidationResult result = validateActionInput(targetPercent, fadeSeconds);
    if (!result.ok) return result;
    gain_.fadeTo(static_cast<double>(targetPercent), fadeSeconds * 1000, now);
    bumpRevision();
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

StateAdoption BrightnessEngine::adoptState(const BrightnessState& state) {
    if (state.schemaVersion != kBrightnessStateSchemaVersion) {
        return StateAdoption::kRejectedUnsupportedVersion;
    }
    // Equal revisions are rejected too: a duplicated or delayed payload
    // carries no newer information, and re-adopting it could only move
    // this node backwards to the sender's older sample of the same state.
    if (state.revision <= revision_) {
        return StateAdoption::kRejectedStaleRevision;
    }
    ceiling_.restore(clampPercent(state.ceilingStart), clampPercent(state.ceilingTarget), state.ceilingFadeStartMillis,
                     state.ceilingFadeEndMillis);
    gain_.restore(clampPercent(state.gainStart), clampPercent(state.gainTarget), state.gainFadeStartMillis,
                  state.gainFadeEndMillis);
    revision_ = state.revision;
    return StateAdoption::kAdopted;
}

namespace {

// A persisted fade is placeable only if its window is coherent and the
// current time is not before the window began. A clock that moved
// backwards across the restart makes the recorded window meaningless.
bool fadeTimingIsTrustworthy(TimeMillis startMillis, TimeMillis endMillis, TimeMillis persistedAt, TimeMillis now) {
    if (endMillis <= startMillis) return false;  // no fade recorded, or an inverted window
    if (now < startMillis) return false;
    if (persistedAt != 0 && now < persistedAt) return false;
    return true;
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

    revision_ = state.revision;
    lastAppliedCeiling_ = clampPercent(state.lastAppliedCeiling);
    lastAppliedGain_ = clampPercent(state.lastAppliedGain);

    if (state.ceilingFadeEndMillis <= state.ceilingFadeStartMillis) {
        ceiling_.settle(clampPercent(state.ceilingTarget));
    } else if (fadeTimingIsTrustworthy(state.ceilingFadeStartMillis, state.ceilingFadeEndMillis, state.persistedAtMillis, now)) {
        ceiling_.restore(clampPercent(state.ceilingStart), clampPercent(state.ceilingTarget), state.ceilingFadeStartMillis,
                         state.ceilingFadeEndMillis);
    } else {
        ceiling_.settle(safeCeiling);
    }

    if (state.gainFadeEndMillis <= state.gainFadeStartMillis) {
        gain_.settle(clampPercent(state.gainTarget));
    } else if (fadeTimingIsTrustworthy(state.gainFadeStartMillis, state.gainFadeEndMillis, state.persistedAtMillis, now)) {
        gain_.restore(clampPercent(state.gainStart), clampPercent(state.gainTarget), state.gainFadeStartMillis,
                      state.gainFadeEndMillis);
    } else {
        gain_.settle(safeGain);
    }

    return StateAdoption::kAdopted;
}

}  // namespace showmesh
