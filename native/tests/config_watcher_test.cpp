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

class NoCredentials : public CredentialSource {
 public:
    bool token(std::string*, std::string*) override { return false; }
    void invalidate() override {}
};

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

    // A real change: sleep past filesystem mtime resolution, then write a
    // different URL of a different length so both mtime and size change.
    ::usleep(1100 * 1000);
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

    ::usleep(1100 * 1000);
    dir.remove();
    watcher.tick();
    CHECK(watcher.currentBaseUrl().empty());
    CHECK(!client.status().configured);
    CHECK(!client.status().configurationError.empty());
}
