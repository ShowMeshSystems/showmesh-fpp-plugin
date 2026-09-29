#include "showmesh/brightness_query.h"

#include <string>

#include "check.h"

using showmesh::BrightnessEngine;
using showmesh::BrightnessQueryResponse;
using showmesh::renderBrightnessQuery;

namespace {
bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}
}  // namespace

TEST(TheQueryRouteReadsTheEnginesCurrentStateAndNeverRefuses) {
    BrightnessEngine engine;
    engine.setCeiling(60, 0, 1000);
    const BrightnessQueryResponse response = renderBrightnessQuery(engine, 1000);
    CHECK_EQ(response.status, 200);
    CHECK(contains(response.body, "\"schemaVersion\":1"));
    CHECK(contains(response.body, "\"ceiling\":60"));
    CHECK(contains(response.body, "\"transitionGain\":100"));
    CHECK(contains(response.body, "\"effectiveOutput\":60"));
    CHECK(contains(response.body, "\"fadeActive\":false"));
    CHECK(contains(response.body, "\"updatedAtMillis\":1000"));
}

TEST(TheQueryRouteReportsAnActiveFade) {
    BrightnessEngine engine;
    engine.setCeiling(80, 10, 1000);
    const BrightnessQueryResponse response = renderBrightnessQuery(engine, 5000);
    CHECK_EQ(response.status, 200);
    CHECK(contains(response.body, "\"fadeActive\":true"));
}

TEST(AClosedWeatherGateReportsZeroOutputWhateverTheCeilingIs) {
    BrightnessEngine engine;
    engine.setCeiling(90, 0, 1000);
    engine.setWeatherGate(true, 1, 1000);
    const BrightnessQueryResponse response = renderBrightnessQuery(engine, 1000);
    CHECK(contains(response.body, "\"ceiling\":90"));
    CHECK(contains(response.body, "\"effectiveOutput\":0"));
    CHECK(contains(response.body, "\"weatherGateClosed\":true"));
}

TEST(ACeilingWriteWhileTheWeatherGateIsClosedDoesNotRaiseOutput) {
    BrightnessEngine engine;
    engine.setCeiling(20, 0, 1000);
    engine.setWeatherGate(true, 1, 1000);
    engine.setCeiling(100, 0, 2000);
    CHECK_EQ(engine.effectivePercentAt(2000), 0);
    CHECK(engine.weatherGateClosed());
    const BrightnessQueryResponse response = renderBrightnessQuery(engine, 2000);
    CHECK(contains(response.body, "\"ceiling\":100"));
    CHECK(contains(response.body, "\"effectiveOutput\":0"));

    engine.setWeatherGate(false, 2, 3000);
    CHECK_EQ(engine.effectivePercentAt(3000), 100);
    CHECK(contains(renderBrightnessQuery(engine, 3000).body, "\"weatherGateClosed\":false"));
}
