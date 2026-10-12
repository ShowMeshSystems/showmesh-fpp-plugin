#include "showmesh/channel_ranges_setting.h"

#include <cstdint>
#include <string>
#include <vector>

#include "check.h"

using showmesh::ChannelRange;
using showmesh::parseChannelRangesSetting;

namespace {

bool sameRanges(const std::vector<ChannelRange>& got, const std::vector<ChannelRange>& want) {
    if (got.size() != want.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].startChannel != want[i].startChannel || got[i].channelCount != want[i].channelCount) return false;
    }
    return true;
}

}  // namespace

// finding 1: the original parser passed substr() temporaries to strtoul and
// dereferenced the saved endptr after those temporaries were destroyed. At
// -O2 this made "1-10" parse to zero ranges. This is the load-bearing
// regression test: reverting the fix (going back to unnamed
// token.substr(...).c_str() arguments) makes this fail under ASan with
// stack-use-after-scope, and can silently produce an empty result at -O2.
TEST(AWellFormedSingleRangeParses) {
    const std::vector<ChannelRange> got = parseChannelRangesSetting("1-10");
    CHECK(sameRanges(got, {ChannelRange{1, 10}}));
}

TEST(MultipleCommaSeparatedRangesAllParse) {
    const std::vector<ChannelRange> got = parseChannelRangesSetting("1-10,50-5,100-1");
    CHECK(sameRanges(got, {ChannelRange{1, 10}, ChannelRange{50, 5}, ChannelRange{100, 1}}));
}

TEST(EmptyInputParsesToNoRanges) {
    CHECK(parseChannelRangesSetting("").empty());
}

TEST(MalformedTokensAreSkippedNotCrashed) {
    for (const char* bad : {"abc", "1-", "-10", "1-2-3", "1", ",", "--1-10"}) {
        const std::vector<ChannelRange> got = parseChannelRangesSetting(bad);
        if (!got.empty()) {
            ::showmesh_test::reportFailure(__FILE__, __LINE__, std::string("expected no ranges from \"") + bad + "\"");
        }
    }
}

// A stray leading or trailing comma produces one empty, malformed token
// that is skipped; the well-formed token elsewhere in the list still
// parses, and skipping never crashes on the empty substring.
TEST(AStrayCommaSkipsOnlyItsOwnEmptyToken) {
    CHECK(sameRanges(parseChannelRangesSetting("1-10,"), {ChannelRange{1, 10}}));
    CHECK(sameRanges(parseChannelRangesSetting(",1-10"), {ChannelRange{1, 10}}));
}

TEST(AZeroStartOrZeroCountIsRejected) {
    CHECK(parseChannelRangesSetting("0-10").empty());
    CHECK(parseChannelRangesSetting("1-0").empty());
}

// Whitespace between digits within a value is rejected: strtoull stops at
// the space, leaving the endptr short of the token's end. Whitespace
// immediately after the dash is leading whitespace of the count field,
// which strtoull tolerates the same way it tolerates a leading '+'; that
// is intentional, not a gap.
TEST(InternalAndTrailingWhitespaceIsRejected) {
    CHECK(parseChannelRangesSetting("1 -10").empty());
    CHECK(parseChannelRangesSetting("1-10 ").empty());
    CHECK(parseChannelRangesSetting("1-1 0").empty());
}

TEST(ALeadingPlusSignIsAccepted) {
    const std::vector<ChannelRange> got = parseChannelRangesSetting("+1-10");
    CHECK(sameRanges(got, {ChannelRange{1, 10}}));
}

// A leading '-' on the start makes the token before the range dash empty
// ("-5-10" splits at the first '-' into "" and "5-10"), so strtoull sees an
// empty string and the token is rejected rather than parsed as a negative
// start.
TEST(ANegativeLookingStartIsRejected) {
    CHECK(parseChannelRangesSetting("-5-10").empty());
}

// Ranges are one-based and independent of each other: overlap and ordering
// are the engine's concern (validateRanges), not the parser's. The parser
// only rejects what cannot be represented at all.
TEST(OverlappingAndOutOfOrderRangesStillParseIndividually) {
    const std::vector<ChannelRange> got = parseChannelRangesSetting("50-10,1-100");
    CHECK(sameRanges(got, {ChannelRange{50, 10}, ChannelRange{1, 100}}));
}

TEST(ValuesAboveUint32MaxAreRejected) {
    CHECK(parseChannelRangesSetting("4294967296-1").empty());   // start > UINT32_MAX
    CHECK(parseChannelRangesSetting("1-4294967296").empty());   // count > UINT32_MAX
    CHECK(parseChannelRangesSetting("18446744073709551615-1").empty());  // does not even fit unsigned long long headroom check
}

// finding 1: 4294967295 (UINT32_MAX) is itself a valid uint32_t value, so a
// check that only rejects start/count individually above UINT32_MAX is not
// enough: start + count must also fit, or ChannelRange::endExclusive()
// wraps. 4294967295-10 must never produce a range at all.
TEST(AStartAndCountThatWouldOverflowEndExclusiveIsRejected) {
    CHECK(parseChannelRangesSetting("4294967295-10").empty());
    // Right at the boundary: start + count == UINT32_MAX is representable.
    const std::vector<ChannelRange> boundary = parseChannelRangesSetting("4294967290-5");
    CHECK(sameRanges(boundary, {ChannelRange{4294967290u, 5u}}));
}

TEST(WholeUint32MaxAsAStartIsAcceptedWhenCountIsOne) {
    // 4294967295 + 1 == 4294967296, one past UINT32_MAX, which is the
    // wrap case from the note above stated with count 1 instead of 10.
    CHECK(parseChannelRangesSetting("4294967295-1").empty());
    const std::vector<ChannelRange> got = parseChannelRangesSetting("4294967294-1");
    CHECK(sameRanges(got, {ChannelRange{4294967294u, 1u}}));
}

TEST(TheStrictFormFailsTheWholeValueOnOneBadToken) {
    const showmesh::ChannelRangesParse good = showmesh::parseChannelRangesSettingStrict("1-512, 1025-512");
    CHECK(good.ok);
    CHECK(sameRanges(good.ranges, {ChannelRange{1, 512}, ChannelRange{1025, 512}}));

    for (const char* bad : {"1-512,abc", "1-512,", "1-0", "0-5", "5", "1-512;1025-512", "+1-5", "4294967295-1"}) {
        const showmesh::ChannelRangesParse parsed = showmesh::parseChannelRangesSettingStrict(bad);
        CHECK(!parsed.ok);
        CHECK(parsed.ranges.empty());
        CHECK(parsed.error.find("start-count pair such as 1-512") != std::string::npos);
    }
}

TEST(AnEmptyStrictValueIsValidAndHoldsNoRanges) {
    CHECK(showmesh::parseChannelRangesSettingStrict("").ok);
    CHECK(showmesh::parseChannelRangesSettingStrict("  ").ranges.empty());
    CHECK(showmesh::parseChannelRangesSettingStrict("  ").ok);
}

TEST(FormattedRangesParseBackToTheSameRanges) {
    const std::vector<ChannelRange> ranges = {ChannelRange{1, 512}, ChannelRange{1025, 512}};
    const std::string text = showmesh::formatChannelRangesSetting(ranges);
    CHECK_EQ(text, std::string("1-512,1025-512"));
    CHECK(sameRanges(showmesh::parseChannelRangesSettingStrict(text).ranges, ranges));
    CHECK_EQ(showmesh::formatChannelRangesSetting({}), std::string());
}

namespace {

// Scales a frame of 200s at a ceiling of 30 and reports one channel, one-based.
int scaledChannel(showmesh::BrightnessEngine* engine, std::size_t channel) {
    std::vector<std::uint8_t> data(4096, 200);
    engine->applyToFrame(data.data(), data.size(), 1'800'000'000'000);
    return data[channel - 1];
}

}  // namespace

TEST(TheExcludeSettingAppliesToEveryApplyListSource) {
    showmesh::ChannelRangeSources fromSetting;
    fromSetting.applySetting = "1-2048";
    showmesh::ChannelRangeSources fromOutputs;
    fromOutputs.outputRanges = {ChannelRange{1, 2048}};
    const showmesh::ChannelRangeSources fromFullBuffer;

    for (showmesh::ChannelRangeSources sources : {fromSetting, fromOutputs, fromFullBuffer}) {
        sources.excludeSetting = "513-512";
        showmesh::BrightnessEngine engine;
        std::string lastRejected;
        CHECK(engine.setCeiling(30, 0, 1'800'000'000'000).ok);
        CHECK(showmesh::configureChannelRangesFrom(sources, 4096, &engine, &lastRejected).empty());
        CHECK(sameRanges(engine.ranges().exclude, {ChannelRange{513, 512}}));
        CHECK_EQ(scaledChannel(&engine, 512), 60);
        CHECK_EQ(scaledChannel(&engine, 513), 200);
        CHECK_EQ(scaledChannel(&engine, 1024), 200);
        CHECK_EQ(scaledChannel(&engine, 1025), 60);
    }
}

TEST(ARejectedExcludeSettingKeepsThePreviousExclusionsAndIsLoggedOnce) {
    showmesh::BrightnessEngine engine;
    std::string lastRejected;
    showmesh::ChannelRangeSources sources;
    sources.excludeSetting = "513-512";
    CHECK(showmesh::configureChannelRangesFrom(sources, 4096, &engine, &lastRejected).empty());

    // Unparseable, overlapping, and past the buffer: each keeps 513-512.
    for (const char* bad : {"513-512,oops", "1-100,50-100", "4000-512"}) {
        sources.excludeSetting = bad;
        const std::vector<std::string> first = showmesh::configureChannelRangesFrom(sources, 4096, &engine, &lastRejected);
        CHECK_EQ(first.size(), std::size_t(1));
        CHECK(first[0].find("ShowMeshExcludeChannelRanges setting rejected: ") == 0);
        CHECK(first[0].find("keeping the previous excluded channels") != std::string::npos);
        CHECK(showmesh::configureChannelRangesFrom(sources, 4096, &engine, &lastRejected).empty());
        CHECK(sameRanges(engine.ranges().exclude, {ChannelRange{513, 512}}));
    }

    // An empty value is the operator clearing the list, not a failure.
    sources.excludeSetting = "";
    CHECK(showmesh::configureChannelRangesFrom(sources, 4096, &engine, &lastRejected).empty());
    CHECK(engine.ranges().exclude.empty());
}

TEST(ARejectedApplySettingStillFallsBackToOutputRangesWithTheExclusions) {
    showmesh::BrightnessEngine engine;
    std::string lastRejected;
    showmesh::ChannelRangeSources sources;
    sources.applySetting = "1-100,50-100";
    sources.excludeSetting = "10-5";
    sources.outputRanges = {ChannelRange{1, 64}};
    const std::vector<std::string> log = showmesh::configureChannelRangesFrom(sources, 4096, &engine, &lastRejected);
    CHECK_EQ(log.size(), std::size_t(1));
    CHECK(log[0].find("ShowMeshChannelRanges setting rejected: ") == 0);
    CHECK(sameRanges(engine.ranges().apply, {ChannelRange{1, 64}}));
    CHECK(sameRanges(engine.ranges().exclude, {ChannelRange{10, 5}}));
}

TEST(AClosedWeatherGateStillDarkensAnExcludedChannel) {
    showmesh::BrightnessEngine engine;
    std::string lastRejected;
    showmesh::ChannelRangeSources sources;
    sources.excludeSetting = "513-512";
    CHECK(showmesh::configureChannelRangesFrom(sources, 4096, &engine, &lastRejected).empty());
    CHECK_EQ(scaledChannel(&engine, 600), 200);
    engine.setWeatherGate(true, 1, 1'800'000'000'000);
    CHECK_EQ(scaledChannel(&engine, 600), 0);
    CHECK_EQ(scaledChannel(&engine, 1), 0);
}
