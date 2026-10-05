// Tests for the ADR-048 fallback executor: the executor key, the outage
// detector, the refetch cadence, and delivery to nodes per contract section
// 5. Delivery is driven through ShowMeshRuntime::observeCallback(), the entry
// point both shipping adapters forward FPP's playlistCallback into.

#include <sys/stat.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "fallback_executor.h"
#include "showmesh/playlist_identity.h"
#include "showmesh/runtime.h"

using showmesh::HttpRequest;
using showmesh::HttpResponse;
using showmesh::PlaylistEntryObservation;
using showmesh::ShowMeshRuntime;
using showmesh::TimeMillis;
using namespace showmesh::fallback;

namespace {

const char* kActivationFixtureDir = "shared/tests/fixtures/fallback-activation";
const char* kCoordinatorUrl = "http://coordinator.invalid:8080";
const char* kFppUuid = "22222222-2222-4222-8222-222222222222";
const char* kPlaylistName = "Main Show";
const char* kPlaylistDefinition =
    "{\"name\":\"Main Show\",\"mainPlaylist\":[{\"sequenceName\":\"a.fseq\"},{\"sequenceName\":\"b.fseq\"},"
    "{\"sequenceName\":\"c.fseq\"}]}";
// RFC 8032 section 7.1 test 3, the seed the fixture programs are signed under.
const char* kCoordinatorTestSeedHex = "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7";
const char* kNodeA = "192.0.2.21:80";
const char* kNodeB = "192.0.2.22:80";

// 2026-10-05T12:00:00Z, the fixture programs' compiledAt. They expire at 12:15.
constexpr TimeMillis kCompiledAtMillis = 1791201600000;
constexpr TimeMillis kFiveMinutesMillis = 300000;

TimeMillis gClock = kCompiledAtMillis + kFiveMinutesMillis;
TimeMillis fixtureClock() { return gClock; }

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string fixture(const char* name) { return readFile(std::string(kActivationFixtureDir) + "/" + name); }

showmesh::json::Value parseJson(const std::string& text) {
    const showmesh::json::ParseResult parsed = showmesh::json::parse(text);
    CHECK(parsed.ok);
    return parsed.value;
}

std::string member(const showmesh::json::Value& object, const char* name) {
    const showmesh::json::Value* v = detail::findMember(object, name);
    return v != nullptr && v->type() == showmesh::json::Type::kString ? v->string() : std::string();
}

const showmesh::json::Value& child(const showmesh::json::Value& object, const char* name) {
    static const showmesh::json::Value missing;
    const showmesh::json::Value* v = detail::findMember(object, name);
    return v != nullptr ? *v : missing;
}

ExecutorKey keyFromHex(const std::string& seedHex) {
    std::vector<uint8_t> seed;
    CHECK(hexDecode(seedHex, &seed));
    ExecutorKey key;
    CHECK(executorKeyFromSeed(seed, &key));
    return key;
}

ExecutorKey fixtureExecutorKey() { return keyFromHex(member(parseJson(fixture("keys.json")), "executorSeedHex")); }

PinnedKeyLoadResult fixturePinnedKey() {
    PinnedKeyLoadResult result;
    result.status = PinnedKeyLoadStatus::kLoaded;
    CHECK(detail::base64Decode(member(parseJson(fixture("keys.json")), "coordinatorPublicKey"), &result.publicKey));
    return result;
}

// Key creation asks for 32 bytes and gets the fixture executor seed, so the
// created key is the one the fixture programs enroll. Execution ids ask for 16.
int gExecutionIdCounter = 0;
bool fixtureRandom(std::uint8_t* out, std::size_t count) {
    if (count == kEd25519SeedBytes) {
        const ExecutorKey key = fixtureExecutorKey();
        for (std::size_t i = 0; i < count; ++i) out[i] = key.seed[i];
        return true;
    }
    ++gExecutionIdCounter;
    for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<std::uint8_t>(gExecutionIdCounter + i);
    return true;
}

struct ProgramTarget {
    std::string nodeId;
    std::string address;  // empty leaves the member out
};

struct ProgramEntry {
    std::string entryKey;
    std::string cueId;
    int cueRevision = 1;
    std::vector<ProgramTarget> targets;
};

struct ProgramSpec {
    std::vector<ProgramEntry> entries;
    std::string executorPublicKey = fixtureExecutorKey().publicKeyBase64;  // empty leaves the member out
    std::string compiledAt = "2026-10-05T12:00:00Z";
    std::string expiresAt = "2026-10-05T12:15:00Z";
    std::string packageId = "pkg-test";
    std::string revision = "rev-test";
};

// A program document signed the way the coordinator signs one, under the
// published test seed, for shapes the copied fixtures do not carry.
std::string signedProgram(const ProgramSpec& spec) {
    std::ostringstream program;
    program << "{\"catalogRevisions\":{\"node-a\":\"cat-rev-1\",\"node-b\":\"cat-rev-b\"},\"compiledAt\":\""
            << spec.compiledAt << "\",\"entries\":[";
    for (std::size_t e = 0; e < spec.entries.size(); ++e) {
        const ProgramEntry& entry = spec.entries[e];
        program << (e == 0 ? "" : ",") << "{\"cueId\":\"" << entry.cueId << "\",\"cueRevision\":" << entry.cueRevision
                << ",\"entryKey\":\"" << entry.entryKey << "\",\"targets\":[";
        for (std::size_t t = 0; t < entry.targets.size(); ++t) {
            const ProgramTarget& target = entry.targets[t];
            program << (t == 0 ? "" : ",") << "{";
            if (!target.address.empty()) program << "\"address\":\"" << target.address << "\",";
            program << "\"nodeId\":\"" << target.nodeId << "\",\"render\":{\"sequence\":\"seq\"}}";
        }
        program << "]}";
    }
    program << "],";
    if (!spec.executorPublicKey.empty()) program << "\"executorPublicKey\":\"" << spec.executorPublicKey << "\",";
    program << "\"expiresAt\":\"" << spec.expiresAt << "\",\"fppInstanceUuid\":\"" << kFppUuid
            << "\",\"generation\":7,\"packageId\":\"" << spec.packageId << "\",\"revision\":\"" << spec.revision
            << "\",\"schemaVersion\":1}";
    const showmesh::json::CanonicalResult canonical = showmesh::json::canonicalize(program.str());
    CHECK(canonical.ok);
    std::string signature;
    CHECK(signWithExecutorKey(keyFromHex(kCoordinatorTestSeedHex), canonical.text, &signature));
    return "{\"program\":" + program.str() + ",\"signature\":\"" + signature + "\"}";
}

// The coordinator's GET envelope around a signed document.
std::string getEnvelope(const std::string& signedDocument) {
    std::string rawProgram;
    CHECK(detail::extractRawRootMember(signedDocument, "program", &rawProgram));
    return "{\"published\":true,\"program\":" + rawProgram + ",\"signatureBase64\":\"" +
           member(parseJson(signedDocument), "signature") + "\"}";
}

HttpResponse answer(int status, const std::string& body) {
    HttpResponse r;
    r.transportOk = true;
    r.statusCode = status;
    r.body = body;
    return r;
}

HttpResponse nodeAnswer(int status, const std::string& outcome, const std::string& extraMembers = "") {
    return answer(status, std::string("{\"accepted\":") + (outcome == "authorized" || outcome == "installed" ? "true"
                                                                                                             : "false") +
                              ",\"outcome\":\"" + outcome + "\",\"reason\":\"node says " + outcome + "\"" +
                              extraMembers + "}");
}

HttpResponse noResponse() {
    HttpResponse r;
    r.error = "connection refused";
    return r;
}

struct Sent {
    std::string method;
    std::string url;
    std::string body;
    std::string bearerToken;
    int timeoutMillis = 0;
};

// A coordinator and any number of nodes behind one transport. Thread safe:
// the executor delivers to several nodes at once.
class FakeNetwork : public showmesh::HttpTransport {
 public:
    HttpResponse get(const HttpRequest& request) override { return handle("GET", request); }
    HttpResponse post(const HttpRequest& request) override { return handle("POST", request); }
    HttpResponse put(const HttpRequest& request) override { return handle("PUT", request); }

    std::vector<Sent> sent(const std::string& method, const std::string& urlPart) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Sent> matching;
        for (const Sent& s : sent_) {
            if (s.method == method && s.url.find(urlPart) != std::string::npos) matching.push_back(s);
        }
        return matching;
    }
    std::size_t count(const std::string& method, const std::string& urlPart) { return sent(method, urlPart).size(); }

    void scriptActivation(const std::string& address, HttpResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        activationScript_[address].push_back(std::move(response));
    }

    bool coordinatorUp = true;
    int healthStatus = 200;
    int registrationStatus = 200;
    bool registrationChanged = true;
    std::string registrationRefusalBody = "{\"title\":\"Forbidden\",\"detail\":\"the coordinator says why\"}";
    std::string programEnvelope = "{\"published\":false}";
    std::set<std::string> nodesDown;

 private:
    HttpResponse handle(const std::string& method, const HttpRequest& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_.push_back(Sent{method, request.url, request.body, request.bearerToken, request.timeoutMillis});
        const std::string coordinator = kCoordinatorUrl;
        if (request.url.compare(0, coordinator.size(), coordinator) == 0) {
            if (!coordinatorUp) return noResponse();
            const std::string path = request.url.substr(coordinator.size());
            if (path == "/healthz") return answer(healthStatus, "ok");
            if (path.find("/executor-key") != std::string::npos) {
                if (registrationStatus != 200) return answer(registrationStatus, registrationRefusalBody);
                return answer(200, std::string("{\"changed\":") + (registrationChanged ? "true" : "false") + "}");
            }
            if (path.find("/acknowledge") != std::string::npos) return answer(200, "{}");
            return answer(200, programEnvelope);
        }
        const std::string address = request.url.substr(7, request.url.find('/', 7) - 7);
        if (nodesDown.count(address) != 0) return noResponse();
        if (method == "PUT") return nodeAnswer(200, "installed");
        std::deque<HttpResponse>& script = activationScript_[address];
        if (script.empty()) return nodeAnswer(200, "authorized");
        HttpResponse next = script.front();
        script.pop_front();
        return next;
    }

    std::mutex mutex_;
    std::vector<Sent> sent_;
    std::map<std::string, std::deque<HttpResponse>> activationScript_;
};

class RecordingNotifier : public FallbackStateNotifier {
 public:
    void raise(const std::string& message) override { raised.push_back(message); }
    void clear(const std::string& message) override { cleared.push_back(message); }
    std::vector<std::string> raised;
    std::vector<std::string> cleared;
};

class FixedDefinitions : public showmesh::PlaylistDefinitionSource {
 public:
    std::string definitionFor(const std::string&) override { return definition; }
    std::string instanceUuid() override { return kFppUuid; }
    std::string definition = kPlaylistDefinition;
};

class CountingSink : public showmesh::ObservationSink {
 public:
    bool publish(const PlaylistEntryObservation& observation) override {
        published.push_back(observation);
        return true;
    }
    bool publishUnavailable(const PlaylistEntryObservation& observation) override {
        published.push_back(observation);
        return true;
    }
    std::vector<PlaylistEntryObservation> published;
};

// One FPP host on a bench: its directories, its network, the executor, and
// the runtime the shipping adapters drive it through.
class Bench {
 public:
    explicit Bench(bool paired = true) {
        gClock = kCompiledAtMillis + kFiveMinutesMillis;
        char buffer[] = "/tmp/showmesh-fallback-executor-XXXXXX";
        root_ = ::mkdtemp(buffer);
        std::filesystem::create_directories(stateDir());
        std::filesystem::create_directories(credentialDir());
        writeFile(stateDir() + "/config.json", std::string("{\"coordinatorUrl\":\"") + kCoordinatorUrl + "\"}");
        if (paired) pair("token-one");
        makeExecutor();
    }

    ~Bench() {
        runtime.reset();
        executor.reset();
        std::filesystem::remove_all(root_);
    }

    std::string stateDir() const { return root_ + "/state"; }
    std::string credentialDir() const { return root_ + "/credential"; }
    std::string installPath() const { return root_ + "/state/fallback-program.json"; }
    std::string keyPath() const { return credentialDir() + "/" + kExecutorKeyFilename; }

    void writeFile(const std::string& path, const std::string& contents) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
    }

    void pair(const std::string& token) {
        writeFile(credentialDir() + "/credential", token);
        ::chmod((credentialDir() + "/credential").c_str(), 0600);
    }

    // A fresh executor over the same directories: a plugin restart.
    void makeExecutor() {
        runtime.reset();
        executor.reset();
        FallbackExecutorOptions options;
        options.clock = fixtureClock;
        options.fppInstanceUuid = kFppUuid;
        options.stateDir = stateDir();
        options.credentialDir = credentialDir();
        options.installPath = installPath();
        options.pinnedKey = fixturePinnedKey();
        options.randomBytes = fixtureRandom;
        options.notifier = &notifier;
        options.log = [this](bool, const std::string& line) { logs.push_back(line); };
        options.pause = [this](int millis) {
            pauses.push_back(millis);
            return true;
        };
        executor = std::make_unique<FallbackExecutor>(options, &network);
        runtime = std::make_unique<ShowMeshRuntime>(&definitions, &sink, fixtureClock, nullptr, nullptr, nullptr,
                                                    showmesh::kDefaultSafeCeilingPercent, executor.get());
    }

    void tick() { executor->tick(gClock); }

    void advanceAndTick(TimeMillis millis) {
        gClock += millis;
        tick();
    }

    // Probes fail until the default detector confirms loss: four probes, 15 seconds.
    void loseCoordinator() {
        network.coordinatorUp = false;
        for (int i = 0; i < 4; ++i) advanceAndTick(kHypothesisProbeIntervalMillis);
        CHECK(executor->coordinatorLost());
    }

    std::string entryKey(int position) const {
        return showmesh::resolveEntryIdentity(kFppUuid, kPlaylistName, kPlaylistDefinition, "mainPlaylist", position)
            .entryKey;
    }

    // One playlistCallback, through the runtime entry point the adapters use.
    void callback(const char* action, int position, std::optional<int> loop = std::nullopt,
                  const char* playlistName = kPlaylistName) {
        runtime->observeCallback(playlistName, action, "mainPlaylist", position, "a.fseq", "", loop);
        CHECK(runtime->drainOnce());
    }

    std::vector<Sent> activations() { return network.sent("POST", kNodeActivationPath); }

    FallbackRecord lastActivationRecord() {
        const std::vector<FallbackRecord> records = activationRecords();
        CHECK(!records.empty());
        return records.empty() ? FallbackRecord() : records.back();
    }

    std::vector<FallbackRecord> activationRecords() {
        std::vector<FallbackRecord> records;
        for (const FallbackRecord& r : executor->status().recent) {
            if (r.kind == "activation") records.push_back(r);
        }
        return records;
    }

    FakeNetwork network;
    RecordingNotifier notifier;
    FixedDefinitions definitions;
    CountingSink sink;
    std::vector<std::string> logs;
    std::vector<int> pauses;
    std::unique_ptr<FallbackExecutor> executor;
    std::unique_ptr<ShowMeshRuntime> runtime;

 private:
    std::string root_;
};

ProgramSpec twoEntryProgram(const Bench& bench) {
    ProgramSpec spec;
    spec.entries = {{bench.entryKey(0), "cue-a", 3, {{"node-a", kNodeA}}},
                    {bench.entryKey(1), "cue-b", 5, {{"node-a", kNodeA}}}};
    return spec;
}

std::string requestMember(const Sent& activation, const char* name) {
    return member(child(parseJson(activation.body), "request"), name);
}

}  // namespace

// --- signing ---------------------------------------------------------------

TEST(SignerProducesTheFixtureSignatureForTheValidRequestsCanonicalBytes) {
    const showmesh::json::Value cases = parseJson(fixture("cases.json"));
    const showmesh::json::Value& valid = child(cases, "validRequest");
    const ExecutorKey key = fixtureExecutorKey();
    CHECK_EQ(key.publicKeyBase64, member(parseJson(fixture("keys.json")), "executorPublicKey"));

    std::string signature;
    CHECK(signWithExecutorKey(key, member(valid, "canonical"), &signature));
    CHECK_EQ(signature, member(valid, "signature"));
}

TEST(RequestBuiltFromTheFixtureProgramIsTheFixturesCanonicalBytesAndBody) {
    const showmesh::json::Value cases = parseJson(fixture("cases.json"));
    const auto now = std::chrono::system_clock::time_point(std::chrono::milliseconds(gClock));
    const ActivationResolution resolution =
        ResolveActivationFromDocument("entry-0", fixture("program.json"), fixturePinnedKey().publicKey, now);
    CHECK(resolution.kind == ActivationResolveKind::kMatch);
    const ActivationTarget& nodeA = resolution.match->targets()[0];
    CHECK_EQ(nodeA.nodeId, std::string("node-a"));
    CHECK_EQ(nodeA.address.value_or(""), std::string(kNodeA));

    const ActivationRequestBuild build =
        BuildActivationRequest(*resolution.match, nodeA, "00000000-0000-4000-8000-000000000001");
    CHECK(build.ok);
    CHECK_EQ(build.canonical, member(child(cases, "validRequest"), "canonical"));

    std::string signature;
    CHECK(signWithExecutorKey(fixtureExecutorKey(), build.canonical, &signature));
    const showmesh::json::Value& validCase = child(cases, "activations").items()[0];
    CHECK_EQ(ActivationRequestBody(build.canonical, signature), member(validCase, "body"));
}

TEST(TheCoordinatorTestSeedIsTheKeyTheFixtureProgramsVerifyUnder) {
    CHECK_EQ(keyFromHex(kCoordinatorTestSeedHex).publicKeyBase64,
             member(parseJson(fixture("keys.json")), "coordinatorPublicKey"));
}

TEST(ExecutionIdIsA36CharacterLowercaseVersion4Uuid) {
    std::uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) bytes[i] = static_cast<std::uint8_t>(0xF0 + i);
    CHECK_EQ(formatExecutionId(bytes), std::string("f0f1f2f3-f4f5-46f7-b8f9-fafbfcfdfeff"));
}

// --- executor key: creation and registration -------------------------------

TEST(NoKeyPairIsCreatedBeforeThePluginIsPaired) {
    Bench bench(/*paired=*/false);
    bench.tick();
    CHECK(!std::filesystem::exists(bench.keyPath()));
    CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(0));
}

TEST(PairingCreatesTheKeyBesideTheTokenWithTheTokensModeAndRegistersIt) {
    Bench bench(/*paired=*/false);
    bench.tick();
    bench.pair("token-one");
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);

    struct ::stat info {};
    CHECK(::stat(bench.keyPath().c_str(), &info) == 0);
    CHECK_EQ(static_cast<int>(info.st_mode & 07777), 0600);

    const std::vector<Sent> registrations = bench.network.sent("PUT", "/executor-key");
    CHECK_EQ(registrations.size(), static_cast<std::size_t>(1));
    CHECK_EQ(registrations[0].url,
             std::string(kCoordinatorUrl) + "/api/v1/fallback-programs/" + kFppUuid + "/executor-key");
    CHECK_EQ(registrations[0].bearerToken, std::string("token-one"));
    CHECK_EQ(registrations[0].body, "{\"publicKey\":\"" + fixtureExecutorKey().publicKeyBase64 + "\"}");
    CHECK(bench.executor->status().executorKeyRegistered);
}

TEST(AnAlreadyPairedPluginWithNoKeyCreatesOneAndRegistersOnItsNextStart) {
    Bench bench;
    CHECK(!std::filesystem::exists(bench.keyPath()));
    bench.tick();
    CHECK(std::filesystem::exists(bench.keyPath()));
    CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(1));
    // Nothing asked for a new pairing.
    CHECK_EQ(bench.network.count("POST", "/pairing/"), static_cast<std::size_t>(0));
}

TEST(ARestartKeepsTheSameKeyAndRegistersItAgain) {
    Bench bench;
    bench.tick();
    const std::string keyOnDisk = readFile(bench.keyPath());

    bench.makeExecutor();
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);

    CHECK_EQ(readFile(bench.keyPath()), keyOnDisk);
    const std::vector<Sent> registrations = bench.network.sent("PUT", "/executor-key");
    CHECK_EQ(registrations.size(), static_cast<std::size_t>(2));
    CHECK_EQ(registrations[0].body, registrations[1].body);
}

TEST(PairingAgainRegistersTheSameKeyUnderTheNewToken) {
    Bench bench;
    bench.tick();
    const std::string keyOnDisk = readFile(bench.keyPath());

    bench.pair("token-two");
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);

    CHECK_EQ(readFile(bench.keyPath()), keyOnDisk);
    const std::vector<Sent> registrations = bench.network.sent("PUT", "/executor-key");
    CHECK_EQ(registrations.size(), static_cast<std::size_t>(2));
    CHECK_EQ(registrations[1].bearerToken, std::string("token-two"));
    CHECK_EQ(registrations[1].body, registrations[0].body);
}

TEST(AnyRefusedRegistrationLeavesTheKeyUnregisteredShowsTheCoordinatorsReasonAndIsTriedAgainAtTheNextFetch) {
    for (int status : {403, 409, 500}) {
        Bench bench;
        bench.network.registrationStatus = status;
        bench.tick();
        CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(1));
        CHECK(!bench.executor->status().executorKeyRegistered);
        CHECK_EQ(bench.executor->status().executorKeyRegistrationProblem,
                 "The coordinator answered " + std::to_string(status) + ": the coordinator says why");
        const showmesh::json::Value file = parseJson(readFile(bench.stateDir() + "/" + kFallbackStatusFilename));
        CHECK(member(file, "executorKeyRegistrationProblem").find("the coordinator says why") != std::string::npos);

        bench.advanceAndTick(kHypothesisProbeIntervalMillis);
        CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(1));

        bench.network.registrationStatus = 200;
        bench.advanceAndTick(kRefetchRetryMillis);
        CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(2));
        CHECK(bench.executor->status().executorKeyRegistered);
        CHECK(bench.executor->status().executorKeyRegistrationProblem.empty());
        // No refusal is ever answered by asking for a new pairing.
        CHECK_EQ(bench.network.count("POST", "/pairing/"), static_cast<std::size_t>(0));
    }
}

TEST(AKeyFileWithAnyOtherModeIsNeitherUsedNorReplaced) {
    Bench bench;
    bench.writeFile(bench.keyPath(), member(parseJson(fixture("keys.json")), "executorSeedHex"));
    ::chmod(bench.keyPath().c_str(), 0644);
    const std::string before = readFile(bench.keyPath());

    bench.tick();

    CHECK_EQ(readFile(bench.keyPath()), before);
    CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(0));
}

// --- outage detector --------------------------------------------------------

TEST(OneFailedProbeNeverConfirmsLoss) {
    CoordinatorOutageDetector detector;
    detector.recordProbe(true, 0);
    detector.recordProbe(false, 5000);
    CHECK(!detector.confirmedLost());
    CHECK(!detector.reachable());
}

TEST(LossNeedsBothTheFailedProbeCountAndTheMinimumSpan) {
    CoordinatorOutageDetector detector;
    detector.recordProbe(false, 0);
    detector.recordProbe(false, 5000);
    detector.recordProbe(false, 10000);
    // Three failures, the count, but only 10 of the 15 seconds.
    CHECK(!detector.confirmedLost());
    detector.recordProbe(false, 15000);
    CHECK(detector.confirmedLost());

    OutageDetectorConfig slowProbes;
    slowProbes.probeIntervalMillis = 20000;
    CoordinatorOutageDetector spanFirst(slowProbes);
    spanFirst.recordProbe(false, 0);
    spanFirst.recordProbe(false, 20000);
    // 20 seconds, the span, but only two of the three failures.
    CHECK(!spanFirst.confirmedLost());
}

TEST(OneGoodProbeEndsARunOfFailures) {
    CoordinatorOutageDetector detector;
    for (TimeMillis t = 0; t <= 15000; t += 5000) detector.recordProbe(false, t);
    CHECK(detector.confirmedLost());
    detector.recordProbe(true, 20000);
    CHECK(!detector.confirmedLost());
    detector.recordProbe(false, 25000);
    CHECK(!detector.confirmedLost());
}

TEST(OutageSettingsAcceptOnlyAWholeNumberInRange) {
    int value = 7;
    CHECK(parseOutageSetting("5000", 1000, 600000, &value));
    CHECK_EQ(value, 5000);
    for (const char* bad : {"", "abc", "5000ms", "5.5", "999", "600001", "-1"}) {
        value = 7;
        CHECK(!parseOutageSetting(bad, 1000, 600000, &value));
        CHECK_EQ(value, 7);
    }
}

TEST(TheProbeReadsHealthzWithItsOwnTimeoutAndOnlyA2xxCountsAsReached) {
    Bench bench;
    bench.tick();
    const std::vector<Sent> probes = bench.network.sent("GET", "/healthz");
    CHECK_EQ(probes.size(), static_cast<std::size_t>(1));
    CHECK_EQ(probes[0].url, std::string(kCoordinatorUrl) + "/healthz");
    CHECK_EQ(probes[0].timeoutMillis, kHypothesisProbeTimeoutMillis);
    CHECK(probes[0].bearerToken.empty());
    CHECK(bench.executor->status().coordinatorReachable);

    // A proxy in front of a dead coordinator answers, with an error status.
    bench.network.healthStatus = 502;
    for (int i = 0; i < 4; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK(bench.executor->coordinatorLost());
}

TEST(ProbesRunNoMoreOftenThanTheConfiguredInterval) {
    Bench bench;
    bench.tick();
    bench.advanceAndTick(kHypothesisProbeIntervalMillis - 1);
    CHECK_EQ(bench.network.count("GET", "/healthz"), static_cast<std::size_t>(1));
    bench.advanceAndTick(1);
    CHECK_EQ(bench.network.count("GET", "/healthz"), static_cast<std::size_t>(2));
}

// --- keeping the program current -------------------------------------------

TEST(AFetchedProgramIsInstalledAcknowledgedAndHandedToEveryDistinctNodeAddressOnce) {
    Bench bench;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    bench.tick();

    CHECK_EQ(readFile(bench.installPath()), fixture("program.json"));
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
    // node-b appears under two entries and still gets one copy.
    const std::vector<Sent> handed = bench.network.sent("PUT", kNodeProgramPathPrefix);
    CHECK_EQ(handed.size(), static_cast<std::size_t>(2));
    CHECK_EQ(handed[0].url, std::string("http://") + kNodeA + kNodeProgramPathPrefix + kFppUuid);
    CHECK_EQ(handed[1].url, std::string("http://") + kNodeB + kNodeProgramPathPrefix + kFppUuid);
    CHECK_EQ(handed[0].body, fixture("program.json"));
    CHECK(handed[0].bearerToken.empty());
    CHECK(bench.executor->status().programEnrollsThisExecutor);
}

TEST(TheProgramIsRefetchedAfterTheNamedFractionOfItsOwnValidityAndHandedOutAgain) {
    Bench bench;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    bench.tick();
    const std::string programRoute = std::string("/api/v1/fallback-programs/") + kFppUuid;
    auto fetches = [&] { return bench.network.count("GET", programRoute); };
    CHECK_EQ(fetches(), static_cast<std::size_t>(1));

    // Fifteen minutes of validity, one third of it: five minutes.
    const TimeMillis delay = 15 * 60 * 1000 * kRefetchValidityNumerator / kRefetchValidityDenominator;
    CHECK_EQ(delay, kFiveMinutesMillis);
    for (TimeMillis waited = kHypothesisProbeIntervalMillis; waited < delay; waited += kHypothesisProbeIntervalMillis) {
        bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    }
    CHECK_EQ(fetches(), static_cast<std::size_t>(1));

    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(fetches(), static_cast<std::size_t>(2));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(4));
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(2));
}

TEST(NothingIsFetchedRegisteredOrHandedOutWhileTheCoordinatorDoesNotAnswer) {
    Bench bench;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    bench.network.coordinatorUp = false;
    for (int i = 0; i < 80; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);

    CHECK_EQ(bench.network.count("GET", "/api/v1/fallback-programs/"), static_cast<std::size_t>(0));
    CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(0));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(0));
    CHECK(!std::filesystem::exists(bench.installPath()));
}

// --- delivery, through the runtime entry point ------------------------------

TEST(AnOutageAtAPlaylistTransitionActivatesTheMappedCueExactlyOnceOnTheIntendedNode) {
    Bench bench;
    const std::string program = signedProgram(twoEntryProgram(bench));
    bench.network.programEnvelope = getEnvelope(program);
    bench.tick();
    CHECK_EQ(readFile(bench.installPath()), program);

    // The coordinator answers: the entry is observed and nothing is sent to a node.
    bench.callback("playing", 0);
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));

    // Loss is confirmed in the middle of entry 0. Nothing happens mid-entry,
    // and FPP resuming the same entry is not a boundary either.
    bench.loseCoordinator();
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);

    // The transition: FPP finishes entry 0 and starts entry 1.
    bench.callback("query_next", 0);
    bench.callback("playing", 1);

    const std::vector<Sent> activations = bench.activations();
    CHECK_EQ(activations.size(), static_cast<std::size_t>(1));
    CHECK_EQ(activations[0].url, std::string("http://") + kNodeA + kNodeActivationPath);
    CHECK_EQ(activations[0].timeoutMillis, kNodeRequestTimeoutMillis);
    CHECK(activations[0].bearerToken.empty());
    CHECK_EQ(requestMember(activations[0], "entryKey"), bench.entryKey(1));
    CHECK_EQ(requestMember(activations[0], "cueId"), std::string("cue-b"));
    CHECK_EQ(requestMember(activations[0], "nodeId"), std::string("node-a"));
    CHECK_EQ(requestMember(activations[0], "packageId"), std::string("pkg-test"));
    CHECK_EQ(requestMember(activations[0], "packageRevision"), std::string("rev-test"));
    CHECK_EQ(requestMember(activations[0], "programExpiresAt"), std::string("2026-10-05T12:15:00Z"));
    CHECK_EQ(requestMember(activations[0], "catalogRevision"), std::string("cat-rev-1"));
    CHECK_EQ(requestMember(activations[0], "fppInstanceUuid"), std::string(kFppUuid));

    // The signature is the executor key's over the canonical bytes of the request as sent.
    const showmesh::json::Value body = parseJson(activations[0].body);
    const showmesh::json::CanonicalResult canonical = showmesh::json::canonicalize(child(body, "request"));
    std::string expectedSignature;
    CHECK(signWithExecutorKey(fixtureExecutorKey(), canonical.text, &expectedSignature));
    CHECK_EQ(member(body, "signature"), expectedSignature);

    const FallbackStatusSnapshot status = bench.executor->status();
    CHECK(status.state.mode == FallbackMode::kFallback);
    CHECK_EQ(status.state.enteredAtEntryKey, bench.entryKey(1));
    CHECK_EQ(status.activationsAuthorized, static_cast<std::uint64_t>(1));
    const std::vector<FallbackRecord> records = bench.activationRecords();
    CHECK_EQ(records.size(), static_cast<std::size_t>(1));
    CHECK_EQ(records[0].outcome, std::string("authorized"));
    CHECK_EQ(records[0].reason, std::string("node says authorized"));
    CHECK_EQ(records[0].executionId, requestMember(activations[0], "executionId"));

    // The three callbacks drained while lost posted nothing to the coordinator, and that is counted.
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(status.coordinatorPostsSkipped, static_cast<std::uint64_t>(3));
    CHECK_EQ(bench.runtime->postsSkippedWhileCoordinatorLostCount(), static_cast<std::uint64_t>(3));

    // An operator can read it: the notice, and the status file beside the other status files.
    CHECK_EQ(bench.notifier.raised.back(), std::string(kFallbackActiveMessage));
    const showmesh::json::Value file = parseJson(readFile(bench.stateDir() + "/" + kFallbackStatusFilename));
    CHECK_EQ(member(file, "mode"), std::string("fallback"));
    CHECK_EQ(member(file, "message"), std::string(kFallbackActiveMessage));
    CHECK_EQ(member(child(file, "recent").items().back(), "outcome"), std::string("authorized"));
}

TEST(ASingleFailedOrSlowProbeDoesNotEnterFallbackAtTheNextEntry) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.tick();

    // One probe times out, the next answers.
    bench.network.coordinatorUp = false;
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    bench.callback("playing", 0);
    bench.network.coordinatorUp = true;
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    bench.callback("query_next", 0);
    bench.callback("playing", 1);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(3));
    CHECK(bench.notifier.raised.empty());
}

TEST(AnEntryThatTargetsTwoNodesSendsEachItsOwnExecutionId) {
    Bench bench;
    ProgramSpec spec;
    spec.entries = {{bench.entryKey(0), "cue-a", 3, {{"node-a", kNodeA}, {"node-b", kNodeB}}}};
    bench.writeFile(bench.installPath(), signedProgram(spec));
    bench.loseCoordinator();
    bench.callback("playing", 0);

    const std::vector<Sent> activations = bench.activations();
    CHECK_EQ(activations.size(), static_cast<std::size_t>(2));
    CHECK_NE(requestMember(activations[0], "executionId"), requestMember(activations[1], "executionId"));
    std::map<std::string, std::string> catalogRevisionByNode;
    for (const Sent& activation : activations) {
        catalogRevisionByNode[requestMember(activation, "nodeId")] = requestMember(activation, "catalogRevision");
        CHECK(activation.url.find(requestMember(activation, "nodeId") == "node-a" ? kNodeA : kNodeB) !=
              std::string::npos);
    }
    CHECK_EQ(catalogRevisionByNode["node-a"], std::string("cat-rev-1"));
    CHECK_EQ(catalogRevisionByNode["node-b"], std::string("cat-rev-b"));
}

TEST(ALoopBackIntoTheSameEntryIsANewOccurrenceWithANewExecutionId) {
    Bench bench;
    ProgramSpec spec;
    spec.entries = {{bench.entryKey(0), "cue-a", 3, {{"node-a", kNodeA}}}};
    bench.writeFile(bench.installPath(), signedProgram(spec));
    bench.loseCoordinator();

    bench.callback("playing", 0, 0);
    bench.callback("playing", 0, 0);  // resumed, the same occurrence
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));

    bench.callback("query_next", 0, 0);
    bench.callback("playing", 0, 1);  // the playlist looped
    const std::vector<Sent> activations = bench.activations();
    CHECK_EQ(activations.size(), static_cast<std::size_t>(2));
    CHECK_NE(requestMember(activations[0], "executionId"), requestMember(activations[1], "executionId"));
    CHECK_EQ(requestMember(activations[0], "entryKey"), requestMember(activations[1], "entryKey"));
}

TEST(AnEntryTheProgramDoesNotMapSendsNothingAndRecordsWhy) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    bench.callback("playing", 2);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(0));
    const std::vector<FallbackRecord> records = bench.activationRecords();
    CHECK_EQ(records.size(), static_cast<std::size_t>(1));
    CHECK_EQ(records[0].outcome, std::string("kUnknownEntry"));
    CHECK(!records[0].reason.empty());
    CHECK_EQ(records[0].entryKey, bench.entryKey(2));
    CHECK_EQ(bench.executor->status().activationsNotDelivered, static_cast<std::uint64_t>(1));
}

TEST(AnEntryFppCouldNotIdentifySendsNothingAndRecordsWhy) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    bench.definitions.definition = "";
    bench.callback("playing", 0);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeEntryNotIdentified));
}

TEST(AMissingProgramSendsNothingAndRecordsWhy) {
    Bench bench;
    bench.loseCoordinator();
    bench.callback("playing", 0);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string("kNoProgramInstalled"));
}

TEST(AnExpiredProgramSendsNothingAndRecordsWhy) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    gClock = kCompiledAtMillis + 15 * 60 * 1000;
    bench.callback("playing", 0);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string("kProgramExpired"));
}

TEST(AProgramThatDoesNotCarryThisPlayersKeySendsNothingAndRecordsWhy) {
    Bench bench;
    ProgramSpec noKey = twoEntryProgram(bench);
    noKey.executorPublicKey.clear();
    bench.writeFile(bench.installPath(), signedProgram(noKey));
    bench.loseCoordinator();
    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeExecutorKeyNotInProgram));

    ProgramSpec otherKey = twoEntryProgram(bench);
    otherKey.executorPublicKey = member(parseJson(fixture("keys.json")), "otherExecutorPublicKey");
    bench.writeFile(bench.installPath(), signedProgram(otherKey));
    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeExecutorKeyNotInProgram));
}

TEST(AnUnpairedPlayerWithNoKeySendsNothingAndRecordsWhy) {
    Bench bench(/*paired=*/false);
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    bench.callback("playing", 0);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeNoExecutorKey));
}

TEST(ATargetWithNoAddressGetsNothingAndIsRecordedWhileItsSiblingIsActivated) {
    Bench bench;
    ProgramSpec spec;
    spec.entries = {{bench.entryKey(0), "cue-a", 3, {{"node-a", ""}, {"node-b", kNodeB}}}};
    bench.writeFile(bench.installPath(), signedProgram(spec));
    bench.loseCoordinator();
    bench.callback("playing", 0);

    const std::vector<Sent> activations = bench.activations();
    CHECK_EQ(activations.size(), static_cast<std::size_t>(1));
    CHECK_EQ(activations[0].url, std::string("http://") + kNodeB + kNodeActivationPath);
    const std::vector<FallbackRecord> records = bench.activationRecords();
    CHECK_EQ(records.size(), static_cast<std::size_t>(2));
    CHECK_EQ(records[0].nodeId, std::string("node-a"));
    CHECK_EQ(records[0].outcome, std::string(kOutcomeNoAddress));
    CHECK_EQ(records[1].outcome, std::string("authorized"));
}

TEST(ARefusalIsRecordedWithTheNodesWordAndReasonAndNothingElseIsSentInItsPlace) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.network.scriptActivation(kNodeA, nodeAnswer(403, "cue-not-authorized"));
    bench.loseCoordinator();
    bench.callback("playing", 0);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(0));
    const FallbackRecord record = bench.lastActivationRecord();
    CHECK_EQ(record.outcome, std::string("cue-not-authorized"));
    CHECK_EQ(record.reason, std::string("node says cue-not-authorized"));
    CHECK_EQ(record.attempts, 1);
    CHECK_EQ(bench.executor->status().activationsAuthorized, static_cast<std::uint64_t>(0));
}

TEST(OnceEnteredFallbackLastsUntilThePlaylistStopsEvenIfTheCoordinatorAnswersAgain) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    CHECK_EQ(bench.notifier.raised.back(), std::string(kCoordinatorLostMessage));
    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));

    // The coordinator comes back mid-playlist. This player keeps delivering,
    // keeps its posts suspended, and installs nothing new.
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK(bench.executor->status().coordinatorReachable);
    CHECK(bench.executor->coordinatorLost());
    CHECK_EQ(bench.network.count("GET", "/api/v1/fallback-programs/"), static_cast<std::size_t>(0));
    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(2));
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));

    // The playlist stops: normal operation, the notice is withdrawn, posts and refetch resume.
    bench.callback("playing", 0, std::nullopt, "");
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK(!bench.executor->coordinatorLost());
    CHECK_EQ(bench.notifier.cleared.back(), std::string(kFallbackActiveMessage));
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(1));
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(bench.network.count("GET", "/api/v1/fallback-programs/"), static_cast<std::size_t>(1));

    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(2));
}

TEST(AnOperatorNoticeNamesTheFactThenTheActionWithNoInternalsVocabulary) {
    for (const std::string& message : {std::string(kCoordinatorLostMessage), std::string(kFallbackActiveMessage)}) {
        CHECK(message.rfind("The coordinator", 0) == 0);
        CHECK(message.find("Check the coordinator.") != std::string::npos);
        for (const char* word : {"fallback", "interlock", "revision", "epoch", "evidence", "boundary", "armed", "ADR",
                                 "executor", "program"}) {
            CHECK(message.find(word) == std::string::npos);
        }
    }
}

// --- the section 5.8 table, case by case ------------------------------------

namespace {

struct TableRun {
    FakeNetwork network;
    std::vector<int> pauses;
    bool keepGoing = true;
    NodeDeliveryResult result;

    void deliver() {
        result = DeliverActivation(&network, kNodeA, "{\"request\":{},\"signature\":\"sig\"}", kFppUuid,
                                   "the installed program", [this](int millis) {
                                       pauses.push_back(millis);
                                       return keepGoing;
                                   });
    }
    std::vector<Sent> posts() { return network.sent("POST", kNodeActivationPath); }
    std::string postBody(std::size_t index) { return posts()[index].body; }
    std::vector<Sent> programs() { return network.sent("PUT", kNodeProgramPathPrefix); }
};

}  // namespace

TEST(Table_AuthorizedIsFinal) {
    TableRun run;
    run.deliver();
    CHECK(run.result.activated());
    CHECK_EQ(run.result.attempts, 1);
    CHECK(run.pauses.empty());
}

TEST(Table_NoResponseIsRetriedWithTheSameBodyAtMostThreeAttemptsAtLeast250MsApart) {
    TableRun run;
    run.network.nodesDown.insert(kNodeA);
    run.deliver();

    CHECK_EQ(run.result.attempts, 3);
    CHECK_EQ(run.result.outcome, std::string(kOutcomeNoResponse));
    CHECK_EQ(run.result.reason, std::string("connection refused"));
    CHECK(!run.result.activated());
    const std::vector<Sent> posts = run.posts();
    CHECK_EQ(posts.size(), static_cast<std::size_t>(3));
    CHECK_EQ(posts[1].body, posts[0].body);
    CHECK_EQ(posts[2].body, posts[0].body);
    CHECK_EQ(run.pauses.size(), static_cast<std::size_t>(2));
    CHECK(run.pauses[0] >= 250 && run.pauses[1] >= 250);
    CHECK(run.programs().empty());
}

TEST(Table_StorageUnavailableNotReadyAndAnyOther5xxAreRetriedThenSucceed) {
    for (const HttpResponse& transient :
         {nodeAnswer(503, "storage-unavailable"), nodeAnswer(503, "not-ready"), answer(502, "bad gateway"),
          nodeAnswer(500, "some-later-word")}) {
        TableRun run;
        run.network.scriptActivation(kNodeA, transient);
        run.deliver();
        CHECK(run.result.activated());
        CHECK_EQ(run.result.attempts, 2);
        CHECK_EQ(run.pauses.size(), static_cast<std::size_t>(1));
    }
}

TEST(Table_NoCoordinatorKeyIsFinalAlthoughItIsA503) {
    TableRun run;
    run.network.scriptActivation(kNodeA, nodeAnswer(503, "no-coordinator-key"));
    run.deliver();
    CHECK_EQ(run.result.outcome, std::string("no-coordinator-key"));
    CHECK_EQ(run.result.attempts, 1);
    CHECK(run.pauses.empty());
}

TEST(Table_RateLimitedIsRetriedOnceAfterOneSecond) {
    TableRun run;
    run.network.scriptActivation(kNodeA, nodeAnswer(429, "rate-limited"));
    run.network.scriptActivation(kNodeA, nodeAnswer(429, "rate-limited"));
    run.deliver();

    CHECK_EQ(run.result.outcome, std::string("rate-limited"));
    CHECK_EQ(run.result.attempts, 2);
    CHECK_EQ(run.pauses.size(), static_cast<std::size_t>(1));
    CHECK_EQ(run.pauses[0], 1000);
    CHECK_EQ(run.postBody(1), run.postBody(0));
}

TEST(Table_ProgramNotInstalledOrNotCurrentSendsTheInstalledProgramThenRetriesOnce) {
    for (const char* word : {"program-not-installed", "program-not-current"}) {
        TableRun run;
        run.network.scriptActivation(kNodeA, nodeAnswer(409, word));
        run.deliver();
        CHECK(run.result.activated());
        CHECK(run.result.programResent);
        CHECK_EQ(run.result.attempts, 2);
        const std::vector<Sent> programs = run.programs();
        CHECK_EQ(programs.size(), static_cast<std::size_t>(1));
        CHECK_EQ(programs[0].url, std::string("http://") + kNodeA + kNodeProgramPathPrefix + kFppUuid);
        CHECK_EQ(programs[0].body, std::string("the installed program"));
        CHECK_EQ(run.postBody(1), run.postBody(0));

        TableRun stillMissing;
        stillMissing.network.scriptActivation(kNodeA, nodeAnswer(409, word));
        stillMissing.network.scriptActivation(kNodeA, nodeAnswer(409, word));
        stillMissing.deliver();
        CHECK_EQ(stillMissing.result.outcome, std::string(word));
        CHECK_EQ(stillMissing.result.attempts, 2);
        CHECK_EQ(stillMissing.programs().size(), static_cast<std::size_t>(1));
    }
}

TEST(Table_ReplayedExecutionIsFinalAndTakesTheFirstOutcomeAsTheResult) {
    TableRun authorizedBefore;
    authorizedBefore.network.scriptActivation(
        kNodeA, nodeAnswer(409, "replayed-execution", ",\"firstOutcome\":\"authorized\""));
    authorizedBefore.deliver();
    CHECK(authorizedBefore.result.replayed);
    CHECK(authorizedBefore.result.activated());
    CHECK_EQ(authorizedBefore.result.attempts, 1);

    TableRun refusedBefore;
    refusedBefore.network.scriptActivation(kNodeA,
                                           nodeAnswer(409, "replayed-execution", ",\"firstOutcome\":\"apply-failed\""));
    refusedBefore.deliver();
    CHECK_EQ(refusedBefore.result.outcome, std::string("apply-failed"));

    TableRun nodeRestarted;
    nodeRestarted.network.scriptActivation(kNodeA,
                                           nodeAnswer(409, "replayed-execution", ",\"firstOutcome\":\"unknown\""));
    nodeRestarted.deliver();
    CHECK_EQ(nodeRestarted.result.outcome, std::string("unknown"));
    CHECK(!nodeRestarted.result.activated());
}

TEST(Table_ALostAnswerThenAReplayIsOneActivationNotTwo) {
    TableRun run;
    run.network.scriptActivation(kNodeA, noResponse());
    run.network.scriptActivation(kNodeA, nodeAnswer(409, "replayed-execution", ",\"firstOutcome\":\"authorized\""));
    run.deliver();
    CHECK(run.result.activated());
    CHECK_EQ(run.result.attempts, 2);
    CHECK_EQ(run.postBody(1), run.postBody(0));
}

TEST(Table_AnythingElseIsFinalAfterOneAttemptAndNothingIsSentInItsPlace) {
    for (const char* word :
         {"malformed-request", "too-large", "wrong-target", "program-expired", "executor-not-enrolled",
          "signature-invalid", "unknown-entry", "cue-not-authorized", "stale-generation", "stale-catalog",
          "cross-show", "unknown-generation", "unknown-cue", "stale-cue", "asset-missing", "weather-delay-active",
          "apply-failed", "a-word-this-build-has-never-seen"}) {
        TableRun run;
        run.network.scriptActivation(kNodeA, nodeAnswer(409, word));
        run.deliver();
        CHECK_EQ(run.result.outcome, std::string(word));
        CHECK_EQ(run.result.attempts, 1);
        CHECK(!run.result.activated());
        CHECK(run.pauses.empty());
        CHECK_EQ(run.posts().size(), static_cast<std::size_t>(1));
        CHECK(run.programs().empty());
    }
}

TEST(Table_AnAnswerWithNoOutcomeWordIsFinalAndNeverReadAsSuccess) {
    TableRun run;
    run.network.scriptActivation(kNodeA, answer(200, "<html>not a node</html>"));
    run.deliver();
    CHECK_EQ(run.result.outcome, std::string(kOutcomeUnrecognizedAnswer));
    CHECK(!run.result.activated());
    CHECK_EQ(run.result.attempts, 1);
}

TEST(Table_AStoppingPluginGivesUpInsteadOfWaitingOutARetry) {
    TableRun run;
    run.network.nodesDown.insert(kNodeA);
    run.keepGoing = false;
    run.deliver();
    CHECK_EQ(run.result.outcome, std::string(kOutcomeStopped));
    CHECK_EQ(run.result.attempts, 1);
}

// --- threads and wiring ------------------------------------------------------

TEST(TheRuntimeStartsAndStopsTheExecutorsOwnThreadAndStopDoesNotWaitOutAProbeInterval) {
    Bench bench;
    bench.runtime->start();
    for (int i = 0; i < 300 && bench.network.count("GET", "/healthz") == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(bench.network.count("GET", "/healthz") >= 1);
    // Callbacks drain on the runtime worker while the executor's own thread ticks.
    for (int position = 0; position < 3; ++position) {
        bench.runtime->observeCallback(kPlaylistName, "playing", "mainPlaylist", position, "a.fseq", "");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const auto begin = std::chrono::steady_clock::now();
    bench.runtime->stop();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    CHECK(elapsed < 1000);
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(3));
}

TEST(BothShippingAdaptersConstructTheDeliveryAndHandItsRecorderToTheRuntime) {
    for (const char* adapter : {"fpp9/plugin.cpp", "fpp10/plugin.cpp"}) {
        const std::string source = readFile(adapter);
        CHECK(source.find("#include \"fallback_activation_delivery.h\"") != std::string::npos);
        CHECK(source.find("showmesh::adapter::FallbackActivationDelivery fallbackDelivery_;") != std::string::npos);
        CHECK(source.find("safeCeilingPercent_, fallbackDelivery_.recorder(), pairingDelivery_.worker()") !=
              std::string::npos);
        CHECK(source.find("runtime_.observeCallback(") != std::string::npos);
    }
    const std::string delivery = readFile("shared/fallback_activation_delivery.h");
    CHECK(delivery.find("showmesh::fallback::FallbackExecutor executor_;") != std::string::npos);
    CHECK(delivery.find("recorder() { return &executor_; }") != std::string::npos);
}
