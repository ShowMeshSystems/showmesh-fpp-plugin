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
#include "fallback_program_fetch.h"
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

// --- fallback program fetch -------------------------------------------

namespace {

const char* kExpectedInstanceUuid = "22222222-2222-4222-8222-222222222222";
const char* kOtherInstanceUuid = "99999999-9999-4999-8999-999999999999";
const char* kBaseUrl = "http://coordinator.invalid:8080";

// 2026-09-08T10:00:00Z and 2026-09-08T13:00:00Z: the fixtures' own
// program.expiresAt is 2026-09-08T12:00:00Z, so these bracket it exactly
// where a real wall clock cannot be trusted to land relative to a fixed
// fixture value.
showmesh::TimeMillis clockBeforeExpiry() { return 1788861600000; }
showmesh::TimeMillis clockAfterExpiry() { return 1788872400000; }

class ScriptedTransport : public showmesh::HttpTransport {
 public:
    showmesh::HttpResponse get(const showmesh::HttpRequest& request) override {
        getCalls.push_back(request);
        return getResponse;
    }
    showmesh::HttpResponse post(const showmesh::HttpRequest& request) override {
        postCalls.push_back(request);
        return postResponse;
    }

    showmesh::HttpResponse getResponse;
    showmesh::HttpResponse postResponse;
    std::vector<showmesh::HttpRequest> getCalls;
    std::vector<showmesh::HttpRequest> postCalls;
};

class FixedCredentials : public showmesh::CredentialSource {
 public:
    bool token(std::string* out, std::string* error) override {
        if (!available) {
            *error = "no credential available";
            return false;
        }
        *out = "test-bearer-token";
        return true;
    }
    void invalidate() override {}

    bool available = true;
};

showmesh::HttpResponse okResponse(std::string body) {
    showmesh::HttpResponse r;
    r.transportOk = true;
    r.statusCode = 200;
    r.body = std::move(body);
    return r;
}

showmesh::HttpResponse statusResponse(int status) {
    showmesh::HttpResponse r;
    r.transportOk = true;
    r.statusCode = status;
    return r;
}

showmesh::HttpResponse unreachableResponse() {
    showmesh::HttpResponse r;
    r.transportOk = false;
    r.error = "connection refused";
    return r;
}

}  // namespace

TEST(FetchInstallsAValidPublishedProgram) {
    const std::string envelope = readFixtureOrFail("valid-get-response.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse(envelope);
    FixedCredentials credentials;
    TempDir dir;

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kInstalled);
    CHECK_EQ(outcome.packageId, std::string(kValidPackageId));
    CHECK_EQ(outcome.revision, std::string(kValidRevision));
    CHECK(showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));

    // The installed bytes are the reconstructed signed document
    // ({"program":...,"signature":"..."}), never the fetched HTTP
    // envelope: re-reading it must independently re-verify, exactly
    // slice one's restart-survival property.
    const showmesh::fallback::ReadInstalledResult reread =
        showmesh::fallback::ReadInstalledFallbackProgram(dir.programPath());
    CHECK(reread.ok);
    const showmesh::fallback::FallbackVerifyResult reverified =
        showmesh::fallback::VerifyFallbackProgram(reread.rawDocument, publicKey);
    CHECK(reverified.accepted);
    CHECK_EQ(reverified.program->packageId(), std::string(kValidPackageId));

    CHECK_EQ(transport.getCalls.size(), static_cast<size_t>(1));
    CHECK_EQ(transport.getCalls[0].bearerToken, std::string("test-bearer-token"));
    CHECK(transport.getCalls[0].maxResponseBytes > 8192);
}

TEST(FetchRefusesAndDoesNotInstallOnConnectionRefused) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = unreachableResponse();
    FixedCredentials credentials;
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    showmesh::writeFileAtomically(dir.programPath(), previousDocument);

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kTransportUnreachable);
    CHECK(!outcome.detail.empty());
    CHECK(!showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(FetchTreatsAnUnexpectedStatusAsRefusedAndDoesNotInstall) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    for (const int status : {404, 500, 503}) {
        ScriptedTransport transport;
        transport.getResponse = statusResponse(status);
        FixedCredentials credentials;
        TempDir dir;

        const std::string previousDocument = readFixtureOrFail("valid.json");
        showmesh::writeFileAtomically(dir.programPath(), previousDocument);

        const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
            &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(),
            &clockBeforeExpiry);

        CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kUnexpectedStatus);
        CHECK_EQ(outcome.statusCode, status);
        CHECK(!showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
        CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
    }
}

TEST(FetchTreatsATruncatedOrEmptyBodyAsMalformedAndDoesNotInstall) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    for (const std::string& body : {std::string(""), std::string("{\"published\":true,\"program\":{\"packageId\"")}) {
        ScriptedTransport transport;
        transport.getResponse = okResponse(body);
        FixedCredentials credentials;
        TempDir dir;

        const std::string previousDocument = readFixtureOrFail("valid.json");
        showmesh::writeFileAtomically(dir.programPath(), previousDocument);

        const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
            &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(),
            &clockBeforeExpiry);

        CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kMalformedEnvelope);
        CHECK(!showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
        CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
    }
}

TEST(FetchTreatsValidJsonThatIsNotASignedProgramAsMalformedAndDoesNotInstall) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse("{\"published\":true}");
    FixedCredentials credentials;
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    showmesh::writeFileAtomically(dir.programPath(), previousDocument);

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kMalformedEnvelope);
    CHECK(!showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(FetchTreatsAProgramThatIsNotAnObjectAsMalformedNeverAsAForgery) {
    // The bug this guards against: reaching VerifyFallbackProgram with a
    // "program" that is a string, not an object, gets refused THERE, and
    // an earlier version of this file acknowledged that refusal as
    // "signature-invalid": a forgery report for a forgery that never
    // happened, no signature ever having been checked. The outcome kind
    // alone does not catch a regression back to that: it must never be
    // acknowledgeable.
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse =
        okResponse("{\"published\":true,\"program\":\"not an object\",\"signatureBase64\":\"AAAA\"}");
    FixedCredentials credentials;
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    showmesh::writeFileAtomically(dir.programPath(), previousDocument);

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kMalformedEnvelope);
    CHECK(!showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(FetchTreatsANonStringSignatureBase64AsMalformedNeverAsAForgery) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse("{\"published\":true,\"program\":{\"packageId\":\"x\"},\"signatureBase64\":12345}");
    FixedCredentials credentials;
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    showmesh::writeFileAtomically(dir.programPath(), previousDocument);

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kMalformedEnvelope);
    CHECK(!showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(FetchTreatsANonJsonBodyAsMalformedAndDoesNotInstall) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse("this is not json at all");
    FixedCredentials credentials;
    TempDir dir;

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kMalformedEnvelope);
    CHECK(!showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
}

TEST(FetchTreatsAnEnormousBodyAsMalformedRatherThanCrashing) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    // A fake can hand back more than any real transport's own cap would
    // allow; this proves the fetch itself never assumes a bounded body,
    // even though CurlHttpTransport also enforces one in production.
    transport.getResponse = okResponse(std::string(2 * 1024 * 1024, 'a'));
    FixedCredentials credentials;
    TempDir dir;

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kMalformedEnvelope);
}

TEST(FetchNotPublishedIsAnOrdinaryOutcomeNotAFailure) {
    const std::string envelope = readFixtureOrFail("not-published-get-response.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse(envelope);
    FixedCredentials credentials;
    TempDir dir;

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kNotPublished);
    CHECK(!showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
}

TEST(FetchRefusesATamperedProgramAndReportsSignatureInvalid) {
    const std::string envelope = readFixtureOrFail("tampered-get-response.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse(envelope);
    FixedCredentials credentials;
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    showmesh::writeFileAtomically(dir.programPath(), previousDocument);

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kVerificationRefused);
    CHECK(showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
    CHECK_EQ(showmesh::fallback::FallbackFetchOutcomeVerificationResult(outcome.kind),
             std::string(showmesh::fallback::kVerificationResultSignatureInvalid));
    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(FetchRefusesAWrongKeyProgramAndReportsSignatureInvalid) {
    const std::string envelope = readFixtureOrFail("wrong-key-get-response.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse(envelope);
    FixedCredentials credentials;
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    showmesh::writeFileAtomically(dir.programPath(), previousDocument);

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kVerificationRefused);
    CHECK_EQ(showmesh::fallback::FallbackFetchOutcomeVerificationResult(outcome.kind),
             std::string(showmesh::fallback::kVerificationResultSignatureInvalid));
    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(FetchRefusesAProgramSignedForADifferentHostAsMismatchedNotSignatureInvalid) {
    const std::string envelope = readFixtureOrFail("valid-get-response.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse(envelope);
    FixedCredentials credentials;
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    showmesh::writeFileAtomically(dir.programPath(), previousDocument);

    // The fixture's own program.fppInstanceUuid is kExpectedInstanceUuid;
    // asking on behalf of a different host is exactly a coordinator (or
    // a proxy in front of one) serving the wrong host's program, valid
    // signature and all.
    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kOtherInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kInstanceMismatch);
    CHECK(showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
    // The signature was genuinely valid: reporting it as signature-invalid
    // would describe a forgery that never happened.
    CHECK_EQ(showmesh::fallback::FallbackFetchOutcomeVerificationResult(outcome.kind),
             std::string(showmesh::fallback::kVerificationResultMismatchedProgram));
    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(FetchRefusesAnExpiredProgramAsMismatchedNotSignatureInvalid) {
    const std::string envelope = readFixtureOrFail("valid-get-response.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse(envelope);
    FixedCredentials credentials;
    TempDir dir;

    const std::string previousDocument = readFixtureOrFail("valid.json");
    showmesh::writeFileAtomically(dir.programPath(), previousDocument);

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockAfterExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kExpired);
    CHECK(showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
    CHECK_EQ(showmesh::fallback::FallbackFetchOutcomeVerificationResult(outcome.kind),
             std::string(showmesh::fallback::kVerificationResultMismatchedProgram));
    CHECK_EQ(readRawOrFail(dir.programPath()), previousDocument);
}

TEST(FetchWithNoCredentialMakesNoNetworkCall) {
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    ScriptedTransport transport;
    transport.getResponse = okResponse(readFixtureOrFail("valid-get-response.json"));
    FixedCredentials credentials;
    credentials.available = false;
    TempDir dir;

    const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, publicKey, dir.programPath(), &clockBeforeExpiry);

    CHECK(outcome.kind == showmesh::fallback::FallbackFetchOutcomeKind::kCredentialUnavailable);
    CHECK(!showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome));
    CHECK_EQ(transport.getCalls.size(), static_cast<size_t>(0));
}

TEST(AcknowledgeSendsExactlyTheFourFieldsWithTheClosedVocabulary) {
    ScriptedTransport transport;
    transport.postResponse = statusResponse(200);
    FixedCredentials credentials;

    showmesh::fallback::FallbackFetchOutcome outcome;
    outcome.kind = showmesh::fallback::FallbackFetchOutcomeKind::kInstalled;
    outcome.packageId = "pkg-1";
    outcome.revision = "rev-1";

    const showmesh::fallback::AcknowledgeResult result = showmesh::fallback::AcknowledgeFallbackProgram(
        &transport, &credentials, kBaseUrl, kExpectedInstanceUuid, outcome, &clockBeforeExpiry);

    CHECK(result.ok);
    CHECK_EQ(transport.postCalls.size(), static_cast<size_t>(1));

    const showmesh::json::ParseResult sent = showmesh::json::parse(transport.postCalls[0].body);
    CHECK(sent.ok);
    CHECK_EQ(sent.value.members().size(), static_cast<size_t>(4));
    bool sawPackageId = false, sawRevision = false, sawResult = false, sawInstalledAt = false;
    for (const auto& member : sent.value.members()) {
        if (member.first == "packageId") {
            CHECK_EQ(member.second.string(), std::string("pkg-1"));
            sawPackageId = true;
        } else if (member.first == "revision") {
            CHECK_EQ(member.second.string(), std::string("rev-1"));
            sawRevision = true;
        } else if (member.first == "verificationResult") {
            CHECK_EQ(member.second.string(), std::string("verified"));
            sawResult = true;
        } else if (member.first == "installedAt") {
            CHECK_EQ(member.second.string(), std::string("2026-09-08T10:00:00Z"));
            sawInstalledAt = true;
        }
    }
    CHECK(sawPackageId);
    CHECK(sawRevision);
    CHECK(sawResult);
    CHECK(sawInstalledAt);
}
