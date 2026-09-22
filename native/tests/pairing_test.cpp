#include "showmesh/pairing.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.h"

using showmesh::CoordinatorUrlSource;
using showmesh::deriveCrockfordPairingCode;
using showmesh::HttpRequest;
using showmesh::HttpResponse;
using showmesh::HttpTransport;
using showmesh::PairingState;
using showmesh::PairingStatus;
using showmesh::PairingWorker;
using showmesh::TimeMillis;

namespace {

TimeMillis gNow = 1000;
TimeMillis testClock() { return gNow; }

class TempDir {
 public:
    explicit TempDir(const char* prefix) {
        std::string tmpl = std::string("/tmp/") + prefix + "-XXXXXX";
        std::vector<char> buffer(tmpl.begin(), tmpl.end());
        buffer.push_back('\0');
        const char* made = ::mkdtemp(buffer.data());
        CHECK(made != nullptr);
        path_ = made != nullptr ? std::string(made) : std::string();
    }
    ~TempDir() {
        // Best effort; a leftover temp dir does not fail the suite.
        ::system(("rm -rf " + path_).c_str());
    }
    const std::string& path() const { return path_; }

    void write(const std::string& name, const std::string& contents) const {
        std::ofstream out(path_ + "/" + name, std::ios::binary | std::ios::trunc);
        out << contents;
    }

    bool exists(const std::string& name) const {
        struct ::stat info {};
        return ::stat((path_ + "/" + name).c_str(), &info) == 0;
    }

    std::string read(const std::string& name) const {
        std::ifstream in(path_ + "/" + name, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    ::mode_t modeOf(const std::string& name) const {
        struct ::stat info {};
        if (::stat((path_ + "/" + name).c_str(), &info) != 0) return 0;
        return info.st_mode & 07777;
    }

 private:
    std::string path_;
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A fixed byte sequence in place of /dev/urandom, so a test drives an
// exact, repeatable secret.
bool fixedBytes(std::uint8_t* out, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<std::uint8_t>(i);
    return true;
}

class FakeUrlSource : public CoordinatorUrlSource {
 public:
    std::string url;
    std::string currentBaseUrl() const override { return url; }
};

class FakeTransport : public HttpTransport {
 public:
    std::vector<HttpResponse> responses;
    std::vector<HttpRequest> requests;

    HttpResponse post(const HttpRequest& request) override {
        requests.push_back(request);
        if (responses.empty()) {
            HttpResponse r;
            r.transportOk = false;
            r.error = "no scripted response";
            return r;
        }
        HttpResponse r = responses.front();
        if (responses.size() > 1) responses.erase(responses.begin());
        return r;
    }
    HttpResponse get(const HttpRequest& request) override { return post(request); }

    static HttpResponse notFound() {
        HttpResponse r;
        r.transportOk = true;
        r.statusCode = 404;
        return r;
    }
    static HttpResponse paired(std::string token, std::string principalId) {
        HttpResponse r;
        r.transportOk = true;
        r.statusCode = 200;
        r.body = "{\"token\":\"" + token + "\",\"principalId\":\"" + principalId + "\",\"instanceId\":\"i-1\"}";
        return r;
    }
    static HttpResponse unparseable200() {
        HttpResponse r;
        r.transportOk = true;
        r.statusCode = 200;
        r.body = "not json";
        return r;
    }
};

}  // namespace

TEST(DeriveCrockfordPairingCodeMatchesAFixedSecretAndItsIndependentlyComputedCode) {
    // secret = bytes 0x00..0x1f; SHA-256(secret) computed independently
    // (Python's hashlib) starts 63 0d cd 29 66 ..., whose first 40 bits
    // Crockford-encode to CC6W-TAB6. A change to either the hashing or the
    // encoding here must reproduce this exact value or the coordinator's
    // own derivation of the same secret disagrees with this plugin.
    const std::string secretHex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    CHECK_EQ(deriveCrockfordPairingCode(secretHex), std::string("CC6W-TAB6"));
}

TEST(DeriveCrockfordPairingCodeRefusesAMalformedSecret) {
    CHECK(deriveCrockfordPairingCode("").empty());
    CHECK(deriveCrockfordPairingCode("too-short").empty());
    CHECK(deriveCrockfordPairingCode(std::string(64, 'g')).empty());  // not hex
    CHECK(deriveCrockfordPairingCode(std::string(63, '0')).empty());  // 63, not 64
}

TEST(APairingRequestFileStartsWaitingAndWritesCodeAndStatus) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    state.write("pairing-request", "{\"requestedAtMillis\":1000}");
    worker.tick(1000);

    CHECK(!state.exists("pairing-request"));
    CHECK(state.exists("pairing-code"));
    CHECK(state.exists("pairing-status.json"));
    const PairingStatus status = worker.status();
    CHECK(status.state == PairingState::kWaiting);
    CHECK(!status.code.empty());
    CHECK(contains(state.read("pairing-status.json"), "\"state\":\"waiting\""));
    CHECK(contains(state.read("pairing-code"), status.code));
}

TEST(A404ThenA200InOneRunPairsAndInstallsTheCredential) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::notFound(), FakeTransport::notFound(),
                           FakeTransport::paired("smsh_abc123", "principal-1")};
    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    state.write("pairing-request", "{}");
    worker.tick(0);
    CHECK(worker.status().state == PairingState::kWaiting);

    worker.tick(3000);
    CHECK(worker.status().state == PairingState::kWaiting);

    worker.tick(6000);
    CHECK_EQ(static_cast<int>(transport.requests.size()), 3);
    const PairingStatus status = worker.status();
    CHECK(status.state == PairingState::kPaired);
    CHECK_EQ(status.principalId, std::string("principal-1"));
    CHECK(status.code.empty());
    CHECK(cred.exists("credential"));
    CHECK_EQ(cred.read("credential"), std::string("smsh_abc123"));
}

TEST(A404KeepsWaitingUntilExpiry) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::notFound()};
    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    state.write("pairing-request", "{}");
    worker.tick(0);
    CHECK(worker.status().state == PairingState::kWaiting);

    // A tick before the claim interval elapses does not attempt another
    // claim.
    worker.tick(1000);
    CHECK_EQ(static_cast<int>(transport.requests.size()), 1);

    // Past the claim interval, still 404: still waiting.
    worker.tick(3000);
    CHECK_EQ(static_cast<int>(transport.requests.size()), 2);
    CHECK(worker.status().state == PairingState::kWaiting);

    // Past the 10-minute expiry: expired, pairing-code is gone, and the
    // code no longer appears in status.
    worker.tick(10 * 60 * 1000 + 1);
    const PairingStatus status = worker.status();
    CHECK(status.state == PairingState::kExpired);
    CHECK(status.code.empty());
    CHECK(!state.exists("pairing-code"));
}

TEST(A200PairsOnceAndInstallsTheCredential) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::paired("smsh_abc123", "principal-1")};
    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    state.write("pairing-request", "{}");
    worker.tick(0);
    CHECK_EQ(static_cast<int>(transport.requests.size()), 1);
    CHECK_EQ(transport.requests[0].url, std::string("http://coordinator.invalid:8080/api/v1/integrations/fpp/pairing/claim"));
    CHECK(transport.requests[0].bearerToken.empty());

    const PairingStatus status = worker.status();
    CHECK(status.state == PairingState::kPaired);
    CHECK_EQ(status.principalId, std::string("principal-1"));
    CHECK(status.code.empty());
    CHECK(!state.exists("pairing-code"));
    CHECK(cred.exists("credential"));
    CHECK_EQ(cred.read("credential"), std::string("smsh_abc123"));
    CHECK_EQ(cred.modeOf("credential"), static_cast<::mode_t>(0600));
    CHECK_EQ(cred.modeOf(""), static_cast<::mode_t>(0700));

    // A second tick, with nothing left scripted, must not attempt another
    // claim: pairing already ended.
    worker.tick(1000);
    CHECK_EQ(static_cast<int>(transport.requests.size()), 1);
}

TEST(AnUnparseable200EndsThePairingAsFailedRatherThanKeepWaiting) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::unparseable200()};
    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    state.write("pairing-request", "{}");
    worker.tick(0);

    const PairingStatus status = worker.status();
    CHECK(status.state == PairingState::kFailed);
    CHECK(!status.lastError.empty());
    CHECK(status.code.empty());
    CHECK(!state.exists("pairing-code"));
    CHECK(!cred.exists("credential"));

    // The claim is spent: nothing polls again on the next tick.
    worker.tick(1000);
    CHECK_EQ(static_cast<int>(transport.requests.size()), 1);
}

TEST(ACredentialWriteFailureEndsThePairingAsFailedAndForgetsTheSecret) {
    TempDir state("showmesh-pairing-state");
    TempDir parent("showmesh-pairing-cred-parent");
    // A regular file where the credential directory should be: mkdir()
    // fails with ENOTDIR rather than EEXIST, so
    // writeCoordinatorCredentialAtomically() cannot create it.
    const std::string credentialPath = parent.path() + "/not-a-directory";
    parent.write("not-a-directory", "occupied");

    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::paired("smsh_abc123", "principal-1")};
    PairingWorker worker(state.path(), credentialPath, &transport, &url, testClock, fixedBytes);

    state.write("pairing-request", "{}");
    worker.tick(0);

    const PairingStatus status = worker.status();
    CHECK(status.state == PairingState::kFailed);
    CHECK(!status.lastError.empty());
    CHECK(status.code.empty());
    // The token itself never appears in the operator-facing error.
    CHECK(!contains(status.lastError, "smsh_abc123"));
    CHECK(!state.exists("pairing-code"));

    // The secret is forgotten: a repeat tick with nothing left scripted
    // does not attempt another claim.
    worker.tick(1000);
    CHECK_EQ(static_cast<int>(transport.requests.size()), 1);
}

TEST(ARepeatedPairingRequestRestartsFromAnyState) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::paired("smsh_first", "principal-1")};
    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    state.write("pairing-request", "{}");
    worker.tick(0);
    CHECK(worker.status().state == PairingState::kPaired);

    transport.responses = {FakeTransport::notFound()};
    state.write("pairing-request", "{}");
    worker.tick(1000);
    CHECK(worker.status().state == PairingState::kWaiting);
    CHECK(!worker.status().code.empty());
}

TEST(AProcessRestartDuringAWaitReconcilesToExpiredAndCleansUpTheCodeFile) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;

    // Simulate a previous process left mid-wait: a waiting
    // pairing-status.json and its pairing-code, with no PairingWorker
    // instance alive to remember the secret.
    state.write("pairing-status.json",
               "{\"state\":\"waiting\",\"code\":\"ABCD-1234\",\"principalId\":\"\",\"pairedAtMillis\":0,"
               "\"lastError\":\"\",\"updatedAtMillis\":500}");
    state.write("pairing-code", "{\"code\":\"ABCD-1234\",\"expiresAtMillis\":600000}");

    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    const PairingStatus status = worker.status();
    CHECK(status.state == PairingState::kExpired);
    CHECK(status.code.empty());
    CHECK(!state.exists("pairing-code"));
    CHECK(contains(state.read("pairing-status.json"), "\"state\":\"expired\""));

    // No claim is ever attempted for a secret this process never had.
    worker.tick(1000);
    CHECK(transport.requests.empty());
}

TEST(AProcessRestartWithNoPriorPairingStaysIdle) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    CHECK(worker.status().state == PairingState::kIdle);
    CHECK(!state.exists("pairing-status.json"));
}

TEST(AProcessRestartAfterAPriorPairingAlreadyEndedIsLeftUntouched) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;

    state.write("pairing-status.json",
               "{\"state\":\"paired\",\"code\":\"\",\"principalId\":\"principal-1\",\"pairedAtMillis\":500,"
               "\"lastError\":\"\",\"updatedAtMillis\":500}");

    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);
    const PairingStatus status = worker.status();
    CHECK(status.state == PairingState::kPaired);
    CHECK_EQ(status.principalId, std::string("principal-1"));
}

TEST(NeitherOnDiskPairingFileEverContainsTheSecretOrTheToken) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::paired("smsh_the-real-token", "principal-1")};
    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    state.write("pairing-request", "{}");
    worker.tick(0);

    const std::string secretHex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const std::string statusContents = state.read("pairing-status.json");
    CHECK(!contains(statusContents, secretHex));
    CHECK(!contains(statusContents, "smsh_the-real-token"));
    // pairing-code was already deleted once paired; the claim request
    // body itself is the only place the secret is allowed to travel.
    CHECK(!state.exists("pairing-code"));
    CHECK_EQ(static_cast<int>(transport.requests.size()), 1);
    CHECK(!contains(transport.requests[0].url, "smsh_the-real-token"));
}

TEST(RequestStopReturnsPromptlyEvenWithAClaimAboutToRun) {
    // A transport whose post() blocks briefly, simulating a slow
    // coordinator. requestStop() must not wait out the pairing's full
    // claim cadence; it only has to wait for whatever the currently
    // running background-thread pass is doing, bounded by this fake's own
    // short delay.
    class SlowTransport : public HttpTransport {
     public:
        HttpResponse post(const HttpRequest&) override {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            return FakeTransport::notFound();
        }
        HttpResponse get(const HttpRequest& request) override { return post(request); }
    };

    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    SlowTransport transport;
    PairingWorker worker(state.path(), cred.path(), &transport, &url, testClock, fixedBytes);

    state.write("pairing-request", "{}");
    worker.start();
    // Give the background thread a moment to pick up the request and
    // enter its first (slow) claim attempt.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto begin = std::chrono::steady_clock::now();
    worker.stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    // Comfortably under the 3s claim interval and the 10s legacy
    // timeout this replaces; bounded by the fake's own 150ms delay plus
    // scheduling slack.
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 2000);
}
