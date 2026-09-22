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
