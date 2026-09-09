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
#include <sys/stat.h>
#include <unistd.h>

#include "callback_fields.h"
#include "check.h"
#include "fallback_activation_resolver.h"
#include "fallback_pinned_key_loader.h"
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

// --- fallback activation resolver --------------------------------------
//
// resolver-edge-cases.json (see fixtures/fallback/README.md and
// generate_fixtures.go's buildResolverEdgeCasesProgram) is a validly
// signed program the coordinator's own compiler would never emit, but
// whose wire format does not forbid: two entries sharing an entryKey, an
// entry with an empty targets list, and an entry whose one target names
// neither render nor audio. It exists only so this resolver, which must
// treat the installed file as untrusted input, is exercised against
// exactly those shapes rather than only against what a healthy
// coordinator happens to produce today.

namespace {

std::chrono::system_clock::time_point timeFromMillis(showmesh::TimeMillis millis) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(millis));
}

std::string stringMember(const showmesh::json::Value& object, const char* name) {
    for (const auto& member : object.members()) {
        if (member.first == name) return member.second.string();
    }
    CHECK(false);
    return "";
}

}  // namespace

TEST(NoInstalledProgramIsRefusedAsNoProgram) {
    TempDir dir;
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::ActivationResolution resolution = showmesh::fallback::ResolveInstalledActivation(
        "entry-0", dir.programPath(), publicKey, timeFromMillis(clockBeforeExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kNoProgramInstalled);
    CHECK(!resolution.reason.empty());
    CHECK(!resolution.match.has_value());
}

TEST(InstalledFileThatNoLongerVerifiesIsRefusedNotTrusted) {
    // The file on disk is exactly tampered-one-byte.json: a document
    // this process never verified and installed itself. Simulates disk
    // content changing after InstallFallbackProgram last wrote it.
    TempDir dir;
    const std::string tampered = readFixtureOrFail("tampered-one-byte.json");
    std::ofstream out(dir.programPath(), std::ios::binary);
    out << tampered;
    out.close();
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::ActivationResolution resolution = showmesh::fallback::ResolveInstalledActivation(
        "entry-0", dir.programPath(), publicKey, timeFromMillis(clockBeforeExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kProgramFailedReverification);
    CHECK(!resolution.reason.empty());
    CHECK(!resolution.match.has_value());
}

TEST(ExpiredInstalledProgramIsRefusedAsExpiredNotAsAMatch) {
    TempDir dir;
    const std::string document = readFixtureOrFail("valid.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");
    const showmesh::fallback::FallbackVerifyResult verified =
        showmesh::fallback::VerifyFallbackProgram(document, publicKey);
    CHECK(showmesh::fallback::InstallFallbackProgram(*verified.program, dir.programPath()).ok);

    const showmesh::fallback::ActivationResolution resolution = showmesh::fallback::ResolveInstalledActivation(
        "entry-0", dir.programPath(), publicKey, timeFromMillis(clockAfterExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kProgramExpired);
    CHECK(!resolution.reason.empty());
    CHECK(!resolution.match.has_value());
}

// A key this program never covers, and a key belonging to a different
// program's show entirely, reach the identical outcome for the identical
// reason: deriveEntryKey hashes playlist name/definition/section/position
// together, so a key from one show structurally cannot appear in another
// show's entries. There is no separate "wrong show" branch in the
// resolver to test, because none exists.
TEST(KeyNotCoveredByTheCurrentProgramIsUnknownEntry) {
    const std::string document = readFixtureOrFail("valid.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::ActivationResolution resolution = showmesh::fallback::ResolveActivationFromDocument(
        "entry-from-a-different-playlist", document, publicKey, timeFromMillis(clockBeforeExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kUnknownEntry);
    CHECK(!resolution.reason.empty());
    CHECK(!resolution.match.has_value());
}

TEST(KeyBelongingToADifferentShowsProgramIsUnknownEntryNotAMatch) {
    // valid.json's own covered key, looked up against
    // resolver-edge-cases.json (a different show, different packageId).
    // Never resolves to valid.json's cue-a: a resolver that fell back to
    // "close enough" here would be exactly the "different Cue" substitution
    // ADR-048 forbids.
    const std::string document = readFixtureOrFail("resolver-edge-cases.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::ActivationResolution resolution =
        showmesh::fallback::ResolveActivationFromDocument("entry-0", document, publicKey, timeFromMillis(clockBeforeExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kUnknownEntry);
    CHECK(!resolution.match.has_value());
}

TEST(EntryKeyMatchingTwoMappingsIsAmbiguousNotAGuess) {
    const std::string document = readFixtureOrFail("resolver-edge-cases.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::ActivationResolution resolution = showmesh::fallback::ResolveActivationFromDocument(
        "entry-dup", document, publicKey, timeFromMillis(clockBeforeExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kAmbiguousEntry);
    CHECK(!resolution.reason.empty());
    CHECK(!resolution.match.has_value());
}

TEST(EmptyTargetsListIsRefusedNeverAMatchWithNothingToSend) {
    const std::string document = readFixtureOrFail("resolver-edge-cases.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::ActivationResolution resolution = showmesh::fallback::ResolveActivationFromDocument(
        "entry-empty", document, publicKey, timeFromMillis(clockBeforeExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kNoActivatableTarget);
    CHECK(!resolution.reason.empty());
    CHECK(!resolution.match.has_value());
}

// A one-target list whose only target is inert is the empty-targets
// failure wearing a different length: a match here would read as
// success to every caller above this resolver while nothing happens on
// any node, identically to EmptyTargetsListIsRefusedNeverAMatchWithNothingToSend
// above. Refused the same way, as kNoActivatableTarget.
TEST(SoleInertTargetIsRefusedNeverAMatchWithNothingToSend) {
    const std::string document = readFixtureOrFail("resolver-edge-cases.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::ActivationResolution resolution = showmesh::fallback::ResolveActivationFromDocument(
        "entry-no-activation", document, publicKey, timeFromMillis(clockBeforeExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kNoActivatableTarget);
    CHECK(!resolution.reason.empty());
    CHECK(!resolution.match.has_value());
}

// An inert target ALONGSIDE a live one is a real match: node-c still
// gets activated, and node-d's inertness is copied through verbatim
// rather than filtered out or turned into a refusal. Only a match where
// NOT ONE target can do anything is refused (the two tests above).
TEST(InertTargetAlongsideALiveOneSurvivesAsAMatch) {
    const std::string document = readFixtureOrFail("resolver-edge-cases.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::ActivationResolution resolution = showmesh::fallback::ResolveActivationFromDocument(
        "entry-mixed-activation", document, publicKey, timeFromMillis(clockBeforeExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kMatch);
    CHECK(resolution.match.has_value());
    CHECK_EQ(resolution.match->targets().size(), static_cast<size_t>(2));

    const showmesh::fallback::ActivationTarget& live = resolution.match->targets()[0];
    CHECK_EQ(live.nodeId, std::string("node-c"));
    CHECK(live.render.has_value());
    CHECK_EQ(stringMember(*live.render, "sequence"), std::string("seq-c"));

    const showmesh::fallback::ActivationTarget& inert = resolution.match->targets()[1];
    CHECK_EQ(inert.nodeId, std::string("node-d"));
    CHECK(!inert.render.has_value());
    CHECK(!inert.audio.has_value());
}

TEST(ValidMatchCopiesCueRevisionAndTargetsVerbatim) {
    const std::string document = readFixtureOrFail("valid.json");
    const std::vector<uint8_t> publicKey = fixturePublicKey("coordinatorPublicKeyBase64");

    const showmesh::fallback::ActivationResolution resolution = showmesh::fallback::ResolveActivationFromDocument(
        "entry-0", document, publicKey, timeFromMillis(clockBeforeExpiry()));

    CHECK(resolution.kind == showmesh::fallback::ActivationResolveKind::kMatch);
    CHECK(resolution.match.has_value());
    CHECK_EQ(resolution.match->packageId(), std::string(kValidPackageId));
    CHECK_EQ(resolution.match->revision(), std::string(kValidRevision));
    CHECK_EQ(resolution.match->fppInstanceUuid(), std::string(kExpectedInstanceUuid));
    CHECK_EQ(resolution.match->entryKey(), std::string("entry-0"));
    CHECK_EQ(resolution.match->cueId(), std::string("cue-a"));
    CHECK_EQ(resolution.match->cueRevision(), static_cast<std::int64_t>(3));
    CHECK(resolution.match->generation().has_value());
    CHECK_EQ(*resolution.match->generation(), static_cast<std::int64_t>(1));
    CHECK_EQ(resolution.match->targets().size(), static_cast<size_t>(1));

    const showmesh::fallback::ActivationTarget& target = resolution.match->targets()[0];
    CHECK_EQ(target.nodeId, std::string("node-a"));
    CHECK(target.render.has_value());
    CHECK(!target.audio.has_value());
    CHECK_EQ(stringMember(*target.render, "sequence"), std::string("seq-a"));
    CHECK_EQ(stringMember(*target.render, "filename"), std::string("seq-a.fseq"));
}

TEST(FallbackFetchOutcomeKindNameIsTheEnumsOwnSpelling) {
    using showmesh::fallback::FallbackFetchOutcomeKind;
    using showmesh::fallback::FallbackFetchOutcomeKindName;
    CHECK_EQ(std::string(FallbackFetchOutcomeKindName(FallbackFetchOutcomeKind::kCredentialUnavailable)),
             std::string("kCredentialUnavailable"));
    CHECK_EQ(std::string(FallbackFetchOutcomeKindName(FallbackFetchOutcomeKind::kInstalled)),
             std::string("kInstalled"));
    CHECK_EQ(std::string(FallbackFetchOutcomeKindName(FallbackFetchOutcomeKind::kNotPublished)),
             std::string("kNotPublished"));
}

TEST(ActivationResolveKindNameIsTheEnumsOwnSpelling) {
    using showmesh::fallback::ActivationResolveKind;
    using showmesh::fallback::ActivationResolveKindName;
    CHECK_EQ(std::string(ActivationResolveKindName(ActivationResolveKind::kNoProgramInstalled)),
             std::string("kNoProgramInstalled"));
    CHECK_EQ(std::string(ActivationResolveKindName(ActivationResolveKind::kProgramFailedReverification)),
             std::string("kProgramFailedReverification"));
    CHECK_EQ(std::string(ActivationResolveKindName(ActivationResolveKind::kProgramExpired)),
             std::string("kProgramExpired"));
    CHECK_EQ(std::string(ActivationResolveKindName(ActivationResolveKind::kUnknownEntry)),
             std::string("kUnknownEntry"));
    CHECK_EQ(std::string(ActivationResolveKindName(ActivationResolveKind::kAmbiguousEntry)),
             std::string("kAmbiguousEntry"));
    CHECK_EQ(std::string(ActivationResolveKindName(ActivationResolveKind::kNoActivatableTarget)),
             std::string("kNoActivatableTarget"));
    CHECK_EQ(std::string(ActivationResolveKindName(ActivationResolveKind::kMatch)), std::string("kMatch"));
}

// --- pinned coordinator public key loader -------------------------------
//
// The directory check runs before the file check, so every branch that
// requires the DIRECTORY to already be root-owned before the file itself
// is examined (a tighter-than-usual file mode that must still load, a
// group- or other-writable file that must still be refused, or malformed
// content once both ownership checks pass) requires a root-owned
// directory to reach at all. That is not exercisable by this
// unprivileged test process, nor by an unprivileged CI runner: a test
// cannot chown a directory to root without root. Those branches were
// verified manually, as root, inside the same Docker container used for
// the fpp10 adapter build, recorded in this change's pull request body.
// This file automatically covers the two shapes reachable without
// privilege: a directory that does not exist at all (kMissing, since
// stat() on the directory fails before anything else is examined), and
// an existing directory this test process itself owns (kOwnershipUntrusted,
// the exact "agent's own account could have written it" case ADR-025
// decision 4 is actually about, reached at the directory level here
// rather than the file level, but the identical status and the identical
// refusal).

namespace {

class TempKeyDir {
 public:
    TempKeyDir() {
        char buffer[] = "/tmp/showmesh-pinned-key-test-XXXXXX";
        const char* made = ::mkdtemp(buffer);
        CHECK(made != nullptr);
        path_ = made != nullptr ? std::string(made) : std::string();
    }
    ~TempKeyDir() {
        std::remove((path_ + "/coordinator-fallback-public-key").c_str());
        ::rmdir(path_.c_str());
    }
    const std::string& path() const { return path_; }

 private:
    std::string path_;
};

}  // namespace

TEST(MissingPinnedKeyDirectoryIsReportedAsMissingNotUntrusted) {
    TempKeyDir dir;
    const std::string neverCreated = dir.path() + "/never-created";
    const showmesh::fallback::PinnedKeyLoadResult result =
        showmesh::fallback::LoadPinnedCoordinatorPublicKey(neverCreated);
    CHECK(result.status == showmesh::fallback::PinnedKeyLoadStatus::kMissing);
    CHECK(!result.error.empty());
    CHECK_EQ(result.publicKey.size(), static_cast<size_t>(0));
}

// Depends on this test process NOT being root: a directory it creates
// itself is then owned by its own uid, the exact "agent's own account
// could have written it" case ADR-025 decision 4 refuses, caught here
// before the loader ever looks for the file inside, regardless of
// whether that file exists, what it contains, or what mode it carries.
// Run as root (this repository's own manual verification container
// included, since a container's default user is root unless told
// otherwise) the assumption is false: root owns the temp directory, so
// the same steps would instead exercise kLoaded, a different test
// entirely. Skipped rather than faked when euid is 0, the identical
// "state the gap plainly, do not report a pass that was not earned"
// choice locale_guard_test.cpp already makes for a locale this host does
// not have installed.
TEST(APresentDirectoryNotOwnedByRootIsRefusedAsUntrustedBeforeCheckingTheFile) {
    if (::geteuid() == 0) {
        std::fprintf(stderr,
                      "SKIP APresentDirectoryNotOwnedByRootIsRefusedAsUntrustedBeforeCheckingTheFile: "
                      "running as root, so a directory this test creates is root-owned and the "
                      "not-owned-by-root assumption does not hold here\n");
        return;
    }

    TempKeyDir dir;
    const std::string path = dir.path() + "/coordinator-fallback-public-key";
    std::ofstream out(path, std::ios::binary);
    out << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    out.close();
    ::chmod(path.c_str(), 0644);

    const showmesh::fallback::PinnedKeyLoadResult result =
        showmesh::fallback::LoadPinnedCoordinatorPublicKey(dir.path());
    CHECK(result.status == showmesh::fallback::PinnedKeyLoadStatus::kOwnershipUntrusted);
    CHECK(!result.error.empty());
    CHECK_EQ(result.publicKey.size(), static_cast<size_t>(0));
}

TEST(PinnedKeyLoadStatusNameIsTheEnumsOwnSpelling) {
    using showmesh::fallback::PinnedKeyLoadStatus;
    using showmesh::fallback::PinnedKeyLoadStatusName;
    CHECK_EQ(std::string(PinnedKeyLoadStatusName(PinnedKeyLoadStatus::kMissing)), std::string("kMissing"));
    CHECK_EQ(std::string(PinnedKeyLoadStatusName(PinnedKeyLoadStatus::kOwnershipUntrusted)),
             std::string("kOwnershipUntrusted"));
    CHECK_EQ(std::string(PinnedKeyLoadStatusName(PinnedKeyLoadStatus::kMalformed)), std::string("kMalformed"));
    CHECK_EQ(std::string(PinnedKeyLoadStatusName(PinnedKeyLoadStatus::kLoaded)), std::string("kLoaded"));
}
