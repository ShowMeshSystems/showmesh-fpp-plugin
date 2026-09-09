// Unit tests for the FPP-facing edges of the adapter boundary: the
// runtime-to-canonical section name mapping (section_names.h) and the
// currentEntry filename extraction (callback_fields.h). These two headers
// are the only adapter-shared code that needs nothing but jsoncpp, so they
// can be exercised here without an installed FPP source tree.
//
// SM-275: before this change, the plugin forwarded FPP's runtime section
// spelling ("LeadIn"/"MainPlaylist"/"LeadOut") straight through, which
// never matched the coordinator's definition-derived entry key spelling
// ("leadIn"/"mainPlaylist"/"leadOut"), and it read sequence/media
// filenames from a "playlist[section]" array that does not exist on FPP
// 10's callback object. Both are exercised below against the frozen
// coordinator fixture and against FPP 10's actual GetInfo() shape.

#include <csignal>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "callback_fields.h"
#include "check.h"
#include "fallback_program_installer.h"
#include "fallback_program_verifier.h"
#include "section_names.h"
#include "showmesh/playlist_identity.h"

using showmesh::EntryIdentity;
using showmesh::deriveEntryKey;
using showmesh::adapter::canonicalPlaylistSection;
using showmesh::adapter::mediaFilenameOf;
using showmesh::adapter::playlistLoopOf;
using showmesh::adapter::sequenceFilenameOf;

namespace {

// test/fixtures/fpp/entry-key.json "baseline" case, vendored verbatim from
// the coordinator repository. If this stops matching, the coordinator and
// this plugin have drifted on the entry-key contract.
constexpr const char* kBaselineEntryKey = "4412de28018bd7bd4b20df96341da9737a24ec604d88c2b2372a8a068f55e591";

}  // namespace

// --- section name mapping -------------------------------------------------

TEST(RuntimeSpellingMapsToCanonical) {
    CHECK_EQ(canonicalPlaylistSection("LeadIn"), std::string("leadIn"));
    CHECK_EQ(canonicalPlaylistSection("MainPlaylist"), std::string("mainPlaylist"));
    CHECK_EQ(canonicalPlaylistSection("LeadOut"), std::string("leadOut"));
}

TEST(UnknownSectionPassesThroughUnchanged) {
    CHECK_EQ(canonicalPlaylistSection("New"), std::string("New"));
    CHECK_EQ(canonicalPlaylistSection(""), std::string(""));
    CHECK_EQ(canonicalPlaylistSection("mainPlaylist"), std::string("mainPlaylist"));
}

TEST(CanonicalSectionProducesFixtureEntryKey) {
    EntryIdentity identity;
    identity.instanceUuid = "6f1c1a52-1b6c-4b53-9a0e-9f7c2f0d1b44";
    identity.playlistHash = "deadbeef";
    identity.playlistName = "Main Show";
    identity.position = 0;
    identity.section = canonicalPlaylistSection("MainPlaylist");

    CHECK_EQ(deriveEntryKey(identity), std::string(kBaselineEntryKey));
}

// --- currentEntry filename extraction -------------------------------------

TEST(SequenceEntryFilenameComesFromCurrentEntry) {
    Json::Value playlist;
    playlist["currentState"] = "playing";
    playlist["currentEntry"]["type"] = "sequence";
    playlist["currentEntry"]["sequenceName"] = "Lane14-One.fseq";

    CHECK_EQ(sequenceFilenameOf(playlist), std::string("Lane14-One.fseq"));
    CHECK_EQ(mediaFilenameOf(playlist), std::string(""));
}

TEST(MediaEntryFilenameComesFromCurrentEntry) {
    Json::Value playlist;
    playlist["currentState"] = "playing";
    playlist["currentEntry"]["type"] = "media";
    playlist["currentEntry"]["mediaFilename"] = "Lane14-Intro.mp4";

    CHECK_EQ(mediaFilenameOf(playlist), std::string("Lane14-Intro.mp4"));
    CHECK_EQ(sequenceFilenameOf(playlist), std::string(""));
}

TEST(IdleCallbackWithNoCurrentEntryYieldsNoFilename) {
    // Playlist::GetInfo() sets result["currentEntry"] = GetCurrentEntry(),
    // and GetCurrentEntry() returns a default-constructed (null,
    // non-object) Json::Value while FPP_STATUS_IDLE. currentEntryOf() must
    // treat that the same as "absent" rather than crash or fabricate a
    // filename.
    Json::Value playlist;
    playlist["currentState"] = "idle";
    playlist["currentEntry"] = Json::Value();

    CHECK_EQ(sequenceFilenameOf(playlist), std::string(""));
    CHECK_EQ(mediaFilenameOf(playlist), std::string(""));
}

TEST(MissingCurrentEntryMemberYieldsNoFilename) {
    Json::Value playlist;
    playlist["currentState"] = "idle";

    CHECK_EQ(sequenceFilenameOf(playlist), std::string(""));
    CHECK_EQ(mediaFilenameOf(playlist), std::string(""));
}

// --- mainPlaylist pass counter --------------------------------------------

TEST(PlaylistLoopReadsLoopAndNeverLoopCount) {
    // This is the whole trap. Playlist::GetInfo() writes both members, and
    // the names invite reading the wrong one: `loop` is the running pass
    // counter (m_loop, incremented in Process()) and `loopCount` is the
    // configured repeat LIMIT it is compared against (m_loopCount, read
    // from the playlist config). Verified identical on FPP 9.5.3 and
    // 10.0. Reading loopCount would report a fixed limit as a lap number,
    // and for the common unlimited-repeat playlist it is 0 on every tick,
    // so the coordinator would never see it change and the loop re-entry
    // this field exists for would still be invisible.
    Json::Value playlist;
    playlist["currentState"] = "playing";
    playlist["loop"] = 2;
    playlist["loopCount"] = 0;  // unlimited repeat, the common show setting

    const std::optional<int> loop = playlistLoopOf(playlist);
    CHECK(loop.has_value());
    CHECK_EQ(*loop, 2);
}

TEST(PlaylistLoopZeroOnAPlayingPlaylistIsARealFirstPass) {
    Json::Value playlist;
    playlist["currentState"] = "playing";
    playlist["loop"] = 0;

    const std::optional<int> loop = playlistLoopOf(playlist);
    CHECK(loop.has_value());
    CHECK_EQ(*loop, 0);
}

TEST(PlaylistLoopIsAbsentWhileIdle) {
    // GetInfo()'s idle branch writes result["loop"] = 0 unconditionally.
    // That 0 means "no playlist is running", not "the running playlist is
    // on its first pass", and the two must not travel as the same value.
    Json::Value playlist;
    playlist["currentState"] = "idle";
    playlist["loop"] = 0;

    CHECK(!playlistLoopOf(playlist).has_value());
}

TEST(PlaylistLoopIsAbsentWhenTheMemberIsMissingOrNotAnInteger) {
    Json::Value missing;
    missing["currentState"] = "playing";
    CHECK(!playlistLoopOf(missing).has_value());

    Json::Value wrongType;
    wrongType["currentState"] = "playing";
    wrongType["loop"] = "2";
    CHECK(!playlistLoopOf(wrongType).has_value());

    CHECK(!playlistLoopOf(Json::Value()).has_value());
}

// --- fallback program verify and install ----------------------------------
//
// Fixtures under fixtures/fallback/ (see that directory's README) are
// signed with the coordinator's own Program.CanonicalBytes/SignedProgram
// code and a throwaway keypair, never hand-typed.

namespace {

const char* kFallbackFixtureDir = "shared/tests/fixtures/fallback";

const char* kValidPackageId = "11111111-1111-4111-8111-111111111111";
const char* kValidRevision = "test-revision-0001";

std::string readFixtureOrFail(const char* name) {
    const std::string path = std::string(kFallbackFixtureDir) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::vector<uint8_t> fixturePublicKey(const char* jsonField) {
    const std::string keysJson = readFixtureOrFail("keys.json");
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(keysJson);
    CHECK(Json::parseFromStream(builder, stream, &root, &errors));
    const std::string b64 = root[jsonField].asString();
    std::vector<uint8_t> key;
    CHECK(showmesh::fallback::detail::base64Decode(b64, &key));
    return key;
}

// A fresh, empty directory this test owns exclusively, so an install
// target and a previously-installed file never collide with another
// test or a leftover run.
class TempDir {
 public:
    TempDir() {
        char buffer[] = "/tmp/showmesh-fallback-test-XXXXXX";
        const char* made = ::mkdtemp(buffer);
        CHECK(made != nullptr);
        path_ = made != nullptr ? std::string(made) : std::string();
    }

    ~TempDir() {
        std::remove((path_ + "/fallback-program.json").c_str());
        std::remove((path_ + "/fallback-program.json.tmp").c_str());
        ::rmdir(path_.c_str());
    }

    std::string programPath() const { return path_ + "/fallback-program.json"; }

 private:
    std::string path_;
};

std::string readRawOrFail(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

TEST(ValidFallbackProgramVerifiesAndInstalls) {
    const std::string document = readFixtureOrFail("valid.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::FallbackVerifyResult verified =
        showmesh::fallback::VerifyFallbackProgram(document, publicKey);
    CHECK(verified.accepted);
    CHECK_EQ(verified.program->packageId(), std::string(kValidPackageId));
    CHECK_EQ(verified.program->revision(), std::string(kValidRevision));

    TempDir dir;
    const showmesh::fallback::InstallResult installed =
        showmesh::fallback::InstallFallbackProgram(*verified.program, dir.programPath());
    CHECK(installed.ok);
    CHECK_EQ(installed.report.packageId, std::string(kValidPackageId));
    CHECK_EQ(installed.report.revision, std::string(kValidRevision));
    CHECK_EQ(installed.report.verificationResult, std::string("accepted"));

    CHECK_EQ(readRawOrFail(dir.programPath()), document);
}

TEST(TamperedFallbackProgramIsRefusedAndNotInstalled) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    TempDir dir;

    // Install a previous good program first, so refusal of the tampered
    // one can be checked against "did the previous program survive",
    // not merely "did the call return false".
    const std::string previousDocument = readFixtureOrFail("valid.json");
    const showmesh::fallback::FallbackVerifyResult previousVerified =
        showmesh::fallback::VerifyFallbackProgram(previousDocument, publicKey);
    CHECK(previousVerified.accepted);
    CHECK(showmesh::fallback::InstallFallbackProgram(*previousVerified.program, dir.programPath()).ok);

    const std::string tamperedDocument = readFixtureOrFail("tampered-one-byte.json");
    const showmesh::fallback::FallbackVerifyResult tamperedVerified =
        showmesh::fallback::VerifyFallbackProgram(tamperedDocument, publicKey);

    CHECK(!tamperedVerified.accepted);
    CHECK(!tamperedVerified.refusalReason.empty());
    // A refused program is never handed to the installer: production
    // code only calls InstallFallbackProgram behind an accepted check,
    // exactly as exercised here.

    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(WrongKeyFallbackProgramIsRefusedAndNotInstalled) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    const showmesh::fallback::FallbackVerifyResult previousVerified =
        showmesh::fallback::VerifyFallbackProgram(previousDocument, publicKey);
    CHECK(previousVerified.accepted);
    CHECK(showmesh::fallback::InstallFallbackProgram(*previousVerified.program, dir.programPath()).ok);

    const std::string wrongKeyDocument = readFixtureOrFail("wrong-key.json");
    const showmesh::fallback::FallbackVerifyResult wrongKeyVerified =
        showmesh::fallback::VerifyFallbackProgram(wrongKeyDocument, publicKey);

    CHECK(!wrongKeyVerified.accepted);
    CHECK(!wrongKeyVerified.refusalReason.empty());

    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(InstalledFallbackProgramSurvivesRestartInAFreshRead) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    const std::string document = readFixtureOrFail("valid.json");
    TempDir dir;

    {
        const showmesh::fallback::FallbackVerifyResult verified =
            showmesh::fallback::VerifyFallbackProgram(document, publicKey);
        CHECK(verified.accepted);
        CHECK(showmesh::fallback::InstallFallbackProgram(*verified.program, dir.programPath()).ok);
    }

    // A fresh read, standing in for a restarted process that never held
    // the original in memory: this is what proves restart survival,
    // unlike comparing an in-memory field the installer never persisted.
    const showmesh::fallback::ReadInstalledResult reloaded =
        showmesh::fallback::ReadInstalledFallbackProgram(dir.programPath());
    CHECK(reloaded.ok);

    const showmesh::fallback::FallbackVerifyResult reverified =
        showmesh::fallback::VerifyFallbackProgram(reloaded.rawDocument, publicKey);
    CHECK(reverified.accepted);
    CHECK_EQ(reverified.program->packageId(), std::string(kValidPackageId));
    CHECK_EQ(reverified.program->revision(), std::string(kValidRevision));
    CHECK_EQ(reloaded.rawDocument, document);
}

TEST(WriteFailurePartwayLeavesPreviousProgramUnchanged) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    const showmesh::fallback::FallbackVerifyResult previousVerified =
        showmesh::fallback::VerifyFallbackProgram(previousDocument, publicKey);
    CHECK(previousVerified.accepted);
    CHECK(showmesh::fallback::InstallFallbackProgram(*previousVerified.program, dir.programPath()).ok);

    const std::string secondDocument = readFixtureOrFail("valid-second.json");
    const showmesh::fallback::FallbackVerifyResult secondVerified =
        showmesh::fallback::VerifyFallbackProgram(secondDocument, publicKey);
    CHECK(secondVerified.accepted);

    // Force a genuine OS-level partial write: cap this process's max
    // file size below the second document's length and ignore SIGXFSZ,
    // so write() returns a short write followed by -1/EFBIG instead of
    // killing the process, rather than mocking a failure injection point
    // that production code would never actually exercise.
    struct rlimit originalLimit {};
    CHECK(getrlimit(RLIMIT_FSIZE, &originalLimit) == 0);
    void (*originalHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);

    struct rlimit smallLimit = originalLimit;
    smallLimit.rlim_cur = 16;
    CHECK(smallLimit.rlim_cur < secondDocument.size());
    CHECK(setrlimit(RLIMIT_FSIZE, &smallLimit) == 0);

    const showmesh::fallback::InstallResult failedInstall =
        showmesh::fallback::InstallFallbackProgram(*secondVerified.program, dir.programPath());

    CHECK(setrlimit(RLIMIT_FSIZE, &originalLimit) == 0);
    std::signal(SIGXFSZ, originalHandler);

    CHECK(!failedInstall.ok);
    CHECK(!failedInstall.refusalReason.empty());
    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}
