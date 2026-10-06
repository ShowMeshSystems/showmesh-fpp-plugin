// Tests for the ADR-048 fallback executor: the executor key, the outage
// detector, the refetch cadence, and delivery to nodes per contract section
// 5. Delivery is driven through ShowMeshRuntime::observeCallback(), the entry
// point both shipping adapters forward FPP's playlistCallback into.

#include <sys/stat.h>

#include <chrono>
#include <deque>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
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
    std::string restHold = "hold";  // any other value is a rule this build does not know
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
            << "\",\"rules\":{\"fallbackBoundary\":\"safe-playback-boundary\",\"localShutdown\":\"local-shutdown\","
            << "\"recoveryBoundary\":\"next-scheduled-show-boundary\",\"restHold\":\"" << spec.restHold
            << "\"},\"schemaVersion\":1}";
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

    // Something that is not a request, kept in the same order as the requests.
    void note(const std::string& what) {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_.push_back(Sent{what, std::string(), std::string(), std::string(), 0});
    }

    // Every request and note so far, as "METHOD path", in order.
    std::vector<std::string> order() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> lines;
        for (const Sent& s : sent_) {
            const std::size_t path = s.url.find('/', 8);
            lines.push_back(s.method + (s.url.empty() ? "" : " " + s.url.substr(path == std::string::npos ? 0 : path)));
        }
        return lines;
    }

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
    int stateReportStatus = 200;
    bool stateReportRecorded = true;
    std::string stateReportRefusalBody = "{\"title\":\"Refused\",\"detail\":\"the coordinator says why\"}";
    std::set<std::string> nodesDown;
    // Runs after a request is recorded and before it is answered, outside the lock.
    std::function<void(const std::string& method, const std::string& url)> beforeAnswer;

 private:
    HttpResponse handle(const std::string& method, const HttpRequest& request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sent_.push_back(Sent{method, request.url, request.body, request.bearerToken, request.timeoutMillis});
        }
        if (beforeAnswer) beforeAnswer(method, request.url);
        std::lock_guard<std::mutex> lock(mutex_);
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
            if (path.find("/fallback-state") != std::string::npos) {
                if (stateReportStatus != 200) return answer(stateReportStatus, stateReportRefusalBody);
                return answer(200, std::string("{\"recorded\":") + (stateReportRecorded ? "true" : "false") +
                                       ",\"state\":\"normal\"}");
            }
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
        if (network != nullptr) network->note("OBSERVE");
        return true;
    }
    bool publishUnavailable(const PlaylistEntryObservation& observation) override {
        published.push_back(observation);
        if (network != nullptr) network->note("OBSERVE");
        return true;
    }
    FakeNetwork* network = nullptr;
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
    std::string statePath() const { return credentialDir() + "/" + kFallbackStateFilename; }

    // The notice a healthy fallback under the bench playlist raises.
    std::string activeNotice(bool coordinatorAnswering = false) const {
        NoticeFacts facts;
        facts.state.mode = FallbackMode::kFallback;
        facts.state.playlistName = "Main Show";
        facts.state.cutoffAt = "2026-10-05T12:15:00Z";
        facts.coordinatorLost = !coordinatorAnswering;
        facts.coordinatorReachable = coordinatorAnswering;
        return FallbackNotice(facts);
    }
    std::string statusMessage() const {
        return member(parseJson(readFile(stateDir() + "/" + kFallbackStatusFilename)), "message");
    }

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
        options.pinnedKey = pinnedKey;
        options.detector = detectorConfig;
        options.playingPlaylistAtStart = playingPlaylistAtStart;
        options.randomBytes = fixtureRandom;
        options.notifier = &notifier;
        options.log = [this](bool, const std::string& line) { logs.push_back(line); };
        options.pause = [this](int millis) {
            pauses.push_back(millis);
            return true;
        };
        sink.network = &network;
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
        return executor->status().recentActivations;
    }

    PinnedKeyLoadResult pinnedKey = fixturePinnedKey();
    OutageDetectorConfig detectorConfig;
    // What FPP is playing when the next executor is made: a plugin restart reads it once.
    std::string playingPlaylistAtStart;
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

TEST(AnyRefusedRegistrationLeavesTheKeyUnregisteredAndShowsTheCoordinatorsReason) {
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

        // Not before the next program fetch.
        bench.advanceAndTick(kHypothesisProbeIntervalMillis);
        CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(1));

        bench.network.registrationStatus = 200;
        bench.advanceAndTick(kProgramRefetchIntervalMillis);
        CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(2));
        CHECK(bench.executor->status().executorKeyRegistered);
        CHECK(bench.executor->status().executorKeyRegistrationProblem.empty());
        // No refusal is ever answered by asking for a new pairing.
        CHECK_EQ(bench.network.count("POST", "/pairing/"), static_cast<std::size_t>(0));
    }
}

namespace {

// Ticks every probe interval for the given time and returns how many registrations were sent in it.
std::size_t registrationsDuring(Bench* bench, TimeMillis millis) {
    const std::size_t before = bench->network.count("PUT", "/executor-key");
    for (TimeMillis waited = 0; waited < millis; waited += kHypothesisProbeIntervalMillis) {
        bench->advanceAndTick(kHypothesisProbeIntervalMillis);
    }
    return bench->network.count("PUT", "/executor-key") - before;
}

}  // namespace

TEST(A409RegistrationIsAskedAgainAtEveryProgramFetch) {
    Bench bench;
    bench.network.registrationStatus = 409;
    bench.tick();
    // Ten refetch intervals, ten more attempts: this answer clears by waiting.
    CHECK_EQ(registrationsDuring(&bench, 10 * kProgramRefetchIntervalMillis), static_cast<std::size_t>(10));
}

TEST(A403RegistrationBacksOffDoublingToTenMinutesAndStartsOverWhenTheAnswerChanges) {
    Bench bench;
    bench.network.registrationStatus = 403;
    bench.tick();
    // Retries land 1, 2, 4, 8 and then 10 minutes apart: at minutes 1, 3, 7, 15, 25, 35.
    CHECK_EQ(registrationsDuring(&bench, 35 * 60 * 1000), static_cast<std::size_t>(6));
    CHECK_EQ(registrationsDuring(&bench, 9 * 60 * 1000), static_cast<std::size_t>(0));
    CHECK_EQ(registrationsDuring(&bench, 60 * 1000), static_cast<std::size_t>(1));

    // A different answer starts the backoff over: one interval, then two.
    bench.network.registrationStatus = 500;
    CHECK_EQ(registrationsDuring(&bench, 10 * 60 * 1000), static_cast<std::size_t>(1));
    CHECK_EQ(registrationsDuring(&bench, 60 * 1000), static_cast<std::size_t>(1));
    CHECK_EQ(registrationsDuring(&bench, 60 * 1000), static_cast<std::size_t>(0));
    CHECK_EQ(registrationsDuring(&bench, 60 * 1000), static_cast<std::size_t>(1));
}

TEST(AProgramWithoutThisPlayersKeyIsNotRewrittenAcknowledgedOrHandedOutAgainAndTheKeyIsRegisteredAgainWithBackoff) {
    Bench bench;
    ProgramSpec noKey = twoEntryProgram(bench);
    noKey.executorPublicKey.clear();
    noKey.expiresAt = "2026-10-05T23:00:00Z";
    bench.network.programEnvelope = getEnvelope(signedProgram(noKey));
    // The coordinator says the key is stored, and keeps publishing a program without it.
    bench.network.registrationChanged = false;
    bench.tick();
    CHECK(bench.executor->status().executorKeyRegistered);
    CHECK(!bench.executor->status().programEnrollsThisExecutor);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(1));
    const auto installedAt = std::filesystem::last_write_time(bench.installPath());
    const std::string statusBefore = readFile(bench.stateDir() + "/" + kFallbackStatusFilename);

    // Thirty-five minutes: the key is registered again at minutes 1, 3, 7, 15, 25 and 35, not every fetch.
    CHECK_EQ(registrationsDuring(&bench, 35 * 60 * 1000), static_cast<std::size_t>(6));
    CHECK_EQ(bench.network.count("GET", std::string("/api/v1/fallback-programs/") + kFppUuid),
             static_cast<std::size_t>(36));
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(1));
    CHECK(std::filesystem::last_write_time(bench.installPath()) == installedAt);
    CHECK_EQ(readFile(bench.stateDir() + "/" + kFallbackStatusFilename), statusBefore);

    // The coordinator publishes the key: installed and handed out once, and no more registrations.
    // A key is program content, so the copy that carries it has a new revision and package id.
    ProgramSpec withKey = twoEntryProgram(bench);
    withKey.expiresAt = "2026-10-05T23:00:00Z";
    withKey.revision = "rev-with-key";
    withKey.packageId = "pkg-with-key";
    bench.network.programEnvelope = getEnvelope(signedProgram(withKey));
    CHECK_EQ(registrationsDuring(&bench, 20 * 60 * 1000), static_cast<std::size_t>(0));
    CHECK(bench.executor->status().programEnrollsThisExecutor);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(2));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(2));
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

TEST(TheProgramIsRefetchedOnTheConfiguredIntervalAndAnUnchangedOneDoesNothingFurther) {
    Bench bench;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    bench.tick();
    const std::string programRoute = std::string("/api/v1/fallback-programs/") + kFppUuid;
    auto fetches = [&] { return bench.network.count("GET", programRoute); };
    CHECK_EQ(fetches(), static_cast<std::size_t>(1));
    const auto installedAt = std::filesystem::last_write_time(bench.installPath());

    for (TimeMillis waited = kHypothesisProbeIntervalMillis; waited < kProgramRefetchIntervalMillis;
         waited += kHypothesisProbeIntervalMillis) {
        bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    }
    CHECK_EQ(fetches(), static_cast<std::size_t>(1));
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(fetches(), static_cast<std::size_t>(2));

    // The same revision and expiry: no file rewrite, no acknowledge, no hand-out, no record.
    CHECK(std::filesystem::last_write_time(bench.installPath()) == installedAt);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(2));
    CHECK_EQ(bench.executor->status().recentProgramHandOffs.size(), static_cast<std::size_t>(2));
}

TEST(ACopyWithTheSamePackageRevisionAndExpiryIsUnchangedWhateverElseItSays) {
    Bench bench;
    ProgramSpec first = twoEntryProgram(bench);
    bench.network.programEnvelope = getEnvelope(signedProgram(first));
    bench.tick();
    const std::string installed = readFile(bench.installPath());

    // Section 5.12 compares exactly these three members, so this copy is the one already held.
    ProgramSpec sameIdentity = first;
    sameIdentity.compiledAt = "2026-10-05T12:01:00Z";
    bench.network.programEnvelope = getEnvelope(signedProgram(sameIdentity));
    for (int i = 0; i < 12; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(readFile(bench.installPath()), installed);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
}

TEST(AChangedProgramIsInstalledAcknowledgedAndHandedOutAtTheNextRefetch) {
    Bench bench;
    bench.network.programEnvelope = getEnvelope(signedProgram(twoEntryProgram(bench)));
    bench.tick();
    ProgramSpec refreshed = twoEntryProgram(bench);
    refreshed.expiresAt = "2026-10-05T12:20:00Z";
    const std::string refreshedDocument = signedProgram(refreshed);
    bench.network.programEnvelope = getEnvelope(refreshedDocument);

    for (TimeMillis waited = 0; waited < kProgramRefetchIntervalMillis;
         waited += kHypothesisProbeIntervalMillis) {
        bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    }
    CHECK_EQ(readFile(bench.installPath()), refreshedDocument);
    CHECK_EQ(bench.executor->status().programExpiresAt, std::string("2026-10-05T12:20:00Z"));
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(2));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(2));
}

TEST(APublishedProgramThatStaysRefusedIsAcknowledgedOnceNotAtEveryFetch) {
    Bench bench;
    ProgramSpec expired = twoEntryProgram(bench);
    expired.expiresAt = "2026-10-05T12:01:00Z";
    bench.network.programEnvelope = getEnvelope(signedProgram(expired));
    bench.tick();
    for (int i = 0; i < 60; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);

    CHECK(bench.network.count("GET", std::string("/api/v1/fallback-programs/") + kFppUuid) >= 5);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
    CHECK(!std::filesystem::exists(bench.installPath()));
}

TEST(TheRefetchIntervalDoesNotDependOnTheProgramsValidity) {
    Bench bench;
    ProgramSpec shortLived = twoEntryProgram(bench);
    shortLived.compiledAt = "2026-10-05T12:04:30Z";
    shortLived.expiresAt = "2026-10-05T12:06:00Z";
    bench.network.programEnvelope = getEnvelope(signedProgram(shortLived));
    bench.tick();
    const std::string programRoute = std::string("/api/v1/fallback-programs/") + kFppUuid;
    auto fetches = [&] {
        std::size_t n = 0;
        for (const Sent& s : bench.network.sent("GET", programRoute)) n += s.url.find("/fallback-state") == std::string::npos;
        return n;
    };
    // Ninety seconds of validity and still sixty seconds between fetches.
    for (int i = 0; i < 11; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(fetches(), static_cast<std::size_t>(1));
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(fetches(), static_cast<std::size_t>(2));
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
    CHECK_EQ(status.state.playlistName, std::string(kPlaylistName));
    CHECK_EQ(status.state.packageId, std::string("pkg-test"));
    CHECK_EQ(status.state.cutoffAt, std::string("2026-10-05T12:15:00Z"));
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
    CHECK_EQ(bench.notifier.raised.back(), bench.activeNotice());
    const showmesh::json::Value file = parseJson(readFile(bench.stateDir() + "/" + kFallbackStatusFilename));
    CHECK_EQ(member(file, "state"), std::string("fallback"));
    CHECK_EQ(member(file, "playlistName"), std::string(kPlaylistName));
    CHECK_EQ(member(file, "cutoffAt"), std::string("2026-10-05T12:15:00Z"));
    CHECK_EQ(member(file, "message"), bench.activeNotice());
    const showmesh::json::Value& lastRecord = child(file, "recentActivations").items().back();
    CHECK_EQ(member(lastRecord, "outcome"), std::string("authorized"));
    CHECK_EQ(member(lastRecord, "answeredBy"), std::string("node"));
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
    CHECK_EQ(records[0].outcome, std::string(kOutcomeUnknownEntry));
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
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeNoProgram));
}

TEST(AnExpiredProgramSendsNothingAndRecordsWhy) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    gClock = kCompiledAtMillis + 15 * 60 * 1000;
    bench.callback("playing", 0);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeProgramExpired));
}

TEST(AProgramThatDoesNotCarryThisPlayersKeySendsNothingAndRecordsWhy) {
    Bench bench;
    ProgramSpec noKey = twoEntryProgram(bench);
    noKey.executorPublicKey.clear();
    bench.writeFile(bench.installPath(), signedProgram(noKey));
    bench.loseCoordinator();
    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomePlayerKeyNotInProgram));

    ProgramSpec otherKey = twoEntryProgram(bench);
    otherKey.executorPublicKey = member(parseJson(fixture("keys.json")), "otherExecutorPublicKey");
    bench.writeFile(bench.installPath(), signedProgram(otherKey));
    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomePlayerKeyNotInProgram));
}

TEST(AnUnpairedPlayerWithNoKeySendsNothingAndRecordsWhy) {
    Bench bench(/*paired=*/false);
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    bench.callback("playing", 0);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeNoPlayerKey));
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

// --- the notice says what is true -------------------------------------------

namespace {

bool isOperatorCopy(const std::string& message) {
    bool ok = (message.rfind("The coordinator", 0) == 0 || message.rfind("This player", 0) == 0) &&
              message.back() == '.';
    for (const char* word : {"fallback", "interlock", "revision", "epoch", "evidence", "boundary", "armed", "ADR",
                             "executor", "program", "(", "kProgram", "kNo", "resting"}) {
        ok = ok && message.find(word) == std::string::npos;
    }
    // One or two sentences.
    std::size_t sentences = 0;
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (message[i] == '.' && (i + 1 == message.size() || message[i + 1] == ' ')) ++sentences;
    }
    return ok && sentences >= 1 && sentences <= 2;
}

}  // namespace

TEST(EveryNoticeIsOperatorCopyAndNamesThePlaylistAndTheCutoffWhileItHoldsOne) {
    NoticeFacts quiet;
    CHECK(FallbackNotice(quiet).empty());
    for (FallbackMode mode : {FallbackMode::kNormal, FallbackMode::kFallback, FallbackMode::kResting}) {
        for (BoundaryResult result : {BoundaryResult::kNone, BoundaryResult::kStarted, BoundaryResult::kNothingToStart,
                                      BoundaryResult::kStartedOnSomeNodes, BoundaryResult::kNotStarted}) {
            for (bool reachable : {false, true}) {
                NoticeFacts facts;
                facts.state.mode = mode;
                facts.state.playlistName = "Main Show";
                facts.state.cutoffAt = "2026-10-05T12:15:00Z";
                facts.state.lastBoundary = result;
                facts.coordinatorLost = true;
                facts.coordinatorReachable = reachable;
                CHECK(isOperatorCopy(FallbackNotice(facts)));
                for (const PlayerOutcomeCopy& copy : kPlayerOutcomeCopy) {
                    facts.problem = copy.word;
                    CHECK(isOperatorCopy(FallbackNotice(facts)));
                }
            }
        }
    }
    NoticeFacts resting;
    resting.state.mode = FallbackMode::kResting;
    resting.state.playlistName = "Main Show";
    resting.state.cutoffAt = "2026-10-05T12:15:00Z";
    CHECK(FallbackNotice(resting).find("playlist Main Show") != std::string::npos);
    CHECK(FallbackNotice(resting).find("12:15 UTC on 2026-10-05") != std::string::npos);
}

TEST(EveryReasonThisPlayerRecordsIsAWholeSentenceWithAnAction) {
    for (const PlayerOutcomeCopy& copy : kPlayerOutcomeCopy) {
        const std::string reason = copy.reason;
        CHECK(reason[0] >= 'A' && reason[0] <= 'Z');
        CHECK(reason.back() == '.');
        CHECK(reason.find(". ") != std::string::npos);
        // The word has the shape of a node's word.
        for (char c : std::string(copy.word)) CHECK((c >= 'a' && c <= 'z') || c == '-');
    }
}

TEST(HealthyFallbackSaysThisPlayerIsStartingThePlannedCuesForThePlaylistUntilTheCutoff) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    CHECK_EQ(bench.executor->notice(), std::string(kCoordinatorLostMessage));
    bench.callback("playing", 0);
    CHECK_EQ(bench.executor->notice(), bench.activeNotice());
    CHECK(bench.activeNotice().find("playlist Main Show") != std::string::npos);
    CHECK(bench.activeNotice().find("12:15 UTC on 2026-10-05") != std::string::npos);
    CHECK_EQ(bench.statusMessage(), bench.activeNotice());

    // An entry the plan maps to no cue is ordinary and does not change the notice.
    bench.callback("query_next", 0);
    bench.callback("playing", 2);
    CHECK_EQ(bench.executor->notice(), bench.activeNotice());
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeUnknownEntry));
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
}

TEST(APlanWithoutThisPlayersKeyNeverSaysCuesWillBeOrAreBeingStarted) {
    Bench bench;
    ProgramSpec noKey = twoEntryProgram(bench);
    noKey.executorPublicKey.clear();
    bench.writeFile(bench.installPath(), signedProgram(noKey));
    bench.loseCoordinator();
    const std::string cannot =
        std::string(kCannotStartPrefix) + "its plan does not carry this player's key" + kRestoreCoordinatorAction;
    CHECK_EQ(bench.executor->notice(), cannot);

    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK_EQ(bench.executor->notice(), cannot);
    CHECK_EQ(bench.statusMessage(), cannot);
    for (const std::string& raised : bench.notifier.raised) {
        CHECK(raised.find("will start") == std::string::npos);
        CHECK(raised.find("is starting") == std::string::npos);
    }
}

TEST(OnConfirmedLossAPlayerThatHoldsNothingToSendSaysItCannotStartThePlannedCues) {
    {
        Bench noPinnedKey;
        noPinnedKey.pinnedKey = PinnedKeyLoadResult();
        noPinnedKey.makeExecutor();
        noPinnedKey.writeFile(noPinnedKey.installPath(), signedProgram(twoEntryProgram(noPinnedKey)));
        noPinnedKey.loseCoordinator();
        CHECK_EQ(noPinnedKey.executor->notice(), std::string(kCannotStartPrefix) +
                                                     "it has no coordinator key to check a plan with" +
                                                     kRestoreCoordinatorAction);
    }
    {
        Bench noProgram;
        noProgram.loseCoordinator();
        CHECK_EQ(noProgram.executor->notice(), std::string(kCannotStartPrefix) +
                                                   "it holds no plan for running the show without the coordinator" +
                                                   kRestoreCoordinatorAction);
        CHECK_EQ(noProgram.statusMessage(), noProgram.executor->notice());
    }
    {
        Bench noPlayerKey(/*paired=*/false);
        noPlayerKey.writeFile(noPlayerKey.installPath(), signedProgram(twoEntryProgram(noPlayerKey)));
        noPlayerKey.loseCoordinator();
        CHECK_EQ(noPlayerKey.executor->notice(), std::string(kCannotStartPrefix) +
                                                     "it has no key of its own for the nodes" +
                                                     kRestoreCoordinatorAction);
    }
}

TEST(ABoundaryNoNodeAcceptedSaysSoAndTheNextOneThatStartsRestoresTheNotice) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.network.scriptActivation(kNodeA, nodeAnswer(403, "cue-not-authorized"));
    bench.loseCoordinator();
    bench.callback("playing", 0);
    CHECK_EQ(bench.executor->notice(), std::string(kNotStartingPrefix) + "no node started the last planned cue" +
                                           kRestoreCoordinatorAction);

    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    CHECK_EQ(bench.executor->notice(), bench.activeNotice());
}

TEST(ACueStartedOnOnlySomeNodesSaysSo) {
    Bench bench;
    ProgramSpec spec;
    spec.entries = {{bench.entryKey(0), "cue-a", 3, {{"node-a", kNodeA}, {"node-b", kNodeB}}}};
    bench.writeFile(bench.installPath(), signedProgram(spec));
    bench.network.nodesDown.insert(kNodeB);
    bench.loseCoordinator();
    bench.callback("playing", 0);
    CHECK_EQ(bench.executor->notice(), std::string(kStartedOnSomeNodesMessage));
    CHECK_EQ(bench.statusMessage(), std::string(kStartedOnSomeNodesMessage));
}

TEST(ActivationRecordsAreNotPushedOutByProgramHandOffs) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    bench.callback("playing", 0);
    bench.callback("playing", 0, std::nullopt, "");

    // The coordinator returns and publishes a changed program every minute for two hours.
    bench.network.coordinatorUp = true;
    for (int minute = 0; minute < 120; ++minute) {
        ProgramSpec changed = twoEntryProgram(bench);
        changed.revision = "rev-" + std::to_string(minute);
        changed.expiresAt = "2026-10-05T23:00:00Z";
        bench.network.programEnvelope = getEnvelope(signedProgram(changed));
        for (int i = 0; i < 12; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    }
    const FallbackStatusSnapshot status = bench.executor->status();
    CHECK_EQ(status.recentProgramHandOffs.size(), kRecentProgramHandOffRecords);
    CHECK_EQ(status.recentActivations.size(), static_cast<std::size_t>(1));
    CHECK_EQ(status.recentActivations[0].outcome, std::string("authorized"));
    const showmesh::json::Value file = parseJson(readFile(bench.stateDir() + "/" + kFallbackStatusFilename));
    CHECK_EQ(child(file, "recentActivations").items().size(), static_cast<std::size_t>(1));
}

// --- the three states, the report, the cutoff and the hand-back ---------------

namespace {

const std::string kStateRoute = std::string("/api/v1/fallback-programs/") + kFppUuid + "/fallback-state";
const std::string kProgramRoute = std::string("/api/v1/fallback-programs/") + kFppUuid;
const std::string kAcknowledgeRoute = kProgramRoute + "/acknowledge";
const std::string kNodeProgramRoute = std::string(kNodeProgramPathPrefix) + kFppUuid;

std::vector<showmesh::json::Value> stateReports(Bench* bench) {
    std::vector<showmesh::json::Value> reports;
    for (const Sent& sent : bench->network.sent("PUT", "/fallback-state")) reports.push_back(parseJson(sent.body));
    return reports;
}

double numberMember(const showmesh::json::Value& object, const char* name) {
    const showmesh::json::Value* v = detail::findMember(object, name);
    return v != nullptr && v->type() == showmesh::json::Type::kNumber ? v->number() : -1;
}

bool hasMember(const showmesh::json::Value& object, const char* name) {
    return detail::findMember(object, name) != nullptr;
}

// Everything the network saw from index `from` on, in order.
std::vector<std::string> orderSince(Bench* bench, std::size_t from) {
    const std::vector<std::string> all = bench->network.order();
    return std::vector<std::string>(all.begin() + static_cast<std::ptrdiff_t>(std::min(from, all.size())), all.end());
}

// A missing element reads as empty, so a regression fails a check instead of crashing the suite.
std::string at(const std::vector<std::string>& lines, std::size_t index) {
    return index < lines.size() ? lines[index] : std::string();
}

showmesh::json::Value lastReport(Bench* bench) {
    const std::vector<showmesh::json::Value> reports = stateReports(bench);
    CHECK(!reports.empty());
    return reports.empty() ? showmesh::json::Value() : reports.back();
}

Sent lastSent(Bench* bench, const std::string& method, const std::string& urlPart) {
    const std::vector<Sent> sent = bench->network.sent(method, urlPart);
    CHECK(!sent.empty());
    return sent.empty() ? Sent() : sent.back();
}

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) out += line + "\n";
    return out;
}

// A bench already in fallback under the bench playlist, entered at entry 0.
void enterFallback(Bench* bench) {
    bench->writeFile(bench->installPath(), signedProgram(twoEntryProgram(*bench)));
    bench->loseCoordinator();
    bench->callback("playing", 0);
    CHECK(bench->executor->status().state.mode == FallbackMode::kFallback);
    CHECK_EQ(bench->activations().size(), static_cast<std::size_t>(1));
}

void stopPlaylist(Bench* bench) { bench->callback("playing", 0, std::nullopt, ""); }

// A plugin restart: a new executor and runtime over the same directories.
void restartPlugin(Bench* bench, const std::string& fppIsPlaying) {
    bench->playingPlaylistAtStart = fppIsPlaying;
    bench->makeExecutor();
}

}  // namespace

TEST(TheStateIsReportedAtOnceOnStartThenEveryTenSecondsAndNeverWhileTheProbeFails) {
    Bench bench;
    bench.tick();
    std::vector<showmesh::json::Value> reports = stateReports(&bench);
    CHECK_EQ(reports.size(), static_cast<std::size_t>(1));
    if (reports.empty()) return;
    CHECK_EQ(numberMember(reports[0], "schemaVersion"), 1.0);
    CHECK_EQ(numberMember(reports[0], "sequence"), 1.0);
    CHECK_EQ(member(reports[0], "bootId"), bench.executor->bootId());
    CHECK_EQ(bench.executor->bootId().size(), static_cast<std::size_t>(36));
    CHECK_EQ(member(reports[0], "state"), std::string("normal"));
    CHECK_EQ(member(reports[0], "since"), std::string("2026-10-05T12:05:00Z"));
    for (const char* absent : {"playlistName", "packageId", "packageRevision", "cutoffAt"}) {
        CHECK(!hasMember(reports[0], absent));
    }
    const Sent sent = lastSent(&bench, "PUT", "/fallback-state");
    CHECK_EQ(sent.url, std::string(kCoordinatorUrl) + kStateRoute);
    CHECK_EQ(sent.bearerToken, std::string("token-one"));
    CHECK_EQ(sent.timeoutMillis, kStateReportTimeoutMillis);
    // The report is the first request after the probe, before the key and the program.
    const std::vector<std::string> order = bench.network.order();
    CHECK_EQ(at(order, 0), std::string("GET /healthz"));
    CHECK_EQ(at(order, 1), "PUT " + kStateRoute);

    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(stateReports(&bench).size(), static_cast<std::size_t>(1));
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    reports = stateReports(&bench);
    CHECK_EQ(reports.size(), static_cast<std::size_t>(2));
    if (reports.size() < 2) return;
    CHECK_EQ(numberMember(reports[1], "sequence"), 2.0);

    bench.network.coordinatorUp = false;
    for (int i = 0; i < 6; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(stateReports(&bench).size(), static_cast<std::size_t>(2));

    // A probe that succeeds after one that failed: the report goes at once and goes first.
    bench.network.coordinatorUp = true;
    const std::size_t mark = bench.network.order().size();
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    const std::vector<std::string> after = orderSince(&bench, mark);
    CHECK_EQ(at(after, 0), std::string("GET /healthz"));
    CHECK_EQ(at(after, 1), "PUT " + kStateRoute);
    CHECK_EQ(stateReports(&bench).size(), static_cast<std::size_t>(3));
}

TEST(AnUnpairedPluginSendsNoReportUntilItHoldsAToken) {
    Bench bench(/*paired=*/false);
    for (int i = 0; i < 5; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(stateReports(&bench).size(), static_cast<std::size_t>(0));
    CHECK(bench.executor->status().stateReportProblem.empty());

    bench.pair("token-one");
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(stateReports(&bench).size(), static_cast<std::size_t>(1));
}

TEST(NoAnswerToAStateReportChangesTheStateAndThePluginKeepsReporting) {
    struct Case {
        int status;
        bool recorded;
        bool problemExpected;
    };
    for (const Case& c : {Case{200, true, false}, Case{200, false, false}, Case{400, true, true},
                          Case{401, true, true}, Case{403, true, true}, Case{409, true, true},
                          Case{404, true, true}, Case{500, true, true}}) {
        Bench bench;
        enterFallback(&bench);
        bench.network.coordinatorUp = true;
        bench.network.stateReportStatus = c.status;
        bench.network.stateReportRecorded = c.recorded;
        bench.advanceAndTick(kHypothesisProbeIntervalMillis);

        CHECK_EQ(stateReports(&bench).size(), static_cast<std::size_t>(1));
        CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
        const std::string problem = bench.executor->status().stateReportProblem;
        CHECK_EQ(!problem.empty(), c.problemExpected);
        if (c.status == 404) CHECK(problem.find("too old") != std::string::npos);
        if (c.problemExpected && c.status != 404) {
            CHECK_EQ(problem, "The coordinator answered " + std::to_string(c.status) +
                                  " to this player's state: the coordinator says why");
        }
        const showmesh::json::Value file = parseJson(readFile(bench.stateDir() + "/" + kFallbackStatusFilename));
        CHECK_EQ(member(file, "stateReportProblem"), problem);

        // Ten seconds later it reports again, and still delivers.
        bench.advanceAndTick(kStateReportIntervalMillis);
        CHECK_EQ(stateReports(&bench).size(), static_cast<std::size_t>(2));
        bench.callback("query_next", 0);
        bench.callback("playing", 1);
        CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(2));
    }
}

TEST(FallbackIsEnteredOnlyWithAUsableProgramAndAMappedEntry) {
    {
        Bench unknownRule;
        ProgramSpec spec = twoEntryProgram(unknownRule);
        spec.restHold = "fade-to-black";
        unknownRule.writeFile(unknownRule.installPath(), signedProgram(spec));
        unknownRule.loseCoordinator();
        unknownRule.callback("playing", 0);
        CHECK(unknownRule.executor->status().state.mode == FallbackMode::kNormal);
        CHECK_EQ(unknownRule.activations().size(), static_cast<std::size_t>(0));
        CHECK_EQ(unknownRule.lastActivationRecord().outcome, std::string(kOutcomeProgramRulesUnknown));
        CHECK_EQ(unknownRule.executor->notice(),
                 std::string(kCannotStartPrefix) + "its plan asks for something this plugin version does not know" +
                     kRestoreCoordinatorAction);
        CHECK(!std::filesystem::exists(unknownRule.statePath()));
    }
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    // The plan does not map entry 2: the player stays out of fallback and records why.
    bench.callback("playing", 2);
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeUnknownEntry));
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
    CHECK(!std::filesystem::exists(bench.statePath()));

    bench.callback("query_next", 2);
    bench.callback("playing", 1);
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
}

TEST(AtTheCutoffThePluginRestsSendsNothingMoreAndSaysSo) {
    Bench bench;
    enterFallback(&bench);

    // The cutoff is the expiry of the copy it entered with, checked at every probe.
    gClock = kCompiledAtMillis + 15 * 60 * 1000;
    bench.tick();
    FallbackStatusSnapshot status = bench.executor->status();
    CHECK(status.state.mode == FallbackMode::kResting);
    CHECK_EQ(status.state.sinceMillis, gClock);
    const std::string notice = bench.executor->notice();
    CHECK(notice.find("This player stopped starting cues for playlist Main Show at 12:15 UTC on 2026-10-05") == 0);
    CHECK_EQ(bench.statusMessage(), notice);
    const showmesh::json::Value file = parseJson(readFile(bench.stateDir() + "/" + kFallbackStatusFilename));
    CHECK_EQ(member(file, "state"), std::string("resting"));
    CHECK_EQ(member(file, "cutoffAt"), std::string("2026-10-05T12:15:00Z"));

    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(0));
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string(kOutcomeResting));
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));

    // The coordinator answering again changes nothing but the report: no fetch, still resting.
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK(bench.executor->status().state.mode == FallbackMode::kResting);
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(0));
    const showmesh::json::Value report = lastReport(&bench);
    CHECK_EQ(member(report, "state"), std::string("resting"));
    CHECK_EQ(member(report, "playlistName"), std::string(kPlaylistName));
    CHECK_EQ(member(report, "packageId"), std::string("pkg-test"));
    CHECK_EQ(member(report, "packageRevision"), std::string("rev-test"));
    CHECK_EQ(member(report, "cutoffAt"), std::string("2026-10-05T12:15:00Z"));
    CHECK_EQ(member(report, "since"), std::string("2026-10-05T12:15:00Z"));

    stopPlaylist(&bench);
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
}

TEST(AnEntryThatStartsAtOrAfterTheCutoffGetsNoActivationEvenBeforeAnyProbeNoticed) {
    Bench bench;
    enterFallback(&bench);
    gClock = kCompiledAtMillis + 15 * 60 * 1000;
    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    CHECK(bench.executor->status().state.mode == FallbackMode::kResting);
}

TEST(ACoordinatorThatComesBackDoesNotTakeTheShowBackBeforeThePlaylistStops) {
    Bench bench;
    enterFallback(&bench);

    // The coordinator restarts and answers again in the middle of the playlist.
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    const std::size_t mark = bench.network.order().size();
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    // The probe and the report, and nothing else: no registration, no fetch, no hand-out.
    CHECK_EQ(joined(orderSince(&bench, mark)), "GET /healthz\nPUT " + kStateRoute + "\n");
    const showmesh::json::Value report = lastReport(&bench);
    CHECK_EQ(member(report, "state"), std::string("fallback"));
    CHECK_EQ(member(report, "playlistName"), std::string(kPlaylistName));
    CHECK_EQ(member(report, "packageId"), std::string("pkg-test"));
    CHECK_EQ(member(report, "packageRevision"), std::string("rev-test"));
    CHECK_EQ(member(report, "cutoffAt"), std::string("2026-10-05T12:15:00Z"));
    CHECK_EQ(bench.executor->notice(), bench.activeNotice(/*coordinatorAnswering=*/true));

    // This player is still the one executor: it activates, and posts no observation.
    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(2));
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));
    CHECK(bench.executor->coordinatorLost());
    for (int i = 0; i < 4; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(member(lastReport(&bench), "state"), std::string("fallback"));
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(0));
}

TEST(WhenThePlaylistStopsThePluginHandsBackReportThenFetchThenAcknowledge) {
    Bench bench;
    enterFallback(&bench);
    const std::string held = readFile(bench.installPath());
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(held);
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK(std::filesystem::exists(bench.statePath()));

    const std::size_t mark = bench.network.order().size();
    stopPlaylist(&bench);
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    // The report has already gone, from the worker, before anything else.
    CHECK_EQ(joined(orderSince(&bench, mark)), "PUT " + kStateRoute + "\n");
    CHECK(!std::filesystem::exists(bench.statePath()));
    const showmesh::json::Value report = lastReport(&bench);
    CHECK_EQ(member(report, "state"), std::string("normal"));
    CHECK(!hasMember(report, "playlistName"));
    CHECK(!hasMember(report, "cutoffAt"));

    // Then the fetch at once, and the acknowledgement although the copy did not change.
    bench.tick();
    CHECK_EQ(joined(orderSince(&bench, mark)),
             "PUT " + kStateRoute + "\nGET " + kProgramRoute + "\nPOST " + kAcknowledgeRoute + "\nPUT " +
                 kNodeProgramRoute + "\n");
    const showmesh::json::Value ack = parseJson(lastSent(&bench, "POST", "/acknowledge").body);
    CHECK_EQ(member(ack, "packageId"), std::string("pkg-test"));
    CHECK_EQ(member(ack, "revision"), std::string("rev-test"));
    CHECK_EQ(member(ack, "verificationResult"), std::string("verified"));
    CHECK_EQ(readFile(bench.installPath()), held);
    CHECK(bench.executor->notice().empty());

    // Nothing was ever posted for an entry that began in fallback, the stop included.
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));
    // The next callback is the coordinator's again: observed, not activated.
    bench.callback("playing", 0);
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    // The hand-back fetch is done once, not at every tick.
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
}

TEST(AHandBackFetchThatFindsANewerProgramInstallsAcknowledgesAndHandsItOut) {
    Bench bench;
    enterFallback(&bench);
    ProgramSpec newer = twoEntryProgram(bench);
    newer.revision = "rev-newer";
    newer.packageId = "pkg-newer";
    const std::string newerDocument = signedProgram(newer);
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(newerDocument);
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    // Not while it is the executor.
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(0));

    stopPlaylist(&bench);
    bench.tick();
    CHECK_EQ(readFile(bench.installPath()), newerDocument);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
    CHECK_EQ(member(parseJson(lastSent(&bench, "POST", "/acknowledge").body), "revision"), std::string("rev-newer"));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(1));
}

TEST(ACallbackThatNamesADifferentPlaylistIsTheBoundaryToo) {
    Bench bench;
    enterFallback(&bench);
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(readFile(bench.installPath()));
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);

    const std::size_t mark = bench.network.order().size();
    bench.callback("playing", 0, std::nullopt, "Other Show");
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    // The report first, then the observation of the entry that began after the hand-back.
    CHECK_EQ(joined(orderSince(&bench, mark)), "PUT " + kStateRoute + "\nOBSERVE\n");
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));

    // A new pass of the same playlist is not a boundary.
    Bench looping;
    enterFallback(&looping);
    looping.callback("query_next", 0, 0);
    looping.callback("playing", 0, 1);
    CHECK(looping.executor->status().state.mode == FallbackMode::kFallback);
    CHECK_EQ(looping.activations().size(), static_cast<std::size_t>(2));
}

TEST(WithTheCoordinatorStillLostAtTheBoundaryThePluginHandsBackOnTheFirstGoodProbe) {
    Bench bench;
    enterFallback(&bench);
    const std::size_t reportsBefore = stateReports(&bench).size();
    stopPlaylist(&bench);
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK(!std::filesystem::exists(bench.statePath()));
    // Nothing is sent to a coordinator that does not answer, and nothing is posted yet.
    CHECK_EQ(stateReports(&bench).size(), reportsBefore);
    CHECK(bench.executor->coordinatorLost());

    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(readFile(bench.installPath()));
    const std::size_t mark = bench.network.order().size();
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    const std::vector<std::string> after = orderSince(&bench, mark);
    CHECK_EQ(at(after, 0), std::string("GET /healthz"));
    CHECK_EQ(at(after, 1), "PUT " + kStateRoute);
    CHECK_EQ(at(after, 2), "GET " + kProgramRoute);
    CHECK_EQ(at(after, 3), "POST " + kAcknowledgeRoute);
    CHECK_EQ(member(lastReport(&bench), "state"), std::string("normal"));
    CHECK(!bench.executor->coordinatorLost());
}

TEST(AnOutageThatOutlastsAPlaylistEntersFallbackAgainUnderTheNextOne) {
    Bench bench;
    enterFallback(&bench);
    stopPlaylist(&bench);
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);

    // FPP starts the playlist again while the coordinator is still lost.
    bench.callback("playing", 0);
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
    const std::vector<Sent> activations = bench.activations();
    CHECK_EQ(activations.size(), static_cast<std::size_t>(2));
    CHECK_NE(requestMember(activations[0], "executionId"), requestMember(activations[1], "executionId"));
    CHECK(std::filesystem::exists(bench.statePath()));
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));
}

TEST(ASecondOutageAfterAHandBackRunsTheWholeCycleAgainWithNoEntryStartedTwice) {
    Bench bench;
    ProgramSpec longLived = twoEntryProgram(bench);
    longLived.expiresAt = "2026-10-05T23:00:00Z";
    const std::string program = signedProgram(longLived);
    bench.network.programEnvelope = getEnvelope(program);
    bench.tick();

    // First outage: entry 0 is this player's.
    bench.loseCoordinator();
    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));

    // The coordinator returns, the playlist stops, the player hands back.
    bench.network.coordinatorUp = true;
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    stopPlaylist(&bench);
    bench.tick();
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);

    // Normal again: entry 0 of the next run is the coordinator's.
    bench.callback("playing", 0);
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));

    // Second outage, in the middle of that run: entry 1 is this player's.
    bench.loseCoordinator();
    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    const std::vector<Sent> activations = bench.activations();
    CHECK_EQ(activations.size(), static_cast<std::size_t>(2));
    CHECK_NE(requestMember(activations[0], "executionId"), requestMember(activations[1], "executionId"));
    CHECK_EQ(requestMember(activations[1], "entryKey"), bench.entryKey(1));
    // Each entry had exactly one owner: one observation or one activation, never both.
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.sink.published[0].entryKey, bench.entryKey(0));
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
}

TEST(ProbingWhileTheCoordinatorIsLostIsNeverLessOftenThanEveryTenSeconds) {
    Bench bench;
    bench.detectorConfig.probeIntervalMillis = 30000;
    bench.makeExecutor();
    bench.tick();
    bench.network.coordinatorUp = false;
    for (int i = 0; i < 3; ++i) bench.advanceAndTick(30000);
    CHECK(bench.executor->coordinatorLost());

    const std::size_t before = bench.network.count("GET", "/healthz");
    for (int second = 0; second < 600; ++second) bench.advanceAndTick(1000);
    // Ten minutes lost: a probe every ten seconds, with no backoff.
    CHECK_EQ(bench.network.count("GET", "/healthz") - before, static_cast<std::size_t>(60));

    bench.network.coordinatorUp = true;
    bench.advanceAndTick(10000);
    const std::size_t healthy = bench.network.count("GET", "/healthz");
    for (int second = 0; second < 60; ++second) bench.advanceAndTick(1000);
    CHECK_EQ(bench.network.count("GET", "/healthz") - healthy, static_cast<std::size_t>(2));
}

// --- the saved state and a plugin restart -------------------------------------

TEST(TheStateIsOnDiskBesideTheTokenBeforeTheFirstActivationLeaves) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    std::string onDiskAtSend;
    int modeAtSend = 0;
    bench.network.beforeAnswer = [&](const std::string& method, const std::string& url) {
        if (method != "POST" || url.find(kNodeActivationPath) == std::string::npos) return;
        onDiskAtSend = readFile(bench.statePath());
        struct ::stat info {};
        if (::stat(bench.statePath().c_str(), &info) == 0) modeAtSend = static_cast<int>(info.st_mode & 07777);
    };
    bench.callback("playing", 0, 3);

    if (bench.activations().empty()) {
        CHECK(false);
        return;
    }
    const std::string executionId = requestMember(bench.activations()[0], "executionId");
    FallbackExecutionState atSend;
    CHECK(ParseFallbackState(onDiskAtSend, &atSend));
    CHECK_EQ(modeAtSend, 0600);
    CHECK(atSend.mode == FallbackMode::kFallback);
    CHECK_EQ(atSend.playlistName, std::string(kPlaylistName));
    CHECK_EQ(atSend.sinceMillis, gClock);
    CHECK_EQ(atSend.occurrence.entryKey, bench.entryKey(0));
    CHECK_EQ(atSend.occurrence.playlistLoop.value_or(-1), 3);
    CHECK(!atSend.occurrence.delivered);
    CHECK_EQ(atSend.occurrence.executionIds.size(), static_cast<std::size_t>(1));
    if (atSend.occurrence.executionIds.empty()) return;
    CHECK_EQ(atSend.occurrence.executionIds[0].first, std::string("node-a"));
    CHECK_EQ(atSend.occurrence.executionIds[0].second, executionId);

    FallbackExecutionState after;
    CHECK(LoadFallbackState(bench.credentialDir(), &after));
    CHECK(after.occurrence.delivered);
    CHECK(!std::filesystem::exists(bench.statePath() + ".tmp"));
}

TEST(APluginRestartInFallbackResumesItUnderTheSamePlaylistAndDoesNotSendTheEntryAgain) {
    Bench bench;
    enterFallback(&bench);
    const FallbackExecutionState before = bench.executor->status().state;
    const std::string firstBoot = bench.executor->bootId();
    const std::string firstId = requestMember(bench.activations()[0], "executionId");

    // The plugin restarts while FPP keeps playing the same playlist.
    gClock += 30000;
    restartPlugin(&bench, kPlaylistName);
    const FallbackExecutionState resumed = bench.executor->status().state;
    CHECK(resumed.mode == FallbackMode::kFallback);
    CHECK_EQ(resumed.playlistName, before.playlistName);
    CHECK_EQ(resumed.sinceMillis, before.sinceMillis);
    CHECK_EQ(resumed.cutoffAt, before.cutoffAt);
    CHECK_NE(bench.executor->bootId(), firstBoot);
    // From its first moment it posts nothing: the coordinator must not start what this player started.
    CHECK(bench.executor->coordinatorLost());

    bench.tick();
    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));

    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    const std::vector<Sent> activations = bench.activations();
    CHECK_EQ(activations.size(), static_cast<std::size_t>(2));
    CHECK_NE(requestMember(activations[1], "executionId"), firstId);

    // Its report carries the state it left, under a new boot id.
    bench.network.coordinatorUp = true;
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    const showmesh::json::Value report = lastReport(&bench);
    CHECK_EQ(member(report, "state"), std::string("fallback"));
    CHECK_EQ(member(report, "playlistName"), std::string(kPlaylistName));
    CHECK_EQ(member(report, "since"), std::string("2026-10-05T12:05:20Z"));
    CHECK_EQ(member(report, "bootId"), bench.executor->bootId());
    CHECK_EQ(numberMember(report, "sequence"), 1.0);
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(0));
}

TEST(ARestartThatInterruptedAnEntryRetriesItWithTheSameExecutionIdsSoTheNodeRunsItOnce) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    std::string onDiskAtSend;
    bench.network.beforeAnswer = [&](const std::string& method, const std::string& url) {
        if (method == "POST" && url.find(kNodeActivationPath) != std::string::npos && onDiskAtSend.empty()) {
            onDiskAtSend = readFile(bench.statePath());
        }
    };
    bench.callback("playing", 0);
    const Sent first = bench.activations()[0];

    // The plugin died after the node ran the cue and before it could record that: the file says unfinished.
    bench.network.beforeAnswer = nullptr;
    bench.writeFile(bench.statePath(), onDiskAtSend);
    ::chmod(bench.statePath().c_str(), 0600);
    bench.network.scriptActivation(kNodeA, nodeAnswer(409, "replayed-execution", ",\"firstOutcome\":\"authorized\""));
    restartPlugin(&bench, kPlaylistName);
    bench.tick();

    const std::vector<Sent> activations = bench.activations();
    CHECK_EQ(activations.size(), static_cast<std::size_t>(2));
    CHECK_EQ(activations[1].body, first.body);
    CHECK_EQ(bench.lastActivationRecord().outcome, std::string("authorized"));
    FallbackExecutionState after;
    CHECK(LoadFallbackState(bench.credentialDir(), &after));
    CHECK(after.occurrence.delivered);

    // Done once: a later tick and FPP repeating the callback send nothing more.
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(2));
}

TEST(ARestartAfterTheCutoffDoesNotResumeEvenUnderTheSamePlaylistName) {
    // The name cannot tell this run from a later run of the playlist. The plan's expiry bounds a resume.
    for (bool restingWhenItStopped : {true, false}) {
        Bench bench;
        enterFallback(&bench);
        if (restingWhenItStopped) {
            gClock = kCompiledAtMillis + 15 * 60 * 1000;
            bench.tick();
            CHECK(bench.executor->status().state.mode == FallbackMode::kResting);
        }
        gClock = kCompiledAtMillis + 16 * 60 * 1000;
        restartPlugin(&bench, kPlaylistName);
        CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
        CHECK(!std::filesystem::exists(bench.statePath()));

        // One second before the cutoff the same restart resumes.
        Bench early;
        enterFallback(&early);
        gClock = kCompiledAtMillis + 15 * 60 * 1000 - 1000;
        restartPlugin(&early, kPlaylistName);
        CHECK(early.executor->status().state.mode == FallbackMode::kFallback);
    }
}

TEST(TheReportNamesThePlaylistAsFppsOwnStatusSpellsIt) {
    CHECK_EQ(FppStatusPlaylistName("Main Show"), std::string("Main Show"));
    CHECK_EQ(FppStatusPlaylistName("Main Show.json"), std::string("Main Show"));
    CHECK_EQ(FppStatusPlaylistName("/home/fpp/media/playlists/Main Show.json"), std::string("Main Show"));
    CHECK_EQ(FppStatusPlaylistName("Act 1.5"), std::string("Act 1.5"));

    StateReport report;
    report.bootId = "boot";
    report.sequence = 1;
    report.state.enter(0, "/home/fpp/media/playlists/Main Show.json", "pkg", "rev", "2026-10-05T12:15:00Z");
    CHECK_EQ(member(parseJson(StateReportBody(report)), "playlistName"), std::string("Main Show"));
}

TEST(ARestartAfterThePlaylistEndedHandsBackBecauseTheBoundaryPassedWhileThePluginWasDown) {
    for (const char* fppIsPlaying : {"", "Other Show"}) {
        Bench bench;
        enterFallback(&bench);
        const std::string held = readFile(bench.installPath());
        bench.network.coordinatorUp = true;
        bench.network.programEnvelope = getEnvelope(held);

        gClock += 30000;
        restartPlugin(&bench, fppIsPlaying);
        CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
        CHECK(!std::filesystem::exists(bench.statePath()));
        // Nothing is posted before the report that must come first.
        CHECK(bench.executor->coordinatorLost());

        const std::size_t mark = bench.network.order().size();
        bench.tick();
        const std::vector<std::string> after = orderSince(&bench, mark);
        CHECK_EQ(at(after, 0), std::string("GET /healthz"));
        CHECK_EQ(at(after, 1), "PUT " + kStateRoute);
        CHECK_EQ(at(after, 2), "GET " + kProgramRoute);
        CHECK_EQ(at(after, 3), "POST " + kAcknowledgeRoute);
        const showmesh::json::Value report = lastReport(&bench);
        CHECK_EQ(member(report, "state"), std::string("normal"));
        CHECK_EQ(member(report, "since"), std::string("2026-10-05T12:05:50Z"));
        CHECK(!bench.executor->coordinatorLost());
    }
}

TEST(AStateFileThatCannotBeReadMeansNormal) {
    for (const char* contents : {"", "not json", "{\"state\":\"fallback\"}", "{\"state\":\"dancing\",\"playlistName\":\"Main Show\","
                                 "\"sinceMillis\":1,\"packageId\":\"p\",\"packageRevision\":\"r\",\"cutoffAt\":\"c\"}"}) {
        Bench bench;
        bench.writeFile(bench.statePath(), contents);
        restartPlugin(&bench, kPlaylistName);
        CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
        bench.tick();
        CHECK_EQ(member(lastReport(&bench), "state"), std::string("normal"));
    }
}

TEST(APluginRestartInNormalStartsInNormalWithNothingSaved) {
    Bench bench;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    bench.tick();
    restartPlugin(&bench, kPlaylistName);
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK(!std::filesystem::exists(bench.statePath()));
    CHECK(!bench.executor->coordinatorLost());
    bench.tick();
    bench.callback("playing", 0);
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
}

TEST(TheSavedStateSurvivesBeingWrittenAndReadBack) {
    FallbackExecutionState state;
    state.enter(1234, "Main Show", "pkg", "rev", "2026-10-05T12:15:00Z");
    state.occurrence.present = true;
    state.occurrence.identityResolved = true;
    state.occurrence.entryKey = "key";
    state.occurrence.playlistLoop = 7;
    state.occurrence.executionIds = {{"node-a", "id-a"}, {"node-b", "id-b"}};
    state.rest(5678);
    FallbackExecutionState read;
    CHECK(ParseFallbackState(RenderFallbackState(state), &read));
    CHECK(read.mode == FallbackMode::kResting);
    CHECK_EQ(read.sinceMillis, static_cast<TimeMillis>(5678));
    CHECK_EQ(read.playlistName, std::string("Main Show"));
    CHECK_EQ(read.packageId + "/" + read.packageRevision + "/" + read.cutoffAt,
             std::string("pkg/rev/2026-10-05T12:15:00Z"));
    CHECK_EQ(read.occurrence.playlistLoop.value_or(-1), 7);
    CHECK_EQ(read.occurrence.executionIds.size(), static_cast<std::size_t>(2));
    CHECK_EQ(read.occurrence.executionIds[1].second, std::string("id-b"));
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
    CHECK_EQ(run.result.reason, std::string("The node did not answer: connection refused. Check the node and the "
                                            "network between it and this player."));
    CHECK(!run.result.nodeAnswered());
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
          "apply-failed", "cue-check-failed", "a-word-this-build-has-never-seen"}) {
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

TEST(StopIsHonoredBetweenEveryStepWhenTheCoordinatorAnswersTheProbeAndThenStalls) {
    Bench bench;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    std::mutex mutex;
    std::condition_variable changed;
    bool stalled = false;
    bool released = false;
    bench.network.beforeAnswer = [&](const std::string& method, const std::string& url) {
        if (method != "PUT" || url.find("/executor-key") == std::string::npos) return;
        std::unique_lock<std::mutex> lock(mutex);
        stalled = true;
        changed.notify_all();
        changed.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
    };

    bench.executor->start();
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(5), [&] { return stalled; }));
    }
    // Stop is asked for while the registration is stalled. When it returns,
    // the tick ends there: no fetch, no acknowledge, no hand-out.
    bench.executor->requestStop();
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
    }
    changed.notify_all();
    bench.executor->stop();

    CHECK_EQ(bench.network.count("GET", "/healthz"), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("PUT", "/executor-key"), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("GET", "/api/v1/fallback-programs/"), static_cast<std::size_t>(0));
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(0));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(0));
}

TEST(StopDuringTheProgramFetchSkipsTheAcknowledgeAndTheHandOuts) {
    Bench bench;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    bench.network.beforeAnswer = [&](const std::string& method, const std::string& url) {
        const bool programFetch = method == "GET" && url.find("/api/v1/fallback-programs/") != std::string::npos;
        if (programFetch) bench.executor->requestStop();
    };
    bench.tick();

    CHECK_EQ(bench.network.count("GET", "/api/v1/fallback-programs/"), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(0));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(0));
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
    // What FPP is playing is read once at construction, for a saved state to be resumed or handed back.
    CHECK(delivery.find("Player::INSTANCE.IsPlaying()") != std::string::npos);
    CHECK(delivery.find("options.playingPlaylistAtStart = Player::INSTANCE.GetPlaylistName();") != std::string::npos);
    CHECK(delivery.find("showmesh::fallback::FallbackExecutor executor_;") != std::string::npos);
    CHECK(delivery.find("recorder() { return &executor_; }") != std::string::npos);
}
