#include "showmesh/brightness_store.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "check.h"
#include "showmesh/brightness_codec.h"

using showmesh::BrightnessFileStore;
using showmesh::BrightnessState;
using showmesh::BrightnessStateLoad;
using showmesh::encodeBrightnessState;

namespace {

class TempDir {
 public:
    TempDir() {
        std::string tmpl = "/tmp/showmesh-brightness-store-test-XXXXXX";
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), "%s", tmpl.c_str());
        const char* made = ::mkdtemp(buffer);
        CHECK(made != nullptr);
        path_ = made != nullptr ? std::string(made) : std::string();
    }

    ~TempDir() {
        for (const char* name : {"brightness-state", "brightness-state.bak", "brightness-state.tmp",
                                 "brightness-state.bak.tmp"}) {
            std::remove((path_ + "/" + name).c_str());
        }
        ::rmdir(path_.c_str());
    }

    const std::string& path() const { return path_; }

 private:
    std::string path_;
};

void writeRaw(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

bool fileExists(const std::string& path) {
    std::ifstream in(path);
    return static_cast<bool>(in);
}

BrightnessState sampleState(double ceilingTarget) {
    BrightnessState s;
    s.revision = 7;
    s.stateChangedAtMillis = 1'800'000'000'000;
    s.instanceId = "node-a";
    s.ceilingStart = 100.0;
    s.ceilingTarget = ceilingTarget;
    s.lastAppliedCeiling = ceilingTarget;
    s.gainStart = 100.0;
    s.gainTarget = 100.0;
    s.lastAppliedGain = 100.0;
    s.persistedAtMillis = 1'800'000'005'000;
    return s;
}

}  // namespace

TEST(AMissingBrightnessStoreLoadsNothing) {
    TempDir dir;
    BrightnessFileStore store(dir.path());
    BrightnessStateLoad loaded = store.load();
    CHECK(!loaded.ok);
    // Nothing was ever written here: a caller can tell this apart from
    // "something was written and cannot be trusted" and safely use the
    // engine's own built-in defaults.
    CHECK(!loaded.recordExpectedButUnreadable);
}

TEST(BrightnessStoreThenLoadRoundTrips) {
    TempDir dir;
    BrightnessFileStore store(dir.path());
    CHECK(store.store(sampleState(42.0)));

    BrightnessStateLoad loaded = store.load();
    CHECK(loaded.ok);
    CHECK(loaded.trustedAsCurrent);
    CHECK_NEAR(loaded.state.ceilingTarget, 42.0, 1e-9);
    CHECK_EQ(loaded.state.instanceId, std::string("node-a"));
    CHECK_EQ(loaded.state.revision, static_cast<std::uint64_t>(7));
}

TEST(BrightnessStoreAlwaysOverwritesEvenWithADarkerValue) {
    // Unlike the sequence store, there is no monotonic floor here: the
    // store's only job is durability, not judging which value is "newer".
    TempDir dir;
    BrightnessFileStore store(dir.path());
    CHECK(store.store(sampleState(90.0)));
    CHECK(store.store(sampleState(10.0)));

    BrightnessStateLoad loaded = store.load();
    CHECK(loaded.ok);
    CHECK_NEAR(loaded.state.ceilingTarget, 10.0, 1e-9);
}

TEST(ACorruptedPrimaryFallsBackToTheRotatedBackup) {
    TempDir dir;
    BrightnessFileStore store(dir.path());
    CHECK(store.store(sampleState(80.0)));
    CHECK(store.store(sampleState(20.0)));  // rotates the 80 record into the backup slot

    writeRaw(dir.path() + "/brightness-state", "not json at all\n");
    BrightnessStateLoad loaded = store.load();
    CHECK(loaded.ok);
    CHECK_NEAR(loaded.state.ceilingTarget, 80.0, 1e-9);
    // The 80 record is the state the (now unreadable) 20 record
    // superseded, not the current one: a caller must not treat it as
    // "the last thing this host applied". See runtime_test.cpp's
    // ARestartWithACorruptedPrimaryAndAStaleBackupNeverComesBackBrighterThanWhatWasApplied
    // for what the engine does with that distinction.
    CHECK(!loaded.trustedAsCurrent);
}

TEST(ABitFlippedRecordWithAStillPlausibleChecksumMismatchIsRejected) {
    TempDir dir;
    BrightnessFileStore store(dir.path());
    CHECK(store.store(sampleState(80.0)));
    CHECK(store.store(sampleState(20.0)));  // backup now holds the 80 record

    // The line is tampered with but the checksum line is left as it was
    // for the true value, so a store that trusted the value line alone
    // would silently resume at the wrong record.
    const std::string tamperedLine = encodeBrightnessState(sampleState(20.0)) + "x";
    const std::string originalChecksumLine = [&] {
        std::ifstream in(dir.path() + "/brightness-state", std::ios::binary);
        std::string line1, line2;
        std::getline(in, line1);
        std::getline(in, line2);
        return line2;
    }();
    writeRaw(dir.path() + "/brightness-state", tamperedLine + "\n" + originalChecksumLine + "\n");
    BrightnessStateLoad loaded = store.load();
    CHECK(loaded.ok);
    CHECK_NEAR(loaded.state.ceilingTarget, 80.0, 1e-9);
}

TEST(BothFilesCorruptedReturnsNothingRatherThanGarbageOrACrash) {
    TempDir dir;
    BrightnessFileStore store(dir.path());
    CHECK(store.store(sampleState(50.0)));
    CHECK(store.store(sampleState(60.0)));  // both primary and backup now exist

    writeRaw(dir.path() + "/brightness-state", "garbage");
    writeRaw(dir.path() + "/brightness-state.bak", "also garbage");
    BrightnessStateLoad loaded = store.load();
    CHECK(!loaded.ok);
    // Both files exist but neither can be trusted: distinct from a fresh
    // install, where recordExpectedButUnreadable stays false.
    CHECK(loaded.recordExpectedButUnreadable);
}

TEST(BrightnessTrailingContentAfterTheChecksumLineIsRejectedAsCorrupt) {
    TempDir dir;
    BrightnessFileStore store(dir.path());
    CHECK(store.store(sampleState(55.0)));
    const std::string valid = [&] {
        std::ifstream in(dir.path() + "/brightness-state", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }();
    writeRaw(dir.path() + "/brightness-state", valid + "unexpected trailing bytes\n");
    BrightnessStateLoad loaded = store.load();
    CHECK(!loaded.ok);
}

TEST(BrightnessStoreDoesNotLeaveATempFileVisibleUnderTheFinalName) {
    TempDir dir;
    BrightnessFileStore store(dir.path());
    CHECK(store.store(sampleState(15.0)));
    CHECK(fileExists(dir.path() + "/brightness-state"));
    CHECK(!fileExists(dir.path() + "/brightness-state.tmp"));
}
