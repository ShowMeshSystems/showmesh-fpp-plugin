#include "showmesh/weather_gate.h"

#include <string>

#include "check.h"
#include "showmesh/brightness.h"

using showmesh::applyWeatherGateRequest;
using showmesh::BrightnessEngine;
using showmesh::renderWeatherGateState;
using showmesh::TimeMillis;
using showmesh::WeatherGateResponse;

namespace {

constexpr TimeMillis kT0 = 1'800'000'000'000;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

struct Route {
    BrightnessEngine engine;

    WeatherGateResponse post(const std::string& text, TimeMillis now = kT0) {
        return applyWeatherGateRequest(text, &engine, now);
    }
    WeatherGateResponse get(TimeMillis now = kT0) { return renderWeatherGateState(&engine, now); }
};

}  // namespace

TEST(TheRegisteredPathAndTheAddressAreDifferentStrings) {
    CHECK_EQ(std::string(showmesh::kWeatherGatePath), std::string("/showmesh/brightness/weather-gate"));
    CHECK_EQ(std::string(showmesh::kWeatherGateLanPath),
             std::string("/api/plugin-apis/showmesh/brightness/weather-gate"));
    CHECK(contains(showmesh::kWeatherGateLanPath, showmesh::kWeatherGatePath));
    CHECK_EQ(std::string(showmesh::kWeatherGateLanPath).rfind("/api/plugin-apis/", 0), std::size_t(0));
    CHECK(std::string(showmesh::kWeatherGatePath).rfind("/api", 0) != 0);
}

TEST(AClosingWriteAppliesImmediatelyAndReportsTheComposedState) {
    Route route;
    CHECK(route.engine.setCeiling(60, 0, kT0).ok);
    WeatherGateResponse r = route.post(R"({"closed":true,"revision":1})");

    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("applied":true)"));
    CHECK(contains(r.body, R"("weatherGateClosed":true)"));
    CHECK(contains(r.body, R"("effectiveOutputPercent":0)"));
    CHECK(contains(r.body, R"("effectiveOutput":0)"));
    CHECK(contains(r.body, R"("ceiling":60)"));
    CHECK_EQ(route.engine.effectivePercentAt(kT0), 0);
}

TEST(AnOpeningWriteRevealsTheCurrentComposedValue) {
    Route route;
    CHECK(route.engine.setCeiling(60, 0, kT0).ok);
    CHECK_EQ(route.post(R"({"closed":true,"revision":1})").status, 200);
    WeatherGateResponse r = route.post(R"({"closed":false,"revision":2})");

    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("weatherGateClosed":false)"));
    CHECK(contains(r.body, R"("effectiveOutputPercent":60)"));
    CHECK_EQ(route.engine.effectivePercentAt(kT0), 60);
}

TEST(AGetReadsWithoutChangingAnything) {
    Route route;
    CHECK(route.engine.setCeiling(40, 0, kT0).ok);
    CHECK_EQ(route.post(R"({"closed":true,"revision":1})").status, 200);

    WeatherGateResponse r = route.get();
    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("applied":false)"));
    CHECK(contains(r.body, R"("weatherGateClosed":true)"));
    CHECK(contains(r.body, R"("effectiveOutputPercent":0)"));
    // Still closed: a read never writes.
    CHECK(route.engine.weatherGateClosed());
}

TEST(AMissingClosedKeyIsRefused) {
    Route route;
    WeatherGateResponse r = route.post(R"({})");
    CHECK_EQ(r.status, 400);
    CHECK(contains(r.body, R"("applied":false)"));
    CHECK(!route.engine.weatherGateClosed());
}

TEST(AnUnknownKeyIsRefusedEvenAlongsideAValidOne) {
    Route route;
    WeatherGateResponse r = route.post(R"({"closed":true,"revision":1,"reason":"storm"})");
    CHECK_EQ(r.status, 400);
    CHECK(!route.engine.weatherGateClosed());
}

TEST(ANonBooleanClosedValueIsRefusedNeverClampedOrGuessed) {
    Route route;
    for (const char* value : {R"("true")", "1", "null", "0"}) {
        WeatherGateResponse r = route.post(std::string(R"({"closed":)") + value + R"(,"revision":1})");
        CHECK_EQ(r.status, 400);
    }
    CHECK(!route.engine.weatherGateClosed());
}

TEST(EveryMalformedBodyShapeIsRefusedWithJson) {
    Route route;
    const char* bodies[] = {
        "",
        "not json",
        "[1,2]",
        R"({"closed":true,"revision":1,"unknown":1})",
    };
    for (const char* text : bodies) {
        WeatherGateResponse r = route.post(text);
        CHECK_EQ(r.status, 400);
        CHECK(contains(r.body, R"("applied":false)"));
        CHECK(contains(r.body, R"("schemaVersion":1)"));
    }
    CHECK(!route.engine.weatherGateClosed());
}

TEST(AnOversizedBodyIsRefusedBeforeItIsParsed) {
    Route route;
    std::string huge(showmesh::kWeatherGateBodyLimitBytes + 1, 'x');
    WeatherGateResponse r = route.post(huge);
    CHECK_EQ(r.status, 400);
    CHECK(contains(r.body, "larger than this route accepts"));
}

TEST(ANullEngineIsRefusedRatherThanDereferenced) {
    WeatherGateResponse r = applyWeatherGateRequest(R"({"closed":true,"revision":1})", nullptr, kT0);
    CHECK_EQ(r.status, 400);
    WeatherGateResponse g = renderWeatherGateState(nullptr, kT0);
    CHECK_EQ(g.status, 400);
}

TEST(TheResponseCarriesTheStoredGateRevision) {
    Route route;
    WeatherGateResponse r = route.post(R"({"closed":true,"revision":12})");
    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("weatherGateRevision":12)"));
    // A lower revision still applies, and the stored revision still climbs.
    r = route.post(R"({"closed":false,"revision":4})");
    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("weatherGateRevision":13)"));
    CHECK(contains(route.get().body, R"("weatherGateRevision":13)"));
}

TEST(ABadOrMissingRevisionIsRefusedWithoutChangingState) {
    Route route;
    const char* bodies[] = {
        R"({"closed":true})",
        R"({"closed":true,"revision":-1})",
        R"({"closed":true,"revision":1.5})",
        R"({"closed":true,"revision":9007199254740992})",
        R"({"closed":true,"revision":"1"})",
        R"({"closed":true,"revision":null})",
        R"({"closed":true,"rev":1})",
    };
    for (const char* text : bodies) {
        WeatherGateResponse r = route.post(text);
        CHECK_EQ(r.status, 400);
    }
    CHECK(!route.engine.weatherGateClosed());
    CHECK_EQ(route.engine.weatherGateRevision(), std::uint64_t{0});
    CHECK_EQ(route.post(R"({"closed":true,"revision":9007199254740991})").status, 200);
}
