// The two properties that only hold when the runtime and the real
// coordinator client are wired together: the FPP callback thread stays
// out of the network and retry path entirely, and gap evidence produced
// by queue pressure reaches the wire instead of being silently absorbed.

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "showmesh/coordinator_client.h"
#include "showmesh/runtime.h"

using showmesh::CoordinatorClient;
using showmesh::HttpRequest;
using showmesh::HttpResponse;
using showmesh::HttpTransport;
using showmesh::PlaylistDefinitionSource;
using showmesh::RetryPolicy;
using showmesh::ShowMeshRuntime;
using showmesh::TimeMillis;

namespace {

TimeMillis gNow = 1'800'000'000'000;
TimeMillis testClock() { return gNow; }

std::mutex gThreadMutex;
std::vector<std::thread::id> gWorkThreads;

void noteWorkThread() {
    std::lock_guard<std::mutex> guard(gThreadMutex);
    gWorkThreads.push_back(std::this_thread::get_id());
}

// A real sleep, not a recorded one: the point of the test below is that
// the callback returns promptly while something else is genuinely
// blocked in backoff.
void sleepingSleeper(int millis) {
    noteWorkThread();
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

bool anyWorkRanOn(std::thread::id id) {
    std::lock_guard<std::mutex> guard(gThreadMutex);
    for (const std::thread::id& seen : gWorkThreads) {
        if (seen == id) return true;
    }
    return false;
}

std::size_t workCount() {
    std::lock_guard<std::mutex> guard(gThreadMutex);
    return gWorkThreads.size();
}

void resetWork() {
    std::lock_guard<std::mutex> guard(gThreadMutex);
    gWorkThreads.clear();
}

class ThreadNotingTransport : public HttpTransport {
 public:
    HttpResponse post(const HttpRequest& request) override {
        noteWorkThread();
        std::lock_guard<std::mutex> guard(mutex_);
        bodies_.push_back(request.body);
        HttpResponse response;
        response.transportOk = reachable;
        response.statusCode = reachable ? 200 : 0;
        if (!reachable) response.error = "connection refused";
        return response;
    }

    std::vector<std::string> bodies() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return bodies_;
    }

    bool reachable = false;

 private:
    mutable std::mutex mutex_;
    std::vector<std::string> bodies_;
};

class StubCredentials : public showmesh::CredentialSource {
 public:
    bool token(std::string* out, std::string*) override {
        *out = "a-token";
        return true;
    }
    void invalidate() override {}
};

class StubDefinitions : public PlaylistDefinitionSource {
 public:
    std::string definitionFor(const std::string&) override {
        return "{\"name\":\"Main Show\",\"mainPlaylist\":[{\"sequenceName\":\"a.fseq\"}]}";
    }
    std::string instanceUuid() override { return "6f1c1a52-1b6c-4b53-9a0e-9f7c2f0d1b44"; }
};

RetryPolicy shortPolicy() {
    RetryPolicy policy;
    policy.maxAttempts = 3;
    policy.initialBackoffMillis = 40;
    policy.maxBackoffMillis = 80;
    return policy;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(TheCallbackThreadNeverPerformsNetworkOrRetryWork) {
    resetWork();
    ThreadNotingTransport transport;  // unreachable, so every post spends its full backoff
    StubCredentials credentials;
    StubDefinitions definitions;
    CoordinatorClient client(&transport, &credentials, "http://coordinator.invalid", testClock, nullptr,
                             sleepingSleeper, shortPolicy());
    ShowMeshRuntime runtime(&definitions, &client, testClock, nullptr, &client);
    runtime.start();

    const std::thread::id callbackThread = std::this_thread::get_id();
    const auto began = std::chrono::steady_clock::now();
    for (int i = 0; i < 8; ++i) {
        runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "a.fseq", "");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count();

    // Eight callbacks against an unreachable coordinator whose per-post
    // backoff alone exceeds this: the callback thread copies bounded
    // evidence into the handoff and returns.
    CHECK(elapsed < 200);

    // Let the worker get far enough to have actually attempted delivery.
    for (int i = 0; i < 200 && workCount() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    runtime.stop();

    CHECK(workCount() > 0);
    CHECK(!anyWorkRanOn(callbackThread));
    CHECK(client.status().transportFailures > 0);
}

TEST(QueuePressureCoalescesToTheNewestStateAndTheGapReachesTheWire) {
    resetWork();
    ThreadNotingTransport transport;
    transport.reachable = true;
    StubCredentials credentials;
    StubDefinitions definitions;
    CoordinatorClient client(&transport, &credentials, "http://coordinator.invalid", testClock, nullptr,
                             sleepingSleeper, shortPolicy());
    // No worker: the callbacks all land before anything drains, which is
    // exactly the pressure case, made deterministic.
    ShowMeshRuntime runtime(&definitions, &client, testClock, nullptr, &client);

    const int offered = 24;
    for (int i = 0; i < offered; ++i) {
        runtime.observeCallback("Main Show", "playing", "mainPlaylist", i, "a.fseq", "");
    }
    CHECK_EQ(runtime.handoff().capacity(), std::size_t{16});
    CHECK(runtime.drainOnce());

    const std::vector<std::string> bodies = transport.bodies();
    bool sawObservation = false;
    for (const std::string& body : bodies) {
        if (!contains(body, "\"coalescedSincePreviousAcknowledged\"")) continue;
        sawObservation = true;
        // 24 offered into a 16 slot handoff drops the 8 oldest, and the
        // count travels so the coordinator never reads a coalesced
        // delivery as a complete event history.
        CHECK(contains(body, "\"coalescedSincePreviousAcknowledged\":8"));
        // The newest complete state survives, not the oldest.
        CHECK(contains(body, "\"position\":8"));
    }
    CHECK(sawObservation);
    CHECK_EQ(client.status().coalescedAcknowledged, std::uint64_t{8});
}
