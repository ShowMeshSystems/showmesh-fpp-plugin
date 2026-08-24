#include "showmesh/sequence_store.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "check.h"

using showmesh::resolveSequenceStateDir;
using showmesh::SequenceFileStore;

namespace {

// A directory under the system temp root, created with mkdtemp so
// concurrent test runs never collide, and removed (files then the
// directory itself) when the test case returns. Every test in this file
// touches only files this fixture created inside its own directory.
class TempDir {
 public:
    TempDir() {
        std::string tmpl = "/tmp/showmesh-sequence-store-test-XXXXXX";
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), "%s", tmpl.c_str());
        const char* made = ::mkdtemp(buffer);
        CHECK(made != nullptr);
        path_ = made != nullptr ? std::string(made) : std::string();
    }

    ~TempDir() {
        for (const char* name : {"sequence-state", "sequence-state.bak", "sequence-state.tmp",
                                 "sequence-state.bak.tmp"}) {
            std::remove((path_ + "/" + name).c_str());
        }
        ::rmdir(path_.c_str());
    }

    const std::string& path() const { return path_; }

 private:
    std::string path_;
};

// Overwrites path with raw bytes, bypassing SequenceFileStore entirely,
// to stand in for a file that arrived corrupt or truncated by some means
// other than this class's own store() (a hand edit, a bit flip, a torn
// write on a filesystem that does not honor rename()'s atomicity).
void writeRaw(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

bool fileExists(const std::string& path) {
    std::ifstream in(path);
    return static_cast<bool>(in);
}

}  // namespace

TEST(AMissingStoreStartsClean) {
    TempDir dir;
    SequenceFileStore store(dir.path());
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(0));
}

TEST(StoreThenLoadRoundTrips) {
    TempDir dir;
    SequenceFileStore store(dir.path());
    CHECK(store.store(42));
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(42));

    CHECK(store.store(43));
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(43));
}

TEST(StoreRefusesToPersistAValueLowerThanWhatIsAlreadyDurable) {
    TempDir dir;
    SequenceFileStore store(dir.path());
    CHECK(store.store(100));
    CHECK(!store.store(50));
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(100));

    // Equal is not a regression; it is idempotent.
    CHECK(store.store(100));
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(100));
}

TEST(ACorruptedPrimaryFallsBackToTheRotatedBackup) {
    TempDir dir;
    SequenceFileStore store(dir.path());
    CHECK(store.store(10));
    CHECK(store.store(20));  // rotates the old primary (10) into the backup slot

    writeRaw(dir.path() + "/sequence-state", "not a number at all\n");
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(10));
}

TEST(ATruncatedPrimaryNeverYieldsALowerValueThanWasDurablyPersisted) {
    TempDir dir;
    SequenceFileStore store(dir.path());
    CHECK(store.store(100));
    CHECK(store.store(150));  // backup now holds the durable value 100

    // Simulate a write torn mid-flight: the value line is cut short, so a
    // naive parser would happily read "1" instead of noticing the record
    // is incomplete.
    writeRaw(dir.path() + "/sequence-state", "1");
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(100));

    // An empty file (the temp-write phase truncated to nothing) is the
    // same failure mode.
    writeRaw(dir.path() + "/sequence-state", "");
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(100));
}

TEST(ABitFlippedValueWithAStillPlausibleChecksumMismatchIsRejected) {
    TempDir dir;
    SequenceFileStore store(dir.path());
    CHECK(store.store(100));
    CHECK(store.store(999));  // backup now holds 100

    // The value line is changed but the checksum line is left as it was
    // for the true value, so a store that trusted the value line alone
    // would silently resume at the wrong, lower number.
    writeRaw(dir.path() + "/sequence-state",
             "1\n" + std::string("ef2d127de37b942baad06145e54b0c619a1f22327b2ebbcfbec78f5564afe39\n"));
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(100));
}

TEST(BothFilesCorruptedReturnsZeroRatherThanGarbageOrACrash) {
    TempDir dir;
    SequenceFileStore store(dir.path());
    CHECK(store.store(500));
    CHECK(store.store(700));  // both primary and backup now exist

    writeRaw(dir.path() + "/sequence-state", "garbage");
    writeRaw(dir.path() + "/sequence-state.bak", "also garbage");
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(0));
}

TEST(TrailingContentAfterTheChecksumLineIsRejectedAsCorrupt) {
    TempDir dir;
    SequenceFileStore store(dir.path());
    CHECK(store.store(5));
    const std::string valid = [&] {
        std::ifstream in(dir.path() + "/sequence-state", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }();
    writeRaw(dir.path() + "/sequence-state", valid + "unexpected trailing bytes\n");
    CHECK_EQ(store.load(), static_cast<std::uint64_t>(0));
}

TEST(StoreDoesNotLeaveATempFileVisibleUnderTheFinalName) {
    TempDir dir;
    SequenceFileStore store(dir.path());
    CHECK(store.store(1));
    CHECK(fileExists(dir.path() + "/sequence-state"));
    CHECK(!fileExists(dir.path() + "/sequence-state.tmp"));
}

TEST(ResolveSequenceStateDirFollowsTheSamePrecedenceAsTheGoHelper) {
    // Save and restore the process environment: these variables are
    // process-global, and this test must not leak a value into whichever
    // test case the harness happens to run next.
    const char* savedOverride = std::getenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR");
    const std::string savedOverrideValue = savedOverride != nullptr ? savedOverride : std::string();
    const bool hadOverride = savedOverride != nullptr;
    const char* savedMedia = std::getenv("MEDIADIR");
    const std::string savedMediaValue = savedMedia != nullptr ? savedMedia : std::string();
    const bool hadMedia = savedMedia != nullptr;

    ::unsetenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR");
    ::unsetenv("MEDIADIR");
    CHECK_EQ(resolveSequenceStateDir(), std::string("/home/fpp/media/plugindata/fpp-showmesh"));

    ::setenv("MEDIADIR", "/mnt/alt-media", 1);
    CHECK_EQ(resolveSequenceStateDir(), std::string("/mnt/alt-media/plugindata/fpp-showmesh"));

    ::setenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR", "/opt/showmesh-state", 1);
    CHECK_EQ(resolveSequenceStateDir(), std::string("/opt/showmesh-state"));

    ::unsetenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR");
    ::unsetenv("MEDIADIR");
    if (hadOverride) ::setenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR", savedOverrideValue.c_str(), 1);
    if (hadMedia) ::setenv("MEDIADIR", savedMediaValue.c_str(), 1);
}
