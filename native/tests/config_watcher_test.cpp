#include "showmesh/config_watcher.h"

#include <unistd.h>

#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "showmesh/coordinator_client.h"
#include "showmesh/observation_payload.h"

using showmesh::ConfigWatcher;
using showmesh::CoordinatorClient;
using showmesh::CredentialSource;
using showmesh::HttpRequest;
using showmesh::HttpResponse;
using showmesh::HttpTransport;
using showmesh::PlaylistAction;
using showmesh::PlaylistEntryObservation;

namespace {

showmesh::TimeMillis testClock() { return 1000; }

class TempDir {
 public:
    TempDir() {
        char buffer[] = "/tmp/showmesh-config-watcher-XXXXXX";
        const char* made = ::mkdtemp(buffer);
        CHECK(made != nullptr);
        path_ = made != nullptr ? std::string(made) : std::string();
    }
    ~TempDir() { ::system(("rm -rf " + path_).c_str()); }
    const std::string& path() const { return path_; }

    void write(const std::string& contents) const {
        std::ofstream out(path_ + "/config.json", std::ios::binary | std::ios::trunc);
        out << contents;
    }
    void remove() const { ::unlink((path_ + "/config.json").c_str()); }

 private:
    std::string path_;
};

class NoopTransport : public HttpTransport {
 public:
    HttpResponse post(const HttpRequest&) override { return HttpResponse{}; }
    HttpResponse get(const HttpRequest&) override { return HttpResponse{}; }
};

// Records every request it is asked to send and answers success, so a
// test can prove whether a post was attempted at all, not just whether
// the client called itself configured.
class RecordingTransport : public HttpTransport {
 public:
    std::vector<HttpRequest> requests;
    HttpResponse post(const HttpRequest& request) override {
        requests.push_back(request);
        HttpResponse r;
        r.transportOk = true;
        r.statusCode = 200;
        r.body = "{}";
        return r;
    }
    HttpResponse get(const HttpRequest& request) override { return post(request); }
};

class NoCredentials : public CredentialSource {
 public:
    bool token(std::string* out, std::string*) override {
        *out = "a-token";
        return true;
    }
    void invalidate() override {}
};

PlaylistEntryObservation resolvedObservation() {
    PlaylistEntryObservation observation;
    observation.identity.instanceUuid = "6f1c1a52-1b6c-4b53-9a0e-9f7c2f0d1b44";
    observation.identity.playlistName = "Halloween Main";
    observation.identity.playlistHash = std::string(64, 'a');
    observation.identity.section = "mainPlaylist";
    observation.identity.position = 3;
    observation.entryKey = std::string(64, 'b');
    observation.sequenceFilename = "Thriller.fseq";
    observation.action = PlaylistAction::kPlaying;
    observation.sequence = 42;
    observation.observedAtMillis = 1000;
    return observation;
}

}  // namespace

TEST(TheWatcherAppliesANewUrlOnlyWhenTheFileActuallyChanges) {
    TempDir dir;
    dir.write("{\"coordinatorUrl\":\"https://first.example\"}");

    NoopTransport transport;
    NoCredentials credentials;
    CoordinatorClient client(&transport, &credentials, std::string(), testClock);
    ConfigWatcher watcher(dir.path(), &client);

    CHECK_EQ(watcher.currentBaseUrl(), std::string("https://first.example"));

    // No change: a tick must not re-apply anything (the client's own
    // constructor-time state is left as is).
    watcher.tick();
    CHECK_EQ(watcher.currentBaseUrl(), std::string("https://first.example"));

    dir.write("{\"coordinatorUrl\":\"https://second.example:9090\"}");
    watcher.tick();
    CHECK_EQ(watcher.currentBaseUrl(), std::string("https://second.example:9090"));
    CHECK(client.status().configured);
}

TEST(TheWatcherRecordsAConfigurationErrorWhenTheFileBecomesUnusable) {
    TempDir dir;
    dir.write("{\"coordinatorUrl\":\"https://first.example\"}");

    NoopTransport transport;
    NoCredentials credentials;
    CoordinatorClient client(&transport, &credentials, std::string(), testClock);
    ConfigWatcher watcher(dir.path(), &client);

    dir.remove();
    watcher.tick();
    CHECK(watcher.currentBaseUrl().empty());
    CHECK(!client.status().configured);
    CHECK(!client.status().configurationError.empty());
}

// A malformed rewrite must stop CoordinatorClient from posting to the URL
// config.json no longer names, not merely report itself unconfigured
// while still holding the stale URL.
TEST(AMalformedRewriteStopsThePosterFromUsingTheStaleUrl) {
    TempDir dir;
    dir.write("{\"coordinatorUrl\":\"https://first.example\"}");

    RecordingTransport transport;
    NoCredentials credentials;
    // Seeded with the same URL config.json already names, mirroring how
    // CoordinatorDelivery constructs both the client and ConfigWatcher
    // from the same construction-time read in production.
    CoordinatorClient client(&transport, &credentials, "https://first.example", testClock);
    ConfigWatcher watcher(dir.path(), &client);

    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(static_cast<int>(transport.requests.size()), 1);
    CHECK(client.status().configured);

    dir.write("not valid json");
    watcher.tick();
    CHECK(!client.status().configured);
    CHECK(!client.status().configurationError.empty());
    CHECK(watcher.currentBaseUrl().empty());

    // The real assertion: no post is even attempted against the URL the
    // client held a moment ago.
    CHECK(!client.publish(resolvedObservation()));
    CHECK_EQ(static_cast<int>(transport.requests.size()), 1);

    // And a later successful post (once config.json is fixed) is what is
    // allowed to declare configured again, not a leftover posting to a
    // URL config.json no longer names.
    dir.write("{\"coordinatorUrl\":\"https://second.example\"}");
    watcher.tick();
    CHECK(client.status().configured);
    CHECK(client.publish(resolvedObservation()));
    CHECK_EQ(static_cast<int>(transport.requests.size()), 2);
}

// Two rewrites landing within the same mtime tick, with identical size
// but different content (a URL swapped for another of the same length),
// must still be detected: nanosecond mtime resolution (or, on a
// filesystem coarser than that, the content hash) has to catch what
// second-resolution mtime and size alone would miss.
TEST(TwoRewritesOfTheSameSizeWithinOneSecondAreBothDetected) {
    TempDir dir;
    dir.write("{\"coordinatorUrl\":\"https://aaaaaaaa.example\"}");

    NoopTransport transport;
    NoCredentials credentials;
    CoordinatorClient client(&transport, &credentials, std::string(), testClock);
    ConfigWatcher watcher(dir.path(), &client);
    CHECK_EQ(watcher.currentBaseUrl(), std::string("https://aaaaaaaa.example"));

    // Same byte length as the URL above; no sleep, so this can land in
    // the same mtime second on a filesystem with coarse resolution.
    dir.write("{\"coordinatorUrl\":\"https://bbbbbbbb.example\"}");
    watcher.tick();
    CHECK_EQ(watcher.currentBaseUrl(), std::string("https://bbbbbbbb.example"));
}

TEST(SnapshotConfigFileReportsAbsentForAMissingFile) {
    const showmesh::ConfigFileSnapshot snapshot = showmesh::snapshotConfigFile("/nonexistent/showmesh/config.json");
    CHECK(!snapshot.exists);
}
