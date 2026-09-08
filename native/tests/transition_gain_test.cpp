#include "showmesh/transition_gain.h"

#include <cstddef>
#include <string>

#include "check.h"
#include "showmesh/brightness.h"

using showmesh::applyTransitionGainRequest;
using showmesh::BrightnessEngine;
using showmesh::TimeMillis;
using showmesh::TransitionGainResponse;

namespace {

constexpr TimeMillis kT0 = 1'800'000'000'000;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string body(const std::string& target, const std::string& fade, const std::string& id) {
    return R"({"schemaVersion":1,"targetPercent":)" + target + R"(,"fadeSeconds":)" + fade + R"(,"requestId":")" + id +
           R"("})";
}

// A fixture carrying the two pieces the route needs and nothing else, so a
// test cannot accidentally share the idempotency key between cases.
struct Route {
    BrightnessEngine engine;
    std::string lastRequestId;

    TransitionGainResponse post(const std::string& text, TimeMillis now = kT0) {
        return applyTransitionGainRequest(text, &engine, &lastRequestId, now);
    }
};

}  // namespace

TEST(TheRegisteredPathAndTheAddressAreDifferentStrings) {
    // The registered path is what each major's own web server holds. The
    // LAN path is what a coordinator posts to, and it carries the
    // plugin-apis prefix FPP's Apache requires, because both majors bind
    // their HTTP server to loopback only. Asserting both, and asserting
    // that the second contains the first, is what stops a future edit
    // moving one without the other.
    CHECK_EQ(std::string(showmesh::kTransitionGainPath), std::string("/showmesh/brightness/transition-gain"));
    CHECK_EQ(std::string(showmesh::kTransitionGainLanPath),
             std::string("/api/plugin-apis/showmesh/brightness/transition-gain"));
    CHECK(contains(showmesh::kTransitionGainLanPath, showmesh::kTransitionGainPath));

    // Pinned to the prefix FPP actually proxies, not merely to each
    // other. Both majors carry the same single rule and nothing else
    // under /api reaches the plugin's own server:
    //
    //   FPP 9  RewriteRule ^plugin-apis/(.*)$ http://localhost:32322/$1 [P]
    //   FPP 10 RewriteRule ^plugin-apis/(.*)$ http://localhost:32322/$1 [P]
    //
    // Without this, an edit that changed both constants consistently to a
    // prefix FPP does not proxy would satisfy every other assertion here
    // and still 404 on every real caller.
    CHECK_EQ(std::string(showmesh::kTransitionGainLanPath).rfind("/api/plugin-apis/", 0), std::size_t(0));

    // A registered path beginning with /api produces a working but
    // visibly wrong address with /api in it twice, which is the defect
    // this pair exists to prevent.
    CHECK(std::string(showmesh::kTransitionGainPath).rfind("/api", 0) != 0);
}

TEST(AGainWriteAppliesAndReportsTheComposedState) {
    Route route;
    TransitionGainResponse r = route.post(body("75", "0", "req-1"));

    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("applied":true)"));
    CHECK(contains(r.body, R"("gainStart":100)"));
    CHECK(contains(r.body, R"("gainTarget":75)"));
    CHECK(contains(r.body, R"("fadeSeconds":0)"));
    // Section 2.2 requires the applied state rather than an HTTP 200, so
    // the caller has evidence. With a ceiling of 100 the composition is
    // the gain itself.
    CHECK(contains(r.body, R"("ceiling":100)"));
    CHECK(contains(r.body, R"("effectiveOutput":75)"));
    CHECK_EQ(route.engine.effectivePercentAt(kT0), 75);
}

TEST(TheGainComposesWithTheCeilingRatherThanReplacingIt) {
    // RES-018 section 1: effective output = round(ceiling * gain / 100).
    // The ceiling is FPP's, the gain is the coordinator's, and this is the
    // whole reason the gain is a separate value instead of a brightness
    // write.
    Route route;
    CHECK(route.engine.setCeiling(60, 0, kT0).ok);
    TransitionGainResponse r = route.post(body("75", "0", "req-1"));

    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("ceiling":60)"));
    CHECK(contains(r.body, R"("effectiveOutput":45)"));
    CHECK_EQ(route.engine.effectivePercentAt(kT0), 45);
}

TEST(OutOfRangeIsRefusedNeverClamped) {
    // Section 2.2: "Out of range is rejected, never clamped", so a
    // mistyped value is visible instead of silently rounded into range.
    Route route;
    for (const char* target : {"101", "-1"}) {
        TransitionGainResponse r = route.post(body(target, "0", "req-range"));
        CHECK_EQ(r.status, 400);
        CHECK(contains(r.body, R"("applied":false)"));
    }
    for (const char* fade : {"-1", "86401"}) {
        TransitionGainResponse r = route.post(body("50", fade, "req-fade"));
        CHECK_EQ(r.status, 400);
    }
    // Nothing was applied by any of those refusals.
    CHECK_EQ(route.engine.effectivePercentAt(kT0), 100);
}

TEST(AWholeNumberIsRequiredSoAMistypedValueIsVisible) {
    Route route;
    for (const char* target : {"75.5", R"("75")", "true", "null"}) {
        TransitionGainResponse r =
            route.post(std::string(R"({"schemaVersion":1,"targetPercent":)") + target +
                       R"(,"fadeSeconds":0,"requestId":"req-1"})");
        CHECK_EQ(r.status, 400);
    }
    CHECK_EQ(route.engine.effectivePercentAt(kT0), 100);
}

TEST(ExponentNotationIsAcceptedBecauseItIsTheSameInteger) {
    // 1e2 IS 100 in JSON: the grammar has one number type and no integer
    // spelling of its own. Refusing it would need the literal preserved
    // rather than the value, and would refuse a caller that is not wrong.
    // The check above rejects a value that is not a whole number, which is
    // what section 2.2 actually requires.
    Route route;
    TransitionGainResponse r =
        route.post(R"({"schemaVersion":1,"targetPercent":1e2,"fadeSeconds":0,"requestId":"req-1"})");
    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("gainTarget":100)"));
}

TEST(ARepeatedRequestIdAppliesNothing) {
    // The caller-minted idempotency key from section 2.2. A caller that
    // retries a response it never saw must not restart the fade from
    // wherever it got to.
    Route route;
    CHECK_EQ(route.post(body("50", "10", "req-1")).status, 200);

    const TimeMillis midFade = kT0 + 5000;
    const int midway = route.engine.effectivePercentAt(midFade);
    TransitionGainResponse repeat = route.post(body("50", "10", "req-1"), midFade);

    CHECK_EQ(repeat.status, 200);
    CHECK(contains(repeat.body, R"("applied":false)"));
    // The fade kept running rather than restarting: the value at the same
    // instant is what it was before the repeat arrived.
    CHECK_EQ(route.engine.effectivePercentAt(midFade), midway);
}

TEST(ADifferentRequestIdDuringAFadeIsAppliedNotIgnored) {
    // Section 2.3's mid-fade case. Idempotency is per id, not a lock: a
    // genuinely new instruction during a fade must land, or the night
    // controller cannot correct itself.
    Route route;
    CHECK_EQ(route.post(body("0", "60", "req-1")).status, 200);

    const TimeMillis midFade = kT0 + 30000;
    TransitionGainResponse second = route.post(body("100", "0", "req-2"), midFade);

    CHECK_EQ(second.status, 200);
    CHECK(contains(second.body, R"("applied":true)"));
    CHECK_EQ(route.engine.effectivePercentAt(midFade), 100);
}

TEST(AGainOf100RevealsTheCurrentCeilingNotACachedOne) {
    // Section 2.3's fourth forbidden case, stated positively: the gain
    // never caches a ceiling, so restoring it cannot restore an old one.
    Route route;
    CHECK(route.engine.setCeiling(80, 0, kT0).ok);
    CHECK_EQ(route.post(body("50", "0", "req-1")).status, 200);
    CHECK_EQ(route.engine.effectivePercentAt(kT0), 40);

    // The ceiling moves while the gain is down, the way FPP's own
    // scheduler would move it.
    CHECK(route.engine.setCeiling(30, 0, kT0).ok);
    TransitionGainResponse restore = route.post(body("100", "0", "req-2"), kT0);

    CHECK_EQ(restore.status, 200);
    CHECK(contains(restore.body, R"("ceiling":30)"));
    CHECK(contains(restore.body, R"("effectiveOutput":30)"));
}

TEST(EveryMalformedBodyShapeIsRefusedWithJson) {
    Route route;
    const char* bodies[] = {
        "",
        "not json",
        "[1,2]",
        R"({"targetPercent":50,"fadeSeconds":0,"requestId":"a"})",
        R"({"schemaVersion":2,"targetPercent":50,"fadeSeconds":0,"requestId":"a"})",
        R"({"schemaVersion":1,"fadeSeconds":0,"requestId":"a"})",
        R"({"schemaVersion":1,"targetPercent":50,"requestId":"a"})",
        R"({"schemaVersion":1,"targetPercent":50,"fadeSeconds":0})",
        R"({"schemaVersion":1,"targetPercent":50,"fadeSeconds":0,"requestId":""})",
        R"({"schemaVersion":1,"targetPercent":50,"fadeSeconds":0,"requestId":7})",
    };
    for (const char* text : bodies) {
        TransitionGainResponse r = route.post(text);
        CHECK_EQ(r.status, 400);
        // Always JSON, never an empty body: a caller parsing the response
        // must not have to special-case a refusal.
        CHECK(contains(r.body, R"("applied":false)"));
        CHECK(contains(r.body, R"("schemaVersion":1)"));
    }
    CHECK_EQ(route.engine.effectivePercentAt(kT0), 100);
}

TEST(AnOversizedBodyIsRefusedBeforeItIsParsed) {
    // The route is unauthenticated (section 2.2's accepted posture), so
    // any host on the show LAN can post to it. An unbounded read on an FPP
    // host during a show is a risk four small fields never justify.
    Route route;
    std::string huge(showmesh::kTransitionGainBodyLimitBytes + 1, 'x');
    TransitionGainResponse r = route.post(huge);
    CHECK_EQ(r.status, 400);
    CHECK(contains(r.body, "larger than this route accepts"));
}

TEST(ANullEngineIsRefusedRatherThanDereferenced) {
    // The handler runs on fppd's web thread and can in principle arrive
    // before or after the runtime it writes to. A refusal is the only
    // acceptable answer; a crash here takes fppd with it.
    std::string lastRequestId;
    TransitionGainResponse r = applyTransitionGainRequest(body("50", "0", "req-1"), nullptr, &lastRequestId, kT0);
    CHECK_EQ(r.status, 400);
}
