#include "showmesh/pairing.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
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

namespace {

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
    PairingWorker worker(state.path(), cred.path(), &transport, &url, fixedBytes);

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

TEST(A404KeepsWaitingUntilExpiry) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::notFound()};
    PairingWorker worker(state.path(), cred.path(), &transport, &url, fixedBytes);

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

    // Past the 10-minute expiry: expired, and pairing-code is gone.
    worker.tick(10 * 60 * 1000 + 1);
    CHECK(worker.status().state == PairingState::kExpired);
    CHECK(!state.exists("pairing-code"));
}

TEST(A200PairsOnceAndInstallsTheCredential) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::paired("smsh_abc123", "principal-1")};
    PairingWorker worker(state.path(), cred.path(), &transport, &url, fixedBytes);

    state.write("pairing-request", "{}");
    worker.tick(0);
    CHECK_EQ(static_cast<int>(transport.requests.size()), 1);
    CHECK_EQ(transport.requests[0].url, std::string("http://coordinator.invalid:8080/api/v1/integrations/fpp/pairing/claim"));
    CHECK(transport.requests[0].bearerToken.empty());

    const PairingStatus status = worker.status();
    CHECK(status.state == PairingState::kPaired);
    CHECK_EQ(status.principalId, std::string("principal-1"));
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

TEST(ARepeatedPairingRequestRestartsFromAnyState) {
    TempDir state("showmesh-pairing-state");
    TempDir cred("showmesh-pairing-cred");
    FakeUrlSource url;
    url.url = "http://coordinator.invalid:8080";
    FakeTransport transport;
    transport.responses = {FakeTransport::paired("smsh_first", "principal-1")};
    PairingWorker worker(state.path(), cred.path(), &transport, &url, fixedBytes);

    state.write("pairing-request", "{}");
    worker.tick(0);
    CHECK(worker.status().state == PairingState::kPaired);

    transport.responses = {FakeTransport::notFound()};
    state.write("pairing-request", "{}");
    worker.tick(1000);
    CHECK(worker.status().state == PairingState::kWaiting);
}
