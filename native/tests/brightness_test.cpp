#include "showmesh/brightness.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "check.h"
#include "showmesh/brightness_codec.h"

using showmesh::BrightnessEngine;
using showmesh::BrightnessState;
using showmesh::ChannelRange;
using showmesh::RangeConfig;
using showmesh::StateAdoption;
using showmesh::TimeMillis;

namespace {

constexpr TimeMillis kT0 = 1'800'000'000'000;

std::vector<std::uint8_t> frame(std::size_t n, std::uint8_t value) { return std::vector<std::uint8_t>(n, value); }

}  // namespace

TEST(ZeroSecondsAppliesImmediately) {
    BrightnessEngine engine;
    CHECK(engine.setCeiling(75, 0, kT0).ok);
    CHECK_EQ(engine.effectivePercentAt(kT0), 75);
    CHECK(!engine.fadingAt(kT0));
}

TEST(FadeIsMonotonicAndReachesExactlyTheTargetAtTheDeadline) {
    BrightnessEngine engine;
    CHECK(engine.setCeiling(100, 0, kT0).ok);
    CHECK(engine.setCeiling(75, 10, kT0).ok);

    double previous = 101.0;
    for (int second = 0; second <= 10; ++second) {
        const double v = engine.ceilingAt(kT0 + second * 1000);
        CHECK(v <= previous);
        previous = v;
    }
    CHECK_NEAR(engine.ceilingAt(kT0 + 5000), 87.5, 1e-9);
    CHECK_NEAR(engine.ceilingAt(kT0 + 10000), 75.0, 1e-9);
    CHECK_EQ(engine.effectivePercentAt(kT0 + 10000), 75);
    // Past the deadline the value holds at the target rather than
    // continuing to extrapolate.
    CHECK_NEAR(engine.ceilingAt(kT0 + 60000), 75.0, 1e-9);
}

TEST(ReplacingAFadeStartsAtTheCurrentInterpolatedValue) {
    BrightnessEngine engine;
    CHECK(engine.setCeiling(100, 0, kT0).ok);
    CHECK(engine.setCeiling(0, 100, kT0).ok);

    const TimeMillis mid = kT0 + 50'000;
    const double atReplacement = engine.ceilingAt(mid);
    CHECK_NEAR(atReplacement, 50.0, 1e-9);

    CHECK(engine.setCeiling(80, 10, mid).ok);
    // No discontinuity: the instant the new fade starts, the value is
    // still exactly what it was.
    CHECK_NEAR(engine.ceilingAt(mid), atReplacement, 1e-9);
    CHECK_NEAR(engine.ceilingAt(mid + 5000), 65.0, 1e-9);
    CHECK_NEAR(engine.ceilingAt(mid + 10000), 80.0, 1e-9);
}

TEST(CeilingAndGainComposeAndAreIndependentlyWritable) {
    BrightnessEngine engine;
    CHECK(engine.setCeiling(60, 0, kT0).ok);
    CHECK(engine.setGain(50, 0, kT0).ok);
    CHECK_EQ(engine.effectivePercentAt(kT0), 30);

    CHECK(engine.setGain(100, 0, kT0).ok);
    CHECK_EQ(engine.effectivePercentAt(kT0), 60);
    CHECK_NEAR(engine.ceilingAt(kT0), 60.0, 1e-9);
}

// The decisive composition case: a ceiling change during a gain fade takes
// effect immediately, and a later gain of 100 reveals the current ceiling
// rather than a cached earlier one.
TEST(GainReturningToFullRevealsTheCurrentCeilingNotACachedOne) {
    BrightnessEngine engine;
    CHECK(engine.setCeiling(60, 0, kT0).ok);
    CHECK(engine.setGain(0, 20, kT0).ok);

    const TimeMillis mid = kT0 + 10'000;
    CHECK(engine.setCeiling(40, 10, mid).ok);

    const TimeMillis after = mid + 10'000;
    CHECK_NEAR(engine.ceilingAt(after), 40.0, 1e-9);

    CHECK(engine.setGain(100, 0, after).ok);
    CHECK_EQ(engine.effectivePercentAt(after), 40);
}

TEST(ConcurrentCeilingAndGainFadesComposeOnEveryFrame) {
    BrightnessEngine engine;
    CHECK(engine.setCeiling(100, 0, kT0).ok);
    CHECK(engine.setCeiling(50, 10, kT0).ok);
    CHECK(engine.setGain(50, 10, kT0).ok);

    // Halfway through both: ceiling 75, gain 75, composed 56.25 -> 56.
    CHECK_NEAR(engine.ceilingAt(kT0 + 5000), 75.0, 1e-9);
    CHECK_NEAR(engine.gainAt(kT0 + 5000), 75.0, 1e-9);
    CHECK_EQ(engine.effectivePercentAt(kT0 + 5000), 56);
    CHECK_EQ(engine.effectivePercentAt(kT0 + 10000), 25);
}

TEST(ActionInputOutsideTheDeclaredBoundsIsRejectedNotClamped) {
    BrightnessEngine engine;
    CHECK(!engine.setCeiling(-1, 0, kT0).ok);
    CHECK(!engine.setCeiling(101, 0, kT0).ok);
    CHECK(!engine.setCeiling(50, -1, kT0).ok);
    CHECK(!engine.setCeiling(50, 86401, kT0).ok);
    CHECK(engine.setCeiling(50, 86400, kT0).ok);
    // A rejected action changes nothing: the accepted day-long fade is
    // still the one running.
    CHECK(!engine.setCeiling(150, 0, kT0).ok);
    CHECK_NEAR(engine.ceilingAt(kT0 + 86400 * 1000), 50.0, 1e-9);
    CHECK_NEAR(engine.ceilingAt(kT0), 100.0, 1e-9);
}

TEST(OverlappingAndOutOfBoundsRangesAreRejected) {
    BrightnessEngine engine;
    RangeConfig overlapping;
    overlapping.apply.push_back(ChannelRange{1, 10});
    overlapping.apply.push_back(ChannelRange{5, 10});
    CHECK(!engine.configureRanges(overlapping, 64).ok);

    RangeConfig past;
    past.apply.push_back(ChannelRange{60, 10});
    CHECK(!engine.configureRanges(past, 64).ok);

    RangeConfig empty;
    empty.apply.push_back(ChannelRange{1, 0});
    CHECK(!engine.configureRanges(empty, 64).ok);

    RangeConfig valid;
    valid.apply.push_back(ChannelRange{1, 16});
    valid.apply.push_back(ChannelRange{17, 16});
    valid.exclude.push_back(ChannelRange{5, 4});
    CHECK(engine.configureRanges(valid, 64).ok);
}

TEST(ChannelsOutsideTheConfiguredRangesAreByteIdenticalAcrossAFade) {
    BrightnessEngine engine;
    RangeConfig config;
    config.apply.push_back(ChannelRange{1, 16});
    config.exclude.push_back(ChannelRange{5, 4});  // channels 5,6,7,8
    CHECK(engine.configureRanges(config, 32).ok);
    CHECK(engine.setCeiling(100, 0, kT0).ok);
    CHECK(engine.setCeiling(50, 10, kT0).ok);

    for (int second = 0; second <= 10; ++second) {
        std::vector<std::uint8_t> data = frame(32, 200);
        engine.applyToFrame(data.data(), data.size(), kT0 + second * 1000);
        for (std::size_t i = 4; i < 8; ++i) {
            CHECK_EQ(static_cast<int>(data[i]), 200);  // excluded
        }
        for (std::size_t i = 16; i < 32; ++i) {
            CHECK_EQ(static_cast<int>(data[i]), 200);  // outside every apply range
        }
    }

    std::vector<std::uint8_t> data = frame(32, 200);
    engine.applyToFrame(data.data(), data.size(), kT0 + 10'000);
    CHECK_EQ(static_cast<int>(data[0]), 100);
    CHECK_EQ(static_cast<int>(data[15]), 100);
}

TEST(AnEmptyApplyListScalesTheWholeUniverse) {
    BrightnessEngine engine;
    CHECK(engine.setCeiling(50, 0, kT0).ok);
    std::vector<std::uint8_t> data = frame(8, 200);
    engine.applyToFrame(data.data(), data.size(), kT0);
    for (std::uint8_t v : data) {
        CHECK_EQ(static_cast<int>(v), 100);
    }
}

TEST(StaleDuplicateAndUnsupportedFullStatePayloadsAreRejected) {
    BrightnessEngine engine;
    CHECK(engine.setCeiling(80, 0, kT0).ok);  // stateChangedAtMillis == kT0

    BrightnessState newer = engine.captureState(kT0);
    newer.stateChangedAtMillis = kT0 + 1000;
    newer.ceilingTarget = 40;
    newer.ceilingStart = 40;
    CHECK(engine.adoptState(newer) == StateAdoption::kAdopted);
    CHECK_NEAR(engine.ceilingAt(kT0), 40.0, 1e-9);

    // The same payload again carries nothing new.
    CHECK(engine.adoptState(newer) == StateAdoption::kRejectedStaleRevision);
    CHECK_NEAR(engine.ceilingAt(kT0), 40.0, 1e-9);

    BrightnessState older = newer;
    older.stateChangedAtMillis = kT0 + 500;
    older.ceilingTarget = 100;
    older.ceilingStart = 100;
    CHECK(engine.adoptState(older) == StateAdoption::kRejectedStaleRevision);
    CHECK_NEAR(engine.ceilingAt(kT0), 40.0, 1e-9);

    BrightnessState future = newer;
    future.stateChangedAtMillis = kT0 + 2000;
    future.schemaVersion = showmesh::kBrightnessStateSchemaVersion + 1;
    future.ceilingTarget = 100;
    future.ceilingStart = 100;
    CHECK(engine.adoptState(future) == StateAdoption::kRejectedUnsupportedVersion);
    CHECK_NEAR(engine.ceilingAt(kT0), 40.0, 1e-9);
}

// MultiSync orders full state by (stateChangedAtMillis, instanceId), never
// by the sender's local revision counter, so two nodes converge on the
// same state regardless of which one adopts first.
TEST(TwoEnginesWithEqualTimestampsConvergeRegardlessOfAdoptionOrder) {
    BrightnessEngine a;
    a.setInstanceId("node-a");
    CHECK(a.setCeiling(40, 0, kT0).ok);

    BrightnessEngine b;
    b.setInstanceId("node-b");
    CHECK(b.setCeiling(70, 0, kT0).ok);

    const BrightnessState stateA = a.captureState(kT0);
    const BrightnessState stateB = b.captureState(kT0);

    // a adopts b's state, b adopts a's state: both timestamps are equal,
    // so the tiebreak is instanceId, and "node-b" > "node-a" wins on both
    // sides regardless of adoption order.
    const StateAdoption aResult = a.adoptState(stateB);
    const StateAdoption bResult = b.adoptState(stateA);
    CHECK(aResult == StateAdoption::kAdopted);
    CHECK(bResult == StateAdoption::kRejectedStaleRevision);
    CHECK_NEAR(a.ceilingAt(kT0), 70.0, 1e-9);
    CHECK_NEAR(b.ceilingAt(kT0), 70.0, 1e-9);

    // Now adopt in the opposite order starting from two fresh engines:
    // the outcome does not depend on which engine goes first.
    BrightnessEngine c;
    c.setInstanceId("node-a");
    CHECK(c.setCeiling(40, 0, kT0).ok);
    BrightnessEngine d;
    d.setInstanceId("node-b");
    CHECK(d.setCeiling(70, 0, kT0).ok);
    const BrightnessState stateC = c.captureState(kT0);
    const BrightnessState stateD = d.captureState(kT0);
    CHECK(d.adoptState(stateC) == StateAdoption::kRejectedStaleRevision);
    CHECK(c.adoptState(stateD) == StateAdoption::kAdopted);
    CHECK_NEAR(c.ceilingAt(kT0), 70.0, 1e-9);
    CHECK_NEAR(d.ceilingAt(kT0), 70.0, 1e-9);
}

TEST(AGenuinelyOlderStateIsRefusedByTimestamp) {
    BrightnessEngine engine;
    engine.setInstanceId("node-a");
    CHECK(engine.setCeiling(50, 0, kT0 + 10'000).ok);

    BrightnessState olderFromOtherNode = engine.captureState(kT0 + 10'000);
    olderFromOtherNode.instanceId = "node-z";  // would win a tiebreak
    olderFromOtherNode.stateChangedAtMillis = kT0;  // but it is strictly older
    olderFromOtherNode.ceilingTarget = 5;
    olderFromOtherNode.ceilingStart = 5;

    CHECK(engine.adoptState(olderFromOtherNode) == StateAdoption::kRejectedStaleRevision);
    CHECK_NEAR(engine.ceilingAt(kT0 + 10'000), 50.0, 1e-9);
}

// finding 2: an adopted fade window with an implausible magnitude must
// never overflow the composition on a later frame.
TEST(AnImplausibleFadeWindowIsRejectedBeforeAdoption) {
    BrightnessEngine engine;
    BrightnessState state = engine.captureState(kT0);
    state.stateChangedAtMillis = kT0 + 1000;
    state.ceilingTarget = 0;
    state.ceilingFadeStartMillis = INT64_MIN;
    state.ceilingFadeEndMillis = INT64_MAX;
    CHECK(engine.adoptState(state) == StateAdoption::kRejectedInvalidFadeWindow);
    // Nothing was adopted: the ceiling is still the untouched default.
    CHECK_NEAR(engine.ceilingAt(kT0), 100.0, 1e-9);

    // An inverted-but-plausible-magnitude window is refused the same way.
    BrightnessState inverted = engine.captureState(kT0);
    inverted.stateChangedAtMillis = kT0 + 1000;
    inverted.ceilingFadeStartMillis = kT0 + 5000;
    inverted.ceilingFadeEndMillis = kT0;
    CHECK(engine.adoptState(inverted) == StateAdoption::kRejectedInvalidFadeWindow);

    // A window longer than the registered action's own 86400-second bound
    // is refused too.
    BrightnessState tooLong = engine.captureState(kT0);
    tooLong.stateChangedAtMillis = kT0 + 1000;
    tooLong.ceilingFadeStartMillis = kT0;
    tooLong.ceilingFadeEndMillis = kT0 + (showmesh::kMaxFadeSeconds + 1) * 1000;
    CHECK(engine.adoptState(tooLong) == StateAdoption::kRejectedInvalidFadeWindow);

    // A plausible window is still adopted normally.
    BrightnessState ok = engine.captureState(kT0);
    ok.stateChangedAtMillis = kT0 + 1000;
    ok.ceilingFadeStartMillis = kT0;
    ok.ceilingFadeEndMillis = kT0 + 10'000;
    ok.ceilingTarget = 10;
    CHECK(engine.adoptState(ok) == StateAdoption::kAdopted);
}

// finding 5: an inverted persisted window must settle at the darker
// value, never at the target, which is what RES-018 section 1 requires.
TEST(AnInvertedPersistedFadeWindowSettlesDarkerNotAtTheTarget) {
    BrightnessState persisted;
    persisted.revision = 1;
    persisted.ceilingTarget = 90;
    persisted.ceilingStart = 90;
    // Inverted: end before start.
    persisted.ceilingFadeStartMillis = kT0 + 10'000;
    persisted.ceilingFadeEndMillis = kT0;
    persisted.lastAppliedCeiling = 5;
    persisted.persistedAtMillis = kT0;

    BrightnessEngine engine;
    CHECK(engine.restoreFromPersisted(persisted, kT0 + 50'000) == StateAdoption::kAdopted);
    // The darker of lastAppliedCeiling (5) and target (90) is 5, never 90.
    CHECK_NEAR(engine.ceilingAt(kT0 + 50'000), 5.0, 1e-9);
    CHECK(!engine.fadingAt(kT0 + 50'000));
}

TEST(ALateJoinerConvergesOnTheCurrentFadePositionAndTarget) {
    BrightnessEngine leader;
    CHECK(leader.setCeiling(100, 0, kT0).ok);
    CHECK(leader.setCeiling(20, 100, kT0).ok);

    const TimeMillis mid = kT0 + 50'000;
    BrightnessState published = leader.captureState(mid);

    BrightnessEngine joiner;
    CHECK(joiner.adoptState(published) == StateAdoption::kAdopted);
    CHECK_NEAR(joiner.ceilingAt(mid), leader.ceilingAt(mid), 1e-9);
    CHECK_NEAR(joiner.ceilingAt(kT0 + 100'000), 20.0, 1e-9);
    // Adopting full state twice is indistinguishable from adopting it once.
    CHECK(joiner.adoptState(published) == StateAdoption::kRejectedStaleRevision);
    CHECK_NEAR(joiner.ceilingAt(kT0 + 100'000), 20.0, 1e-9);
}

TEST(RestartResumesATrustworthyFadeFromItsCurrentPosition) {
    BrightnessEngine before;
    CHECK(before.setCeiling(100, 0, kT0).ok);
    CHECK(before.setCeiling(20, 100, kT0).ok);
    std::vector<std::uint8_t> data = frame(4, 255);
    before.applyToFrame(data.data(), data.size(), kT0 + 10'000);
    const BrightnessState persisted = before.captureState(kT0 + 10'000);

    BrightnessEngine after;
    CHECK(after.restoreFromPersisted(persisted, kT0 + 50'000) == StateAdoption::kAdopted);
    CHECK_NEAR(after.ceilingAt(kT0 + 50'000), before.ceilingAt(kT0 + 50'000), 1e-9);
    CHECK_NEAR(after.ceilingAt(kT0 + 100'000), 20.0, 1e-9);
}

TEST(RestartWithUntrustworthyTimingChoosesTheDarkerValue) {
    BrightnessEngine before;
    CHECK(before.setCeiling(100, 0, kT0).ok);
    CHECK(before.setCeiling(20, 100, kT0).ok);
    std::vector<std::uint8_t> data = frame(4, 255);
    before.applyToFrame(data.data(), data.size(), kT0 + 10'000);
    const BrightnessState persisted = before.captureState(kT0 + 10'000);
    CHECK_NEAR(persisted.lastAppliedCeiling, 92.0, 1e-9);

    // A clock that moved backwards across the restart cannot place the
    // recorded window, so the darker of the last applied value and the
    // target is used rather than a resumed fade or a jump to full.
    BrightnessEngine after;
    CHECK(after.restoreFromPersisted(persisted, kT0 - 60'000) == StateAdoption::kAdopted);
    CHECK_NEAR(after.ceilingAt(kT0 - 60'000), 20.0, 1e-9);
    CHECK(!after.fadingAt(kT0 - 60'000));
}

TEST(RestartNeverFailsBrighterThanWhatWasApplied) {
    BrightnessState persisted;
    persisted.revision = 3;
    persisted.ceilingStart = 30;
    persisted.ceilingTarget = 90;   // was fading up
    persisted.ceilingFadeStartMillis = kT0;
    persisted.ceilingFadeEndMillis = kT0 + 100'000;
    persisted.lastAppliedCeiling = 30;
    persisted.persistedAtMillis = kT0 + 1'000;

    // Trustworthy timing resumes the fade at its real position, which is
    // still below the target.
    BrightnessEngine resumed;
    CHECK(resumed.restoreFromPersisted(persisted, kT0 + 50'000) == StateAdoption::kAdopted);
    CHECK_NEAR(resumed.ceilingAt(kT0 + 50'000), 60.0, 1e-9);

    // Untrustworthy timing settles at the darker of the two, never at 90.
    BrightnessEngine settled;
    CHECK(settled.restoreFromPersisted(persisted, kT0 - 1) == StateAdoption::kAdopted);
    CHECK_NEAR(settled.ceilingAt(kT0 - 1), 30.0, 1e-9);

    // An unreadable schema version is refused and still settles darker.
    BrightnessState unsupported = persisted;
    unsupported.schemaVersion = 99;
    BrightnessEngine refused;
    CHECK(refused.restoreFromPersisted(unsupported, kT0 + 50'000) == StateAdoption::kRejectedUnsupportedVersion);
    CHECK_NEAR(refused.ceilingAt(kT0 + 50'000), 30.0, 1e-9);
}

TEST(PersistedStateRoundTripsThroughItsEncoding) {
    BrightnessEngine engine;
    CHECK(engine.setCeiling(100, 0, kT0).ok);
    CHECK(engine.setCeiling(35, 120, kT0).ok);
    CHECK(engine.setGain(60, 30, kT0).ok);
    std::vector<std::uint8_t> data = frame(4, 255);
    engine.applyToFrame(data.data(), data.size(), kT0 + 5'000);

    const BrightnessState original = engine.captureState(kT0 + 5'000);
    const std::string encoded = showmesh::encodeBrightnessState(original);
    CHECK(!encoded.empty());

    showmesh::BrightnessStateDecode decoded = showmesh::decodeBrightnessState(encoded);
    CHECK(decoded.ok);
    CHECK_EQ(showmesh::encodeBrightnessState(decoded.state), encoded);

    BrightnessEngine restored;
    CHECK(restored.restoreFromPersisted(decoded.state, kT0 + 60'000) == StateAdoption::kAdopted);
    CHECK_NEAR(restored.ceilingAt(kT0 + 60'000), engine.ceilingAt(kT0 + 60'000), 1e-9);
    CHECK_NEAR(restored.gainAt(kT0 + 60'000), engine.gainAt(kT0 + 60'000), 1e-9);
}

TEST(APersistedPayloadMissingAFieldIsRejectedRatherThanDefaulted) {
    showmesh::BrightnessStateDecode decoded =
        showmesh::decodeBrightnessState("{\"schemaVersion\":1,\"revision\":2}");
    CHECK(!decoded.ok);
    CHECK(!decoded.error.empty());

    CHECK(!showmesh::decodeBrightnessState("not json").ok);
    CHECK(!showmesh::decodeBrightnessState("[1,2,3]").ok);
}

// finding 3: a representable but absurd revision (1e19) or an outright
// unrepresentable one (1e300) must fail the decode, not wedge the node by
// being adopted as newer-than-everything forever.
TEST(APersistedPayloadWithAnAbsurdRevisionIsRejected) {
    BrightnessEngine engine;
    const std::string encoded = showmesh::encodeBrightnessState(engine.captureState(kT0));
    const std::string needle = "\"revision\":0";
    const std::size_t pos = encoded.find(needle);
    CHECK(pos != std::string::npos);

    std::string tampered = encoded;
    tampered.replace(pos, needle.size(), "\"revision\":1e19");
    CHECK(!showmesh::decodeBrightnessState(tampered).ok);

    tampered = encoded;
    tampered.replace(pos, needle.size(), "\"revision\":1e300");
    CHECK(!showmesh::decodeBrightnessState(tampered).ok);
}

// finding 3: a field cast to TimeMillis must be range-checked against
// int64, not merely finite; 1e300 is finite but its cast is undefined
// behavior.
TEST(APersistedPayloadWithAnUnrepresentableTimeIsRejected) {
    BrightnessEngine engine;
    const std::string encoded = showmesh::encodeBrightnessState(engine.captureState(kT0));
    const std::string needle = "\"persistedAtMillis\":" + std::to_string(kT0);
    const std::size_t pos = encoded.find(needle);
    CHECK(pos != std::string::npos);

    std::string tampered = encoded;
    tampered.replace(pos, needle.size(), "\"persistedAtMillis\":1e300");
    CHECK(!showmesh::decodeBrightnessState(tampered).ok);
}

TEST(ExclusionsSplitApplyRangesCorrectlyAtEveryEdge) {
    struct Case {
        const char* name;
        RangeConfig config;
        std::vector<int> scaledChannels;  // one-based
    };

    RangeConfig wholeRangeExcluded;
    wholeRangeExcluded.apply.push_back(ChannelRange{1, 8});
    wholeRangeExcluded.exclude.push_back(ChannelRange{1, 8});

    RangeConfig leadingExclusion;
    leadingExclusion.apply.push_back(ChannelRange{1, 8});
    leadingExclusion.exclude.push_back(ChannelRange{1, 3});

    RangeConfig trailingExclusion;
    trailingExclusion.apply.push_back(ChannelRange{1, 8});
    trailingExclusion.exclude.push_back(ChannelRange{6, 3});

    RangeConfig twoHoles;
    twoHoles.apply.push_back(ChannelRange{1, 10});
    twoHoles.exclude.push_back(ChannelRange{3, 2});
    twoHoles.exclude.push_back(ChannelRange{7, 1});

    RangeConfig exclusionOutside;
    exclusionOutside.apply.push_back(ChannelRange{5, 4});
    exclusionOutside.exclude.push_back(ChannelRange{20, 4});

    const std::vector<Case> cases = {
        {"the exclusion covers the whole range", wholeRangeExcluded, {}},
        {"the exclusion is at the range start", leadingExclusion, {4, 5, 6, 7, 8}},
        {"the exclusion is at the range end", trailingExclusion, {1, 2, 3, 4, 5}},
        {"two exclusions punch two holes", twoHoles, {1, 2, 5, 6, 8, 9, 10}},
        {"the exclusion misses the range entirely", exclusionOutside, {5, 6, 7, 8}},
    };

    for (const Case& c : cases) {
        BrightnessEngine engine;
        CHECK(engine.configureRanges(c.config, 32).ok);
        CHECK(engine.setCeiling(50, 0, kT0).ok);
        std::vector<std::uint8_t> data = frame(32, 200);
        engine.applyToFrame(data.data(), data.size(), kT0);

        for (std::size_t i = 0; i < data.size(); ++i) {
            const int oneBased = static_cast<int>(i) + 1;
            const bool expectScaled =
                std::find(c.scaledChannels.begin(), c.scaledChannels.end(), oneBased) != c.scaledChannels.end();
            const int want = expectScaled ? 100 : 200;
            if (static_cast<int>(data[i]) != want) {
                ::showmesh_test::reportFailure(__FILE__, __LINE__,
                                               std::string(c.name) + ": channel " + std::to_string(oneBased) +
                                                   " = " + std::to_string(static_cast<int>(data[i])) + ", want " +
                                                   std::to_string(want));
            }
        }
    }
}

// FPP hands the plugin its entire channel buffer, which is megabytes. Only
// the configured channels may be read or written, or the per-frame cost is
// set by the buffer rather than by the configuration.
TEST(OnlyConfiguredChannelsAreTouchedInAFullSizeBuffer) {
    constexpr std::size_t kBufferChannels = 8192 * 1024;
    BrightnessEngine engine;
    RangeConfig config;
    config.apply.push_back(ChannelRange{1025, 16});
    CHECK(engine.configureRanges(config, static_cast<std::uint32_t>(kBufferChannels)).ok);
    CHECK(engine.setCeiling(50, 0, kT0).ok);

    std::vector<std::uint8_t> data(kBufferChannels, 200);
    engine.applyToFrame(data.data(), data.size(), kT0);

    for (std::size_t i = 1024; i < 1040; ++i) {
        CHECK_EQ(static_cast<int>(data[i]), 100);
    }
    CHECK_EQ(static_cast<int>(data[1023]), 200);
    CHECK_EQ(static_cast<int>(data[1040]), 200);
    CHECK_EQ(static_cast<int>(data[0]), 200);
    CHECK_EQ(static_cast<int>(data[kBufferChannels - 1]), 200);
}

// A configured range that runs past the frame the adapter actually handed
// over must clip, not read past the end.
TEST(ARangeBeyondTheFrameIsClipped) {
    BrightnessEngine engine;
    RangeConfig config;
    config.apply.push_back(ChannelRange{1, 64});
    CHECK(engine.configureRanges(config, 1024).ok);
    CHECK(engine.setCeiling(50, 0, kT0).ok);

    std::vector<std::uint8_t> data = frame(8, 200);
    engine.applyToFrame(data.data(), data.size(), kT0);
    for (std::uint8_t v : data) {
        CHECK_EQ(static_cast<int>(v), 100);
    }
}
