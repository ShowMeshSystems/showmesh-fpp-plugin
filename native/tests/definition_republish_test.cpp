#include "showmesh/definition_republish.h"

#include <atomic>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "showmesh/coordinator_client.h"
#include "showmesh/http_transport.h"
#include "showmesh/runtime.h"

using showmesh::applyDefinitionRepublishRequest;
using showmesh::CoordinatorClient;
using showmesh::CredentialSource;
using showmesh::DefinitionHoldings;
using showmesh::DefinitionPublisher;
using showmesh::DefinitionRepublishResponse;
using showmesh::HttpRequest;
using showmesh::HttpResponse;
using showmesh::HttpTransport;
using showmesh::PlaylistDefinitionSource;
using showmesh::RetryPolicy;
using showmesh::ShowMeshRuntime;
using showmesh::SweepRecord;
using showmesh::TimeMillis;

namespace {

TimeMillis gNow = 1'800'000'000'000;
TimeMillis testClock() { return gNow; }

const char* kUuid = "6f1c1a52-1b6c-4b53-9a0e-9f7c2f0d1b44";
const char* kDefinition = "{\"name\":\"Main Show\",\"mainPlaylist\":[{\"sequenceName\":\"a.fseq\"}]}";
const char* kBaseUrl = "http://coordinator.invalid:8080";

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string body(const std::string& id) {
    return R"({"schemaVersion":1,"requestId":")" + id + R"("})";
}

class FakePublisher : public DefinitionPublisher {
 public:
    bool publishDefinition(const std::string&, const std::string&, const std::string&, const std::string&,
                           TimeMillis) override {
        return true;
    }

    DefinitionHoldings clearHeldDefinitions() override {
        DefinitionHoldings out;
        out.cleared = holdings.held;
        holdings.held = 0;
        out.held = 0;
        out.refusedTerminally = holdings.refusedTerminally;
        ++clears;
        return out;
    }

    DefinitionHoldings definitionHoldings() const override { return holdings; }

    DefinitionHoldings holdings;
    int clears = 0;
};

class FakeSweepRecord : public SweepRecord {
 public:
    void requestSweep() override {
        ++requests;
        pending = true;
    }
    bool sweepPending() const override { return pending; }

    int requests = 0;
    bool pending = false;
};

// The three pieces the route needs, so no test shares an idempotency key
// with another.
struct Route {
    FakePublisher publisher;
    FakeSweepRecord sweep;
    std::string lastRequestId;

    DefinitionRepublishResponse post(const std::string& text) {
        return applyDefinitionRepublishRequest(text, &publisher, &sweep, &lastRequestId);
    }
};

class FakeDefinitions : public PlaylistDefinitionSource {
 public:
    std::string definitionFor(const std::string&) override { return kDefinition; }
    std::string instanceUuid() override { return kUuid; }
    std::vector<std::string> playlistNames() override { return {"Main Show"}; }
};

class FakeTransport : public HttpTransport {
 public:
    HttpResponse post(const HttpRequest& request) override {
        requests.push_back(request);
        HttpResponse response;
        response.transportOk = true;
        response.statusCode = statusCode;
        return response;
    }

    // Not exercised by this file's own tests, which only drive publish
    // paths; shares post()'s canned response so the interface's second
    // verb has a stated behavior rather than none.
    HttpResponse get(const HttpRequest& request) override { return post(request); }

    std::vector<HttpRequest> requests;
    int statusCode = 200;
};

class FakeCredentials : public CredentialSource {
 public:
    bool token(std::string* out, std::string*) override {
        *out = "a-bearer-token-that-must-never-be-logged";
        return true;
    }
    void invalidate() override {}
};

void noSleep(int, const std::atomic<bool>*) {}

std::string readFileOrFail(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ::showmesh_test::reportFailure(__FILE__, __LINE__, std::string("could not open ") + path);
        return std::string();
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) ++count;
    return count;
}

}  // namespace

TEST(TheRegisteredRepublishPathAndTheAddressAreDifferentStrings) {
    // The registered path is what each major's own web server holds. The
    // LAN path is what a coordinator posts to, and it carries the
    // plugin-apis prefix FPP's Apache requires, because both majors bind
    // their HTTP server to loopback only.
    CHECK_EQ(std::string(showmesh::kDefinitionRepublishPath), std::string("/showmesh/playlists/republish"));
    CHECK_EQ(std::string(showmesh::kDefinitionRepublishLanPath),
             std::string("/api/plugin-apis/showmesh/playlists/republish"));
    CHECK(contains(showmesh::kDefinitionRepublishLanPath, showmesh::kDefinitionRepublishPath));

    // Pinned to the prefix FPP actually proxies, not merely to each other:
    //
    //   FPP 9  RewriteRule ^plugin-apis/(.*)$ http://localhost:32322/$1 [P]
    //   FPP 10 RewriteRule ^plugin-apis/(.*)$ http://localhost:32322/$1 [P]
    //
    // Without this, an edit that changed both constants consistently to a
    // prefix FPP does not proxy would satisfy every other assertion here
    // and still 404 on every real caller.
    CHECK_EQ(std::string(showmesh::kDefinitionRepublishLanPath).rfind("/api/plugin-apis/", 0), std::size_t(0));

    // A registered path beginning with /api produces a working but visibly
    // wrong address with /api in it twice.
    CHECK(std::string(showmesh::kDefinitionRepublishPath).rfind("/api", 0) != 0);
}

TEST(ARepublishClearsTheHeldSetRecordsASweepAndReportsWhatItDropped) {
    Route route;
    route.publisher.holdings.held = 6;
    route.publisher.holdings.refusedTerminally = 1;

    DefinitionRepublishResponse r = route.post(body("req-1"));

    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("applied":true)"));
    CHECK(contains(r.body, R"("definitionsCleared":6)"));
    CHECK(contains(r.body, R"("definitionsHeld":0)"));
    CHECK(contains(r.body, R"("definitionsRefusedTerminally":1)"));
    CHECK(contains(r.body, R"("sweepPending":true)"));
    CHECK_EQ(route.publisher.clears, 1);
    CHECK_EQ(route.sweep.requests, 1);
}

TEST(ARepublishLeavesTerminallyRefusedDefinitionsIntact) {
    // Section 3.9 item 3 is a prohibition: a terminal refusal cannot
    // change until the plugin restarts, so clearing that set alongside the
    // held one would spend the retry budget on a condition that will not
    // improve. The count must survive the clear and be reported.
    Route route;
    route.publisher.holdings.held = 2;
    route.publisher.holdings.refusedTerminally = 3;

    DefinitionRepublishResponse r = route.post(body("req-1"));

    CHECK_EQ(r.status, 200);
    CHECK_EQ(route.publisher.holdings.refusedTerminally, std::size_t(3));
    CHECK(contains(r.body, R"("definitionsRefusedTerminally":3)"));
}

TEST(ARepeatOfTheSameRequestIdAppliesNothingAndReportsTheStateAsItStands) {
    Route route;
    route.publisher.holdings.held = 4;
    route.post(body("req-1"));

    // The worker has re-sent two definitions and had them accepted since
    // the republish, and the sweep it owed has completed.
    route.publisher.holdings.held = 2;
    route.sweep.pending = false;

    DefinitionRepublishResponse r = route.post(body("req-1"));

    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("applied":false)"));
    CHECK(contains(r.body, R"("definitionsCleared":0)"));
    // A repeat reports progress rather than an echo: this is how a caller
    // polling the same id learns the sweep finished.
    CHECK(contains(r.body, R"("definitionsHeld":2)"));
    CHECK(contains(r.body, R"("sweepPending":false)"));
    CHECK_EQ(route.publisher.clears, 1);
    CHECK_EQ(route.sweep.requests, 1);
}

TEST(ADifferentRequestIdAfterARepeatAppliesAgain) {
    Route route;
    route.publisher.holdings.held = 1;
    route.post(body("req-1"));
    route.post(body("req-1"));
    route.publisher.holdings.held = 1;

    DefinitionRepublishResponse r = route.post(body("req-2"));

    CHECK(contains(r.body, R"("applied":true)"));
    CHECK_EQ(route.publisher.clears, 2);
}

TEST(ARepublishRefusesEveryMalformedBodyTheContractLists) {
    struct Case {
        const char* body;
        const char* expectedError;
    };
    const Case cases[] = {
        {"not json at all", "not valid JSON"},
        {R"([1,2,3])", "must be a JSON object"},
        {R"({"requestId":"req-1"})", "schemaVersion is required"},
        {R"({"schemaVersion":"1","requestId":"req-1"})", "schemaVersion must be a number"},
        {R"({"schemaVersion":1.5,"requestId":"req-1"})", "schemaVersion must be a whole number"},
        {R"({"schemaVersion":2,"requestId":"req-1"})", "unsupported schemaVersion 2"},
        {R"({"schemaVersion":1})", "requestId is required"},
        {R"({"schemaVersion":1,"requestId":7})", "requestId is required and must be a string"},
        {R"({"schemaVersion":1,"requestId":""})", "requestId must not be empty"},
    };
    for (const Case& c : cases) {
        Route route;
        DefinitionRepublishResponse r = route.post(c.body);
        CHECK_EQ(r.status, 400);
        CHECK(contains(r.body, R"("applied":false)"));
        CHECK(contains(r.body, c.expectedError));
        // A refusal changes nothing: neither the held set nor the sweep.
        CHECK_EQ(route.publisher.clears, 0);
        CHECK_EQ(route.sweep.requests, 0);
    }
}

TEST(ARepublishRefusesABodyOverTheBoundWithoutParsingIt) {
    Route route;
    // Valid JSON that would otherwise apply, padded past the bound with
    // whitespace, so the refusal can only be the size check.
    std::string oversize = body("req-1");
    oversize.append(showmesh::kDefinitionRepublishBodyLimitBytes, ' ');

    DefinitionRepublishResponse r = route.post(oversize);

    CHECK_EQ(r.status, 400);
    CHECK(contains(r.body, "larger than this route accepts"));
    CHECK_EQ(route.publisher.clears, 0);
    CHECK_EQ(route.sweep.requests, 0);
}

TEST(ARepublishRefusesWhenThePluginPublishesNoDefinitions) {
    std::string lastRequestId;
    FakeSweepRecord sweep;
    DefinitionRepublishResponse r = applyDefinitionRepublishRequest(body("req-1"), nullptr, &sweep, &lastRequestId);

    CHECK_EQ(r.status, 400);
    CHECK(contains(r.body, "not configured to publish playlist definitions"));
    CHECK_EQ(sweep.requests, 0);
}

TEST(ADefinitionTheCoordinatorLostIsSentAgainThoughItsHashDidNotChange) {
    // The case that cannot self-heal. The coordinator loses a definition
    // it once accepted; the plugin still holds that hash, the
    // content-addressed skip fires on every later sweep because the bytes
    // have not changed, and without this route only a plugin restart
    // clears it.
    FakeDefinitions definitions;
    FakeTransport transport;
    FakeCredentials credentials;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, noSleep, RetryPolicy());
    ShowMeshRuntime runtime(&definitions, nullptr, testClock, nullptr, &client);

    CHECK(runtime.sweepDefinitions());
    CHECK_EQ(transport.requests.size(), std::size_t(1));
    const std::string firstPost = transport.requests[0].body;

    // The control: without a republish the definition is never re-sent,
    // however many sweeps run, because its hash has not changed.
    CHECK(runtime.sweepDefinitions());
    CHECK_EQ(runtime.maybeSweepDefinitions(), false);
    CHECK_EQ(transport.requests.size(), std::size_t(1));

    const DefinitionRepublishResponse r = runtime.applyDefinitionRepublish(body("req-1"));
    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, R"("definitionsCleared":1)"));

    // The owed sweep runs on the worker's next pass even though the
    // re-scan interval has not elapsed, and it re-sends the identical
    // definition under the identical hash.
    CHECK(runtime.maybeSweepDefinitions());
    CHECK_EQ(transport.requests.size(), std::size_t(2));
    if (transport.requests.size() > 1) CHECK_EQ(transport.requests[1].body, firstPost);

    // The record is cleared by the sweep it caused, so a caller polling
    // the same request id learns the sweep finished.
    const DefinitionRepublishResponse repeat = runtime.applyDefinitionRepublish(body("req-1"));
    CHECK(contains(repeat.body, R"("applied":false)"));
    CHECK(contains(repeat.body, R"("sweepPending":false)"));
}

TEST(ARepublishDoesNotResendADefinitionTheCoordinatorRefusedTerminally) {
    FakeDefinitions definitions;
    FakeTransport transport;
    FakeCredentials credentials;
    // A 400 is the coordinator judging these exact bytes unacceptable, so
    // the client caches the refusal; re-sending them gets the same answer.
    transport.statusCode = 400;
    CoordinatorClient client(&transport, &credentials, kBaseUrl, testClock, nullptr, noSleep, RetryPolicy());
    ShowMeshRuntime runtime(&definitions, nullptr, testClock, nullptr, &client);

    CHECK(runtime.sweepDefinitions());
    CHECK_EQ(transport.requests.size(), std::size_t(1));

    const DefinitionRepublishResponse r = runtime.applyDefinitionRepublish(body("req-1"));
    CHECK(contains(r.body, R"("definitionsRefusedTerminally":1)"));

    CHECK(runtime.maybeSweepDefinitions());
    CHECK_EQ(transport.requests.size(), std::size_t(1));
}

TEST(BothAdaptersRegisterAndWithdrawBothRoutes) {
    // Neither adapter compiles without an FPP source tree, so this reads
    // their source. A route registered and never withdrawn fails only at
    // fppd shutdown on a real player: on FPP 9 libhttpserver keeps a bare
    // pointer to a destroyed member, and on FPP 10 a handler left
    // registered pins the .so and defeats FPP_PLUGIN_SUPPORTS_UNLOAD().
    const char* kAdapters[] = {"adapters/fpp9/plugin.cpp", "adapters/fpp10/plugin.cpp"};
    const char* kConstants[] = {"kTransitionGainPath", "kDefinitionRepublishPath"};
    for (const char* adapter : kAdapters) {
        const std::string source = readFileOrFail(adapter);
        for (const char* constant : kConstants) {
            const std::string qualified = std::string("showmesh::") + constant;
            const std::size_t withdrawals = countOccurrences(source, "unregister_resource(" + qualified + ")") +
                                            countOccurrences(source, "unregisterPluginApi(" + qualified + ")");
            // Two mentions and one withdrawal: the route is registered
            // once and withdrawn once. A third mention, or a missing
            // withdrawal, is the defect this test exists for.
            const std::size_t mentions = countOccurrences(source, qualified);
            if (withdrawals != 1 || mentions != 2) {
                ::showmesh_test::reportFailure(__FILE__, __LINE__,
                                               std::string(adapter) + " mentions " + qualified + " " +
                                                   std::to_string(mentions) + " time(s) and withdraws it " +
                                                   std::to_string(withdrawals) + " time(s), want 2 and 1");
            }
        }
    }
}
