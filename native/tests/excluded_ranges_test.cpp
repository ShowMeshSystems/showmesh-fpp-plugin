#include "showmesh/excluded_ranges.h"

#include <string>
#include <vector>

#include "check.h"
#include "showmesh/runtime.h"

using showmesh::ChannelRange;
using showmesh::ChannelRangeSources;
using showmesh::ExcludedRangesResponse;
using showmesh::ShowMeshRuntime;
using showmesh::TimeMillis;

namespace {

constexpr TimeMillis kT0 = 1'800'000'000'000;
constexpr std::uint32_t kChannels = 4096;
TimeMillis testClock() { return kT0; }

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class NoDefinitions : public showmesh::PlaylistDefinitionSource {
 public:
    std::string definitionFor(const std::string&) override { return std::string(); }
    std::string instanceUuid() override { return std::string(); }
    std::vector<std::string> playlistNames() override { return {}; }
};

// The runtime plus what FPP does around it: a settings store whose write
// calls the plugin's own listener, the way setSetting(key, value, true) does.
struct Host {
    NoDefinitions definitions;
    ShowMeshRuntime runtime{&definitions, nullptr, testClock};
    std::string excludeSetting;
    std::string applySetting;
    int stores = 0;
    bool storeWorks = true;
    std::vector<std::string> log;

    Host() { listener(); }

    void listener() {
        ChannelRangeSources sources;
        sources.applySetting = applySetting;
        sources.excludeSetting = excludeSetting;
        for (const std::string& line : runtime.configureChannelRanges(sources, kChannels)) log.push_back(line);
    }

    ExcludedRangesResponse put(const std::string& body) {
        return runtime.applyExcludedRanges(body, [this](const std::string& value) {
            if (!storeWorks) return false;
            ++stores;
            excludeSetting = value;
            listener();
            return true;
        });
    }

    std::vector<ChannelRange> excluded() { return runtime.brightness()->ranges().exclude; }
};

}  // namespace

TEST(TheExcludedRangesPathAndItsLanAddressAreDifferentStrings) {
    CHECK_EQ(std::string(showmesh::kExcludedRangesPath), std::string("/showmesh/brightness/excluded-ranges"));
    CHECK_EQ(std::string(showmesh::kExcludedRangesLanPath),
             std::string("/api/plugin-apis/showmesh/brightness/excluded-ranges"));
}

TEST(TheSettingListenerPathAppliesAChangedExcludeSettingLive) {
    Host host;
    CHECK(host.excluded().empty());
    host.excludeSetting = "513-512";
    host.listener();
    CHECK_EQ(showmesh::formatChannelRangesSetting(host.excluded()), std::string("513-512"));

    // A bad value is logged once and the good list stays in force.
    host.excludeSetting = "513-";
    host.listener();
    host.listener();
    CHECK_EQ(host.log.size(), std::size_t(1));
    CHECK_EQ(host.excluded().size(), std::size_t(1));
}

TEST(AnAppliedWriteStoresTheSettingAndReportsWhatTheEngineHolds) {
    Host host;
    const ExcludedRangesResponse r = host.put(
        R"({"schemaVersion":1,"excludedRanges":[{"startChannel":1,"channelCount":512},{"channelCount":512,"startChannel":1025}]})");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string(R"({"excludedRanges":[{"channelCount":512,"startChannel":1},)"
                                 R"({"channelCount":512,"startChannel":1025}],"schemaVersion":1})"));
    CHECK_EQ(host.excludeSetting, std::string("1-512,1025-512"));
    CHECK_EQ(host.stores, 1);
    CHECK_EQ(host.excluded().size(), std::size_t(2));

    CHECK(host.runtime.brightness()->setCeiling(30, 0, kT0).ok);
    std::vector<std::uint8_t> data(kChannels, 200);
    host.runtime.modifyChannelData(data.data(), data.size());
    CHECK_EQ(static_cast<int>(data[0]), 200);     // channel 1, excluded
    CHECK_EQ(static_cast<int>(data[512]), 60);    // channel 513, scaled
    CHECK_EQ(static_cast<int>(data[1024]), 200);  // channel 1025, excluded
}

TEST(AnEmptyListClearsTheExclusionsAndTheStoredSetting) {
    Host host;
    CHECK_EQ(host.put(R"({"excludedRanges":[{"startChannel":513,"channelCount":512}]})").status, 200);
    const ExcludedRangesResponse r = host.put(R"({"excludedRanges":[]})");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string(R"({"excludedRanges":[],"schemaVersion":1})"));
    CHECK(host.excludeSetting.empty());
    CHECK(host.excluded().empty());
}

TEST(ARefusedWriteAnswers400WithTheRightFormAndChangesNothing) {
    Host host;
    CHECK_EQ(host.put(R"({"excludedRanges":[{"startChannel":513,"channelCount":512}]})").status, 200);
    const char* const bodies[] = {
        "",
        "not json",
        "[]",
        R"({})",
        R"({"excludedRanges":"1-512"})",
        R"({"excludedRanges":[],"extra":1})",
        R"({"excludedRanges":[{"startChannel":1}]})",
        R"({"excludedRanges":[{"startChannel":1,"channelCount":512,"note":"x"}]})",
        R"({"excludedRanges":[{"startChannel":0,"channelCount":512}]})",
        R"({"excludedRanges":[{"startChannel":1,"channelCount":0}]})",
        R"({"excludedRanges":[{"startChannel":1.5,"channelCount":512}]})",
        R"({"excludedRanges":[{"startChannel":"1","channelCount":512}]})",
        R"({"excludedRanges":[{"startChannel":4294967294,"channelCount":2}]})",
        R"({"excludedRanges":[{"startChannel":1,"channelCount":512},{"startChannel":500,"channelCount":100}]})",
        R"({"excludedRanges":[{"startChannel":4000,"channelCount":512}]})",
    };
    for (const char* body : bodies) {
        const ExcludedRangesResponse r = host.put(body);
        CHECK_EQ(r.status, 400);
        CHECK(contains(r.body, R"("applied":false)"));
        CHECK(contains(r.body, R"(send {\"excludedRanges\":[{\"startChannel\":1,\"channelCount\":512}]})"));
        CHECK(!contains(r.body, "\n"));
    }
    CHECK_EQ(host.put(std::string(showmesh::kExcludedRangesBodyLimitBytes + 1, ' ')).status, 400);
    CHECK_EQ(host.stores, 1);
    CHECK_EQ(host.excludeSetting, std::string("513-512"));
    CHECK_EQ(host.excluded().size(), std::size_t(1));
}

TEST(AWriteThatCannotBeStoredAnswers500AndIsNotApplied) {
    Host host;
    host.storeWorks = false;
    const ExcludedRangesResponse r = host.put(R"({"excludedRanges":[{"startChannel":513,"channelCount":512}]})");
    CHECK_EQ(r.status, 500);
    CHECK(contains(r.body, "could not be saved"));
    CHECK(host.excluded().empty());
}

TEST(TheBrightnessQueryReportsTheExcludedRangesTheEngineHolds) {
    Host host;
    CHECK(contains(host.runtime.queryBrightness().body, R"("excludedRanges":[])"));
    CHECK_EQ(host.put(R"({"excludedRanges":[{"startChannel":513,"channelCount":512}]})").status, 200);
    CHECK(contains(host.runtime.queryBrightness().body,
                   R"("excludedRanges":[{"channelCount":512,"startChannel":513}])"));
}
