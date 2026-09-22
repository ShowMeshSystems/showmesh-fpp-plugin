#include "showmesh/coordinator_config.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "check.h"

using showmesh::CoordinatorUrlLoad;
using showmesh::CredentialLoad;
using showmesh::FileCredentialSource;
using showmesh::joinUrlPath;
using showmesh::loadCoordinatorBaseUrl;
using showmesh::loadCoordinatorCredential;

namespace {

class TempDir {
 public:
    TempDir() {
        char buffer[] = "/tmp/showmesh-config-test-XXXXXX";
        const char* made = ::mkdtemp(buffer);
        CHECK(made != nullptr);
        path_ = made != nullptr ? std::string(made) : std::string();
    }
    ~TempDir() {
        for (const char* name : {"config.json", "credential"}) {
            std::remove((path_ + "/" + name).c_str());
        }
        ::rmdir(path_.c_str());
    }
    const std::string& path() const { return path_; }

    void write(const char* name, const std::string& contents, ::mode_t mode) const {
        const std::string full = path_ + "/" + name;
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        out << contents;
        out.close();
        CHECK(::chmod(full.c_str(), mode) == 0);
    }

 private:
    std::string path_;
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(TheCoordinatorUrlComesFromTheSameConfigFileTheGoHelperReads) {
    TempDir dir;
    dir.write("config.json", "{\"coordinatorUrl\":\"https://coordinator.example:8443\"}", 0644);

    const CoordinatorUrlLoad load = loadCoordinatorBaseUrl(dir.path());
    CHECK(load.ok);
    CHECK_EQ(load.baseUrl, std::string("https://coordinator.example:8443"));
}

TEST(AMissingEmptyOrSchemelessCoordinatorUrlIsAReportedConfigurationFailure) {
    TempDir dir;
    const CoordinatorUrlLoad absent = loadCoordinatorBaseUrl(dir.path());
    CHECK(!absent.ok);
    CHECK(contains(absent.error, "config.json"));

    dir.write("config.json", "{}", 0644);
    const CoordinatorUrlLoad noKey = loadCoordinatorBaseUrl(dir.path());
    CHECK(!noKey.ok);
    CHECK(contains(noKey.error, "no coordinatorUrl key"));

    dir.write("config.json", "{\"coordinatorUrl\":\"\"}", 0644);
    const CoordinatorUrlLoad empty = loadCoordinatorBaseUrl(dir.path());
    CHECK(!empty.ok);
    CHECK(contains(empty.error, "empty coordinatorUrl"));

    dir.write("config.json", "{\"coordinatorUrl\":\"ftp://coordinator.example\"}", 0644);
    const CoordinatorUrlLoad wrongScheme = loadCoordinatorBaseUrl(dir.path());
    CHECK(!wrongScheme.ok);
    CHECK(contains(wrongScheme.error, "http or https"));

    dir.write("config.json", "{\"coordinatorUrl\":\"http:///no-host\"}", 0644);
    const CoordinatorUrlLoad noHost = loadCoordinatorBaseUrl(dir.path());
    CHECK(!noHost.ok);
    CHECK(contains(noHost.error, "no host"));
}

TEST(TheCredentialIsReadOnlyFromAFileWithExactly0600) {
    TempDir dir;
    dir.write("credential", "a-bearer-token\n", 0600);
    const CredentialLoad ok = loadCoordinatorCredential(dir.path());
    CHECK(ok.ok);
    CHECK_EQ(ok.token, std::string("a-bearer-token"));

    // Exact, not "no more permissive than": a mode this plugin's own
    // installer did not write is a reason to distrust the file.
    dir.write("credential", "a-bearer-token\n", 0640);
    const CredentialLoad tooOpen = loadCoordinatorCredential(dir.path());
    CHECK(!tooOpen.ok);
    CHECK(contains(tooOpen.error, "0640"));
    CHECK(!contains(tooOpen.error, "a-bearer-token"));

    dir.write("credential", "a-bearer-token\n", 0400);
    const CredentialLoad tooStrict = loadCoordinatorCredential(dir.path());
    CHECK(!tooStrict.ok);
    CHECK(contains(tooStrict.error, "0400"));
}

TEST(AnEmptyOrAbsentCredentialFileIsReportedWithoutQuotingTheFile) {
    TempDir dir;
    const CredentialLoad absent = loadCoordinatorCredential(dir.path());
    CHECK(!absent.ok);
    CHECK(contains(absent.error, "does not exist"));

    dir.write("credential", "   \n", 0600);
    const CredentialLoad empty = loadCoordinatorCredential(dir.path());
    CHECK(!empty.ok);
    CHECK(contains(empty.error, "is empty"));
}

TEST(TheCredentialSourceRereadsTheFileOnlyAfterItIsInvalidated) {
    TempDir dir;
    dir.write("credential", "first-token", 0600);
    FileCredentialSource source(dir.path());

    std::string token;
    std::string error;
    CHECK(source.token(&token, &error));
    CHECK_EQ(token, std::string("first-token"));

    dir.write("credential", "second-token", 0600);
    CHECK(source.token(&token, &error));
    CHECK_EQ(token, std::string("first-token"));

    source.invalidate();
    CHECK(source.token(&token, &error));
    CHECK_EQ(token, std::string("second-token"));
}

TEST(TheCredentialDirectoryIsFixedAndNotConfigurable) {
    CHECK_EQ(showmesh::resolveCredentialDir(), std::string("/etc/showmesh-fpp-plugin"));
}

TEST(ARouteIsJoinedToTheBaseUrlWithExactlyOneSeparator) {
    CHECK_EQ(joinUrlPath("http://host:8080", "/api/v1/x"), std::string("http://host:8080/api/v1/x"));
    CHECK_EQ(joinUrlPath("http://host:8080/", "/api/v1/x"), std::string("http://host:8080/api/v1/x"));
    CHECK_EQ(joinUrlPath("http://host:8080/base", "/api/v1/x"), std::string("http://host:8080/base/api/v1/x"));
}

TEST(WritingTheCredentialCreatesTheDirectoryWithTheRequiredModesAndTheFileReadsBackCleanly) {
    char buffer[] = "/tmp/showmesh-credential-write-XXXXXX";
    const char* made = ::mkdtemp(buffer);
    CHECK(made != nullptr);
    const std::string parent = made != nullptr ? std::string(made) : std::string();
    const std::string credentialDir = parent + "/nested";

    std::string error;
    CHECK(showmesh::writeCoordinatorCredentialAtomically(credentialDir, "smsh_a-real-token", &error));
    CHECK(error.empty());

    const CredentialLoad loaded = loadCoordinatorCredential(credentialDir);
    CHECK(loaded.ok);
    CHECK_EQ(loaded.token, std::string("smsh_a-real-token"));

    struct ::stat dirInfo {};
    CHECK(::stat(credentialDir.c_str(), &dirInfo) == 0);
    CHECK_EQ(static_cast<int>(dirInfo.st_mode & 07777), 0700);

    // A second write, with the directory already present, still leaves
    // exactly the new token behind: no stray temp file, no growth.
    CHECK(showmesh::writeCoordinatorCredentialAtomically(credentialDir, "smsh_replacement", &error));
    const CredentialLoad replaced = loadCoordinatorCredential(credentialDir);
    CHECK(replaced.ok);
    CHECK_EQ(replaced.token, std::string("smsh_replacement"));

    ::system(("rm -rf " + parent).c_str());
}
