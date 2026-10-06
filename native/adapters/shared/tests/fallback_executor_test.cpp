// Tests for the ADR-048 fallback executor: the executor key, the outage
// detector, the refetch cadence, and delivery to nodes per contract section
// 5. Delivery is driven through ShowMeshRuntime::observeCallback(), the entry
// point both shipping adapters forward FPP's playlistCallback into.

#include <sys/stat.h>

#include <atomic>
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

// Atomic because some cases run the runtime worker and the executor's own thread.
std::atomic<TimeMillis> gClock{kCompiledAtMillis + kFiveMinutesMillis};
// How far the wall clock was stepped away from the time that really passed.
std::atomic<TimeMillis> gWallClockStep{0};
TimeMillis fixtureClock() { return gClock.load() + gWallClockStep.load(); }
// The same passage of time from another origin, as a monotonic clock has.
TimeMillis fixtureMonotonicClock() { return gClock.load() - kCompiledAtMillis; }

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

    // The next acknowledgements get these answers, in order, instead of 200.
    void scriptAcknowledge(HttpResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        acknowledgeScript_.push_back(std::move(response));
    }

    void scriptActivation(const std::string& address, HttpResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        activationScript_[address].push_back(std::move(response));
    }

    std::atomic<bool> coordinatorUp{true};
    // The coordinator answers its health probe while the program fetch gets no answer.
    std::atomic<bool> programFetchUnreachable{false};
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
            if (path.find("/acknowledge") != std::string::npos) {
                if (acknowledgeScript_.empty()) return answer(200, "{}");
                HttpResponse next = acknowledgeScript_.front();
                acknowledgeScript_.pop_front();
                return next;
            }
            if (path.find("/fallback-state") != std::string::npos) {
                if (stateReportStatus != 200) return answer(stateReportStatus, stateReportRefusalBody);
                return answer(200, std::string("{\"recorded\":") + (stateReportRecorded ? "true" : "false") +
                                       ",\"state\":\"normal\"}");
            }
            if (programFetchUnreachable.load()) return noResponse();
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
    std::deque<HttpResponse> acknowledgeScript_;
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
        gWallClockStep = 0;
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
        options.monotonicClock = fixtureMonotonicClock;
        if (writeState) options.writeState = writeState;
        options.fppInstanceUuid = kFppUuid;
        options.stateDir = stateDir();
        options.credentialDir = credentialDir();
        options.installPath = installPath();
        options.pinnedKey = pinnedKey;
        options.detector = detectorConfig;
        options.randomBytes = fixtureRandom;
        options.notifier = &notifier;
        options.log = [this](bool, const std::string& line) {
            std::lock_guard<std::mutex> lock(recordMutex);
            logs.push_back(line);
        };
        options.pause = [this](int millis) {
            std::lock_guard<std::mutex> lock(recordMutex);
            pauses.push_back(millis);
            if (onPause) onPause();
            return true;
        };
        sink.network = &network;
        executor = std::make_unique<FallbackExecutor>(options, &network);
        runtime = std::make_unique<ShowMeshRuntime>(&definitions, &sink, fixtureClock, nullptr, nullptr, nullptr,
                                                    showmesh::kDefaultSafeCeilingPercent, executor.get());
    }

    void tick() { executor->tick(fixtureClock()); }

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
    // The activation at index, or an empty one when it was never sent.
    Sent activation(std::size_t index) {
        const std::vector<Sent> all = activations();
        CHECK(index < all.size());
        return index < all.size() ? all[index] : Sent();
    }

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
    FakeNetwork network;
    RecordingNotifier notifier;
    FixedDefinitions definitions;
    CountingSink sink;
    std::mutex recordMutex;
    std::vector<std::string> logs;
    std::vector<int> pauses;
    // Runs inside every wait between delivery attempts.
    std::function<void()> onPause;
    // Stands in for the write of the state file in executors made after it is set.
    std::function<bool(const std::string&, const FallbackExecutionState&)> writeState;
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
    if (registrations.size() < 1) return;
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
    if (registrations.size() < 2) return;
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
    if (registrations.size() < 2) return;
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
    if (probes.size() < 1) return;
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
    if (handed.size() < 2) return;
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
    if (activations.size() < 1) return;
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
    if (records.size() < 1) return;
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
    if (activations.size() < 2) return;
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
    if (activations.size() < 2) return;
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
    if (records.size() < 1) return;
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
    if (activations.size() < 1) return;
    CHECK_EQ(activations[0].url, std::string("http://") + kNodeB + kNodeActivationPath);
    const std::vector<FallbackRecord> records = bench.activationRecords();
    CHECK_EQ(records.size(), static_cast<std::size_t>(2));
    if (records.size() < 2) return;
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
                facts.waitingForFpp = true;
                CHECK(isOperatorCopy(FallbackNotice(facts)));
                facts.waitingForFpp = false;
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
    CHECK_EQ(bench.executor->notice(),
             std::string("The coordinator stopped answering, and this player is not starting the planned cues for "
                         "playlist Main Show because no node started the last planned cue") +
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
    const std::string someNodes = "The coordinator stopped answering, and this player started the last planned cue for "
                                  "playlist Main Show on only some of its nodes. Check the nodes and restore the "
                                  "coordinator.";
    CHECK_EQ(bench.executor->notice(), someNodes);
    CHECK_EQ(bench.statusMessage(), someNodes);
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
// Nothing tells it what FPP is playing; only FPP's callbacks and time do.
void restartPlugin(Bench* bench) { bench->makeExecutor(); }

}  // namespace

TEST(TheStateIsReportedAtOnceOnStartThenEveryTenSecondsAndNeverWhileTheProbeFails) {
    Bench bench;
    bench.tick();
    std::vector<showmesh::json::Value> reports = stateReports(&bench);
    CHECK_EQ(reports.size(), static_cast<std::size_t>(1));
    if (reports.size() < 1) return;
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
            const char* action = c.status == 400   ? "Check that the coordinator and this plugin are versions "
                                                     "that work together."
                                 : c.status == 409 ? "Check that the coordinator can reach this player."
                                 : c.status == 500 ? "Check the coordinator; the plugin keeps sending this "
                                                     "player's state."
                                                   : "Check this player's pairing on the coordinator.";
            CHECK_EQ(problem, "The coordinator answered " + std::to_string(c.status) +
                                  " to this player's state: the coordinator says why. " + action);
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
    CHECK_EQ(status.state.sinceMillis, gClock.load());
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
    if (activations.size() < 2) return;
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
    if (activations.size() < 2) return;
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
    const std::string executionId = requestMember(bench.activation(0), "executionId");
    FallbackExecutionState atSend;
    CHECK(ParseFallbackState(onDiskAtSend, &atSend) == SavedStateRead::kLoaded);
    CHECK_EQ(modeAtSend, 0600);
    CHECK(atSend.mode == FallbackMode::kFallback);
    CHECK_EQ(atSend.playlistName, std::string(kPlaylistName));
    CHECK_EQ(atSend.sinceMillis, gClock.load());
    CHECK_EQ(atSend.occurrence.entryKey, bench.entryKey(0));
    CHECK_EQ(atSend.occurrence.playlistLoop.value_or(-1), 3);
    CHECK(!atSend.occurrence.delivered);
    CHECK_EQ(atSend.occurrence.executionIds.size(), static_cast<std::size_t>(1));
    if (atSend.occurrence.executionIds.empty()) return;
    CHECK_EQ(atSend.occurrence.executionIds[0].first, std::string("node-a"));
    CHECK_EQ(atSend.occurrence.executionIds[0].second, executionId);

    FallbackExecutionState after;
    CHECK(LoadFallbackState(bench.credentialDir(), &after) == SavedStateRead::kLoaded);
    CHECK(after.occurrence.delivered);
    CHECK(!std::filesystem::exists(bench.statePath() + ".tmp"));
}

// A restarted plugin is told nothing about what FPP is playing: FPP loads
// plugins before it starts any playlist. Every case below decides from the
// callbacks FPP then sends, through the runtime entry point, or from time.

TEST(APluginThatStartsWithASavedStateBehavesAsThatStateUntilFppDecidesIt) {
    Bench bench;
    enterFallback(&bench);
    const FallbackExecutionState before = bench.executor->status().state;
    const std::string firstBoot = bench.executor->bootId();
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));

    gClock += 2000;
    restartPlugin(&bench);
    // Undecided: for everything the coordinator sees it is still in the saved state.
    FallbackExecutionState now = bench.executor->status().state;
    CHECK(now.mode == FallbackMode::kFallback);
    CHECK_EQ(now.playlistName, before.playlistName);
    CHECK_EQ(now.sinceMillis, before.sinceMillis);
    CHECK(bench.executor->coordinatorLost());
    CHECK(std::filesystem::exists(bench.statePath()));

    const std::size_t mark = bench.network.order().size();
    bench.tick();
    // It reports the saved state after its first probe, and fetches, acknowledges and hands back nothing.
    CHECK_EQ(joined(orderSince(&bench, mark)), "GET /healthz\nPUT " + kStateRoute + "\n");
    const showmesh::json::Value report = lastReport(&bench);
    CHECK_EQ(member(report, "state"), std::string("fallback"));
    CHECK_EQ(member(report, "playlistName"), std::string(kPlaylistName));
    CHECK_EQ(member(report, "since"), std::string("2026-10-05T12:05:20Z"));
    CHECK_NE(member(report, "bootId"), firstBoot);
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));
    // What an operator reads says it is waiting, not that cues are being started.
    CHECK(bench.executor->status().waitingForFpp);
    const std::string waiting = "This player's plugin restarted while it was running playlist Main Show without the "
                                "coordinator, and it is waiting for FPP to say what it is playing. Nothing to do.";
    CHECK_EQ(bench.executor->notice(), waiting);
    CHECK_EQ(bench.statusMessage(), waiting);

    bench.callback("playing", 0);
    CHECK(!bench.executor->status().waitingForFpp);
    CHECK_EQ(bench.executor->notice(), bench.activeNotice(/*coordinatorAnswering=*/true));
}

TEST(TheFirstCallbackThatNamesTheSavedPlaylistResumesAndTheEntryAlreadyHandledGetsNoCue) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    // FPP was on its third pass of the playlist when fallback started entry 0's cue.
    bench.callback("playing", 0, 2);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    const std::string firstId = requestMember(bench.activation(0), "executionId");

    // fppd restarts and resumes the playlist at the same entry. Its pass counter starts over.
    gClock += 5000;
    restartPlugin(&bench);
    bench.tick();
    bench.callback("start", 0, 0);
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));
    // FPP repeating the callback for that entry changes nothing either.
    bench.callback("playing", 0, 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));

    // The next entry is a boundary like any other, with a new execution id.
    bench.callback("query_next", 0, 0);
    bench.callback("playing", 1, 0);
    const std::vector<Sent> activations = bench.activations();
    CHECK_EQ(activations.size(), static_cast<std::size_t>(2));
    if (activations.size() < 2) return;
    CHECK_NE(requestMember(activations[1], "executionId"), firstId);
    CHECK_EQ(requestMember(activations[1], "entryKey"), bench.entryKey(1));
}

TEST(AResumeIntoALaterEntryThanTheRecordedOneStartsThatEntrysCue) {
    Bench bench;
    enterFallback(&bench);
    gClock += 5000;
    restartPlugin(&bench);
    // FPP moved on while the plugin was down: this entry was never handled.
    bench.callback("playing", 1, 0);
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(2));
}

TEST(AnEntryARestartInterruptedIsNotTriedAgainAndIsRecordedAsSuch) {
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
    bench.network.beforeAnswer = nullptr;

    // The plugin died while that cue was being started: the file says unfinished.
    bench.writeFile(bench.statePath(), onDiskAtSend);
    ::chmod(bench.statePath().c_str(), 0600);
    gClock += 40000;
    restartPlugin(&bench);
    bench.tick();
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));

    // FPP is forty seconds into that entry. A cue started now would be worse than a missed one.
    bench.callback("playing", 0);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    const FallbackRecord record = bench.lastActivationRecord();
    CHECK_EQ(record.outcome, std::string(kOutcomeInterruptedByRestart));
    CHECK_EQ(record.entryKey, bench.entryKey(0));
    CHECK(!record.nodeAnswered);
    FallbackExecutionState after;
    CHECK(LoadFallbackState(bench.credentialDir(), &after) == SavedStateRead::kLoaded);
    CHECK(after.occurrence.delivered);

    bench.callback("query_next", 0);
    bench.callback("playing", 1);
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(2));
}

TEST(WhenFppStaysIdleForTheSettleWindowAfterAStartThePluginHandsBack) {
    Bench bench;
    enterFallback(&bench);
    const std::string held = readFile(bench.installPath());
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(held);

    gClock += 2000;
    restartPlugin(&bench);
    const TimeMillis startedAt = gClock;
    bench.tick();
    // One probe interval short of the window: still the saved state, nothing fetched.
    while (gClock + kHypothesisProbeIntervalMillis < startedAt + kHypothesisRestartSettleMillis) {
        bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    }
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(0));
    CHECK_EQ(member(lastReport(&bench), "state"), std::string("fallback"));
    CHECK(std::filesystem::exists(bench.statePath()));

    const std::size_t mark = bench.network.order().size();
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK(!std::filesystem::exists(bench.statePath()));
    const std::string order = joined(orderSince(&bench, mark));
    CHECK(order.find("PUT " + kStateRoute + "\nGET " + kProgramRoute + "\nPOST " + kAcknowledgeRoute) !=
          std::string::npos);
    CHECK_EQ(member(lastReport(&bench), "state"), std::string("normal"));
    CHECK(!bench.executor->coordinatorLost());

    // A playlist that starts now is the coordinator's.
    bench.callback("playing", 0);
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
}

TEST(ACallbackThatNamesAnotherPlaylistAfterAStartHandsBackAtOnce) {
    Bench bench;
    enterFallback(&bench);
    bench.network.coordinatorUp = true;
    gClock += 2000;
    restartPlugin(&bench);
    bench.tick();
    const std::size_t mark = bench.network.order().size();
    bench.callback("start", 0, 0, "Other Show");
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK(!std::filesystem::exists(bench.statePath()));
    CHECK_EQ(joined(orderSince(&bench, mark)), "PUT " + kStateRoute + "\nOBSERVE\n");
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
}

TEST(TheSettleWindowIsANamedHypothesis) { CHECK_EQ(kHypothesisRestartSettleMillis, 30000); }

TEST(ARestartAfterTheCutoffHandsBackAtOnceWhateverFppIsDoing) {
    for (bool restingWhenItStopped : {true, false}) {
        Bench bench;
        enterFallback(&bench);
        const std::string held = readFile(bench.installPath());
        if (restingWhenItStopped) {
            gClock = kCompiledAtMillis + 15 * 60 * 1000;
            bench.tick();
            CHECK(bench.executor->status().state.mode == FallbackMode::kResting);
        }
        gClock = kCompiledAtMillis + 16 * 60 * 1000;
        restartPlugin(&bench);
        CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
        CHECK(!std::filesystem::exists(bench.statePath()));
        // Nothing is posted before the report that must come first.
        CHECK(bench.executor->coordinatorLost());
        (void)held;
    }
    // One second before the cutoff the same restart is undecided, in the saved state.
    Bench early;
    enterFallback(&early);
    gClock = kCompiledAtMillis + 15 * 60 * 1000 - 1000;
    restartPlugin(&early);
    CHECK(early.executor->status().state.mode == FallbackMode::kFallback);
}

TEST(AStateFileThatCannotBeReadMeansNormal) {
    for (const char* contents : {"", "not json", "{\"version\":1,\"state\":\"fallback\"}",
                                 "{\"version\":1,\"state\":\"dancing\",\"playlistName\":\"Main Show\","
                                 "\"sinceMillis\":1,\"packageId\":\"p\",\"packageRevision\":\"r\",\"cutoffAt\":\"c\"}"}) {
        Bench bench;
        bench.writeFile(bench.statePath(), contents);
        restartPlugin(&bench);
        CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
        bench.tick();
        CHECK_EQ(member(lastReport(&bench), "state"), std::string("normal"));
    }
}

TEST(AStateFileFromAnotherVersionIsSetAsideNeverResumedAndAHandBackFetchIsOwed) {
    Bench bench;
    enterFallback(&bench);
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(readFile(bench.installPath()));
    std::string saved = readFile(bench.statePath());
    CHECK(saved.find("\"version\":1") != std::string::npos);
    const std::string later = "\"version\":2";
    saved.replace(saved.find("\"version\":1"), later.size(), later);
    bench.writeFile(bench.statePath(), saved);

    gClock += 2000;
    restartPlugin(&bench);
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK(!std::filesystem::exists(bench.statePath()));
    CHECK(std::filesystem::exists(bench.statePath() + ".unknown-version"));
    bench.tick();
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));

    // A file with no version member is treated the same way.
    FallbackExecutionState ignored;
    CHECK(ParseFallbackState("{\"state\":\"fallback\",\"playlistName\":\"Main Show\",\"sinceMillis\":1,"
                             "\"packageId\":\"p\",\"packageRevision\":\"r\",\"cutoffAt\":\"2026-10-05T12:15:00Z\"}",
                             &ignored) == SavedStateRead::kUnknownVersion);
}

TEST(APluginRestartInNormalStartsInNormalWithNothingSaved) {
    Bench bench;
    bench.network.programEnvelope = getEnvelope(fixture("program.json"));
    bench.tick();
    restartPlugin(&bench);
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    CHECK(!std::filesystem::exists(bench.statePath()));
    CHECK(!bench.executor->coordinatorLost());
    bench.tick();
    bench.callback("playing", 0);
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
}

TEST(ProbingIsBoundedToTenSecondsWheneverThePluginIsNotInNormalEvenWithAFreshDetector) {
    Bench bench;
    bench.detectorConfig.probeIntervalMillis = 60000;
    bench.detectorConfig.minimumLossMillis = 60000;
    bench.makeExecutor();
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.network.coordinatorUp = false;
    for (int i = 0; i < 4; ++i) bench.advanceAndTick(60000);
    CHECK(bench.executor->coordinatorLost());
    gClock = kCompiledAtMillis + kFiveMinutesMillis;
    bench.callback("playing", 0);
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);

    // A restart: this run's detector has confirmed nothing, and the coordinator answers.
    bench.network.coordinatorUp = true;
    restartPlugin(&bench);
    bench.tick();
    const std::size_t before = bench.network.count("GET", "/healthz");
    for (int second = 0; second < 25; ++second) bench.advanceAndTick(1000);
    // Undecided, twenty-five seconds: probes at 10 s and 20 s although the setting says 60 s.
    CHECK_EQ(bench.network.count("GET", "/healthz") - before, static_cast<std::size_t>(2));
    CHECK(!bench.executor->status().coordinatorLost);

    bench.callback("playing", 0);
    const std::size_t resumed = bench.network.count("GET", "/healthz");
    for (int second = 0; second < 60; ++second) bench.advanceAndTick(1000);
    CHECK_EQ(bench.network.count("GET", "/healthz") - resumed, static_cast<std::size_t>(6));
}

// --- what is owed to the coordinator after a hand-back ------------------------

TEST(AHandBackAcknowledgementThatFailsStaysOwedAndIsRetriedAtEveryProbeUntilItSucceeds) {
    Bench bench;
    enterFallback(&bench);
    ProgramSpec newer = twoEntryProgram(bench);
    newer.revision = "rev-b";
    newer.packageId = "pkg-b";
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(signedProgram(newer));
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);

    // The coordinator has just come back: its first answers are a timeout and a 5xx.
    bench.network.scriptAcknowledge(noResponse());
    bench.network.scriptAcknowledge(answer(503, "{\"title\":\"Unavailable\"}"));
    stopPlaylist(&bench);
    bench.tick();
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
    CHECK_EQ(bench.executor->status().programRevision, std::string("rev-b"));
    // The status says it is owed and waiting.
    CHECK(bench.executor->status().acknowledgementOwed);
    CHECK_EQ(bench.executor->status().acknowledgementProblem, std::string(kAcknowledgementWaitingMessage));
    const showmesh::json::Value waitingFile = parseJson(readFile(bench.stateDir() + "/" + kFallbackStatusFilename));
    CHECK_EQ(member(waitingFile, "acknowledgementProblem"), std::string(kAcknowledgementWaitingMessage));

    // Not on every 250 ms tick, on the probe cadence.
    for (int i = 0; i < 19; ++i) bench.advanceAndTick(250);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
    bench.advanceAndTick(250);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(2));
    // The fetch between says unchanged; the acknowledgement is still owed.
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    const std::vector<Sent> acks = bench.network.sent("POST", "/acknowledge");
    CHECK_EQ(acks.size(), static_cast<std::size_t>(3));
    if (acks.size() < 3) return;
    for (const Sent& ack : acks) {
        CHECK_EQ(member(parseJson(ack.body), "revision"), std::string("rev-b"));
        CHECK_EQ(member(parseJson(ack.body), "verificationResult"), std::string("verified"));
    }
    // It succeeded: nothing more is sent, through a full refetch interval of unchanged fetches.
    for (int i = 0; i < 14; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(3));
    CHECK(bench.network.count("GET", kProgramRoute) >= 2);
    CHECK(!bench.executor->status().acknowledgementOwed);
    CHECK(bench.executor->status().acknowledgementProblem.empty());
}

namespace {

// A bench handed back with a newer copy published and the next acknowledgements scripted.
void handBackWithAcknowledgementAnswers(Bench* bench, const std::vector<HttpResponse>& answers) {
    enterFallback(bench);
    ProgramSpec newer = twoEntryProgram(*bench);
    newer.revision = "rev-b";
    newer.packageId = "pkg-b";
    newer.expiresAt = "2026-10-06T12:00:00Z";
    bench->network.coordinatorUp = true;
    bench->network.programEnvelope = getEnvelope(signedProgram(newer));
    bench->advanceAndTick(kHypothesisProbeIntervalMillis);
    for (const HttpResponse& response : answers) bench->network.scriptAcknowledge(response);
    stopPlaylist(bench);
    bench->tick();
}

std::size_t acknowledgements(Bench* bench) { return bench->network.count("POST", "/acknowledge"); }

}  // namespace

TEST(AnAcknowledgementTheCoordinatorRefusesIsNotAskedAgainUntilANewPairingOrANewCopy) {
    Bench bench;
    const HttpResponse forbidden = answer(403, "{\"title\":\"Forbidden\"}");
    const HttpResponse badRequest = answer(400, "{}");
    handBackWithAcknowledgementAnswers(&bench, {forbidden, forbidden, badRequest, badRequest, badRequest});
    CHECK_EQ(acknowledgements(&bench), static_cast<std::size_t>(1));

    // An hour of probes and refetches: it is not asked again, and it is recorded once.
    for (int i = 0; i < 720; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(acknowledgements(&bench), static_cast<std::size_t>(1));
    std::size_t refusedLines = 0;
    for (const std::string& line : bench.logs) refusedLines += line.find("acknowledge refused") == 0 ? 1 : 0;
    CHECK_EQ(refusedLines, static_cast<std::size_t>(1));
    const std::string pairAgain = "The coordinator refused to record which plan this player holds and answered 403. "
                                  "Pair this player with the coordinator again.";
    CHECK(bench.executor->status().acknowledgementOwed);
    CHECK_EQ(bench.executor->status().acknowledgementProblem, pairAgain);
    const showmesh::json::Value file = parseJson(readFile(bench.stateDir() + "/" + kFallbackStatusFilename));
    CHECK_EQ(member(file, "acknowledgementProblem"), pairAgain);
    CHECK(isOperatorCopy(pairAgain));

    // A new pairing: one more try, refused again, and it stops again.
    bench.pair("token-two");
    for (int i = 0; i < 30; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(acknowledgements(&bench), static_cast<std::size_t>(2));
    // A second hand-back while the same copy is refused does not ask either.
    bench.loseCoordinator();
    bench.callback("playing", 0);
    bench.network.coordinatorUp = true;
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    stopPlaylist(&bench);
    for (int i = 0; i < 30; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(acknowledgements(&bench), static_cast<std::size_t>(2));

    // A newly installed copy is asked for, even the same package and revision with a later expiry.
    ProgramSpec refreshed = twoEntryProgram(bench);
    refreshed.revision = "rev-b";
    refreshed.packageId = "pkg-b";
    refreshed.expiresAt = "2026-10-07T12:00:00Z";
    bench.network.programEnvelope = getEnvelope(signedProgram(refreshed));
    for (int i = 0; i < 30; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(acknowledgements(&bench), static_cast<std::size_t>(3));
    CHECK_EQ(member(parseJson(lastSent(&bench, "POST", "/acknowledge").body), "revision"), std::string("rev-b"));
    const std::string versions = "The coordinator refused to record which plan this player holds and answered 400. "
                                 "Check that the coordinator and this plugin are versions that work together.";
    CHECK_EQ(bench.executor->status().acknowledgementProblem, versions);
    CHECK(isOperatorCopy(versions));
    CHECK(isOperatorCopy(kAcknowledgementWaitingMessage));
    // A refusal is remembered for that copy only: another published copy this player refuses is reported.
    ProgramSpec expired = twoEntryProgram(bench);
    expired.revision = "rev-x";
    expired.packageId = "pkg-x";
    expired.expiresAt = "2026-10-05T12:01:00Z";
    bench.network.programEnvelope = getEnvelope(signedProgram(expired));
    for (int i = 0; i < 30; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(acknowledgements(&bench), static_cast<std::size_t>(4));
    CHECK_EQ(member(parseJson(lastSent(&bench, "POST", "/acknowledge").body), "revision"), std::string("rev-x"));

    // So is a different copy, whose refusal is its own.
    ProgramSpec newest = twoEntryProgram(bench);
    newest.revision = "rev-c";
    newest.packageId = "pkg-c";
    newest.expiresAt = "2026-10-06T12:00:00Z";
    bench.network.programEnvelope = getEnvelope(signedProgram(newest));
    for (int i = 0; i < 30; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(acknowledgements(&bench), static_cast<std::size_t>(5));
    CHECK_EQ(member(parseJson(lastSent(&bench, "POST", "/acknowledge").body), "revision"), std::string("rev-c"));

    // The next pairing asks once more, and a success clears what the status said.
    bench.pair("token-three");
    for (int i = 0; i < 3; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(acknowledgements(&bench), static_cast<std::size_t>(6));
    CHECK(!bench.executor->status().acknowledgementOwed);
    CHECK(bench.executor->status().acknowledgementProblem.empty());
}

TEST(ATimeoutA408A429AndA5xxKeepTheAcknowledgementRetriedOnTheProbeCadence) {
    Bench bench;
    handBackWithAcknowledgementAnswers(
        &bench, {noResponse(), answer(408, "{}"), answer(429, "{}"), answer(500, "{}"), answer(503, "{}")});
    for (std::size_t expected = 1; expected <= 5; ++expected) {
        CHECK_EQ(acknowledgements(&bench), expected);
        CHECK(bench.executor->status().acknowledgementOwed);
        CHECK_EQ(bench.executor->status().acknowledgementProblem, std::string(kAcknowledgementWaitingMessage));
        bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    }
    CHECK_EQ(acknowledgements(&bench), static_cast<std::size_t>(6));
    CHECK(!bench.executor->status().acknowledgementOwed);
}

TEST(AStateReportProblemGivesTheActionThatFitsTheAnswer) {
    auto problem = [](StateReportAnswerKind kind, int status) {
        StateReportAnswer answer;
        answer.kind = kind;
        answer.statusCode = status;
        return StateReportProblem(answer);
    };
    const std::string pairing = "Check this player's pairing on the coordinator.";
    CHECK(problem(StateReportAnswerKind::kNotAllowed, 401).find(pairing) != std::string::npos);
    CHECK(problem(StateReportAnswerKind::kNotAllowed, 403).find(pairing) != std::string::npos);
    CHECK_EQ(problem(StateReportAnswerKind::kInvalid, 400),
             std::string("The coordinator answered 400 to this player's state. Check that the coordinator and this "
                         "plugin are versions that work together."));
    CHECK_EQ(problem(StateReportAnswerKind::kNotYet, 409),
             std::string("The coordinator answered 409 to this player's state. Check that the coordinator can reach "
                         "this player."));
    CHECK_EQ(problem(StateReportAnswerKind::kOtherStatus, 503),
             std::string("The coordinator answered 503 to this player's state. Check the coordinator; the plugin "
                         "keeps sending this player's state."));
    CHECK_EQ(problem(StateReportAnswerKind::kOtherStatus, 418),
             std::string("The coordinator answered 418 to this player's state. Check the coordinator's log."));
    for (int status : {400, 409, 418, 500, 503}) {
        const StateReportAnswerKind kind = status == 400   ? StateReportAnswerKind::kInvalid
                                           : status == 409 ? StateReportAnswerKind::kNotYet
                                                           : StateReportAnswerKind::kOtherStatus;
        CHECK(problem(kind, status).find("pairing") == std::string::npos);
        CHECK(isOperatorCopy(problem(kind, status)));
    }
}

TEST(AWallClockStepAfterAStartNeitherEndsTheSettleWindowEarlyNorHandsBackAtOnce) {
    for (TimeMillis step : {static_cast<TimeMillis>(-3600000), static_cast<TimeMillis>(120000)}) {
        Bench bench;
        ProgramSpec longLived = twoEntryProgram(bench);
        longLived.expiresAt = "2026-10-06T12:00:00Z";
        bench.writeFile(bench.installPath(), signedProgram(longLived));
        bench.loseCoordinator();
        bench.callback("playing", 0);
        bench.network.coordinatorUp = true;
        restartPlugin(&bench);
        bench.tick();

        // The player's clock is set, back an hour or forward two minutes, one second after the start.
        gClock += 1000;
        gWallClockStep = step;
        bench.tick();
        CHECK(bench.executor->status().waitingForFpp);
        CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
        gClock += kHypothesisRestartSettleMillis - 1001;
        bench.tick();
        CHECK(bench.executor->status().waitingForFpp);
        // Thirty seconds really passed.
        gClock += 1;
        bench.tick();
        CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
        CHECK(!std::filesystem::exists(bench.statePath()));
    }
}

TEST(AnExpiredCopyIsNeverAcknowledgedAsVerified) {
    Bench bench;
    enterFallback(&bench);
    const std::string held = readFile(bench.installPath());
    // The outage outlasts the plan; the coordinator comes back still serving the same expired copy.
    gClock = kCompiledAtMillis + 15 * 60 * 1000;
    bench.tick();
    CHECK(bench.executor->status().state.mode == FallbackMode::kResting);
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(held);
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    stopPlaylist(&bench);
    for (int i = 0; i < 3; ++i) bench.advanceAndTick(kHypothesisProbeIntervalMillis);

    const std::vector<Sent> acks = bench.network.sent("POST", "/acknowledge");
    CHECK(!acks.empty());
    for (const Sent& ack : acks) {
        CHECK_EQ(member(parseJson(ack.body), "verificationResult"), std::string("mismatched-program"));
    }
}

TEST(AHandBackFetchThatGetsNoAnswerIsRetriedOnTheProbeCadenceNotOnEveryTick) {
    Bench bench;
    enterFallback(&bench);
    bench.network.coordinatorUp = true;
    bench.network.programEnvelope = getEnvelope(readFile(bench.installPath()));
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    bench.network.programFetchUnreachable = true;
    stopPlaylist(&bench);
    bench.tick();
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(1));
    for (int i = 0; i < 19; ++i) bench.advanceAndTick(250);
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(1));
    bench.advanceAndTick(250);
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(2));

    bench.network.programFetchUnreachable = false;
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    CHECK_EQ(bench.network.count("GET", kProgramRoute), static_cast<std::size_t>(3));
    CHECK_EQ(bench.network.count("POST", "/acknowledge"), static_cast<std::size_t>(1));
}

// --- nothing leaves after the cutoff ------------------------------------------

TEST(ARetryAlreadyWaitingIsNotSentAfterTheCutoffAndNeitherIsAProgram) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.loseCoordinator();
    bench.network.nodesDown.insert(kNodeA);
    // The entry starts 100 ms before the cutoff. The node does not answer, and the wait before the retry crosses it.
    gClock = kCompiledAtMillis + 15 * 60 * 1000 - 100;
    bench.onPause = [] { gClock += 250; };
    bench.callback("playing", 0);

    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(1));
    CHECK_EQ(bench.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(0));
    const FallbackRecord record = bench.lastActivationRecord();
    CHECK_EQ(record.outcome, std::string(kOutcomeCutoffPassed));
    CHECK_EQ(record.attempts, 1);
    CHECK(!record.nodeAnswered);
    CHECK(record.reason.find("ran out before the node answered") != std::string::npos);

    Bench programAsked;
    programAsked.writeFile(programAsked.installPath(), signedProgram(twoEntryProgram(programAsked)));
    programAsked.loseCoordinator();
    programAsked.network.scriptActivation(kNodeA, nodeAnswer(409, "program-not-installed"));
    gClock = kCompiledAtMillis + 15 * 60 * 1000 - 100;
    programAsked.network.beforeAnswer = [](const std::string& method, const std::string&) {
        if (method == "POST") gClock += 200;
    };
    programAsked.callback("playing", 0);
    // The node asked for the program, but by then the cutoff had passed: no program and no retry.
    CHECK_EQ(programAsked.activations().size(), static_cast<std::size_t>(1));
    CHECK_EQ(programAsked.network.count("PUT", kNodeProgramPathPrefix), static_cast<std::size_t>(0));
    CHECK_EQ(programAsked.lastActivationRecord().outcome, std::string(kOutcomeCutoffPassed));
}

TEST(TheNoticesAreTrueWhenTheCoordinatorIsAnsweringAgain) {
    Bench bench;
    enterFallback(&bench);
    gClock = kCompiledAtMillis + 15 * 60 * 1000;
    bench.tick();
    const std::string restingWhileLost = bench.executor->notice();
    CHECK(restingWhileLost.find("Restore the coordinator") != std::string::npos);

    bench.network.coordinatorUp = true;
    bench.advanceAndTick(kHypothesisProbeIntervalMillis);
    const std::string restingWhileBack = bench.executor->notice();
    CHECK(restingWhileBack.find("This player stopped starting cues for playlist Main Show at 12:15 UTC on "
                                "2026-10-05") == 0);
    CHECK(restingWhileBack.find("Restore the coordinator") == std::string::npos);
    CHECK(restingWhileBack.find(kCoordinatorTakesOverAction) != std::string::npos);
    CHECK_EQ(bench.statusMessage(), restingWhileBack);

    for (BoundaryResult result : {BoundaryResult::kNotStarted, BoundaryResult::kStartedOnSomeNodes}) {
        NoticeFacts facts;
        facts.state.mode = FallbackMode::kFallback;
        facts.state.playlistName = "Main Show";
        facts.state.cutoffAt = "2026-10-05T12:15:00Z";
        facts.state.lastBoundary = result;
        facts.coordinatorReachable = true;
        const std::string notice = FallbackNotice(facts);
        CHECK(notice.rfind("The coordinator is answering again", 0) == 0);
        CHECK(notice.find("stopped answering") == std::string::npos);
        CHECK(notice.find("estore the coordinator") == std::string::npos);
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
    CHECK(ParseFallbackState(RenderFallbackState(state), &read) == SavedStateRead::kLoaded);
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
    if (posts.size() < 3) return;
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

// --- the worker and the executor's own thread together ------------------------
//
// These cases start the runtime, so the runtime worker and the executor's
// thread run at once. They are what the thread sanitizer run is for.

namespace {

bool waitUntil(const std::function<bool()>& done, int millis = 8000) {
    for (int waited = 0; waited < millis; waited += 5) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

// Moves the fixture clock forward one probe interval at a time until done.
bool advanceUntil(const std::function<bool()>& done) {
    for (int step = 0; step < 200; ++step) {
        if (done()) return true;
        gClock += kHypothesisProbeIntervalMillis;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    return done();
}

void fppCallback(Bench* bench, const char* action, int position, int loop, const char* playlist = kPlaylistName) {
    bench->runtime->observeCallback(playlist, action, "mainPlaylist", position, "a.fseq", "", loop);
}

FallbackMode modeOnDisk(Bench* bench) {
    FallbackExecutionState onDisk;
    return LoadFallbackState(bench->credentialDir(), &onDisk) == SavedStateRead::kLoaded ? onDisk.mode
                                                                                         : FallbackMode::kNormal;
}

}  // namespace

TEST(TwoThreads_OutageDeliveryRecoveryAndHandBack) {
    Bench bench;
    ProgramSpec longLived = twoEntryProgram(bench);
    longLived.expiresAt = "2026-10-06T12:00:00Z";
    const std::string program = signedProgram(longLived);
    bench.writeFile(bench.installPath(), program);
    bench.network.programEnvelope = getEnvelope(program);
    bench.network.coordinatorUp = false;
    bench.runtime->start();

    CHECK(advanceUntil([&] { return bench.executor->coordinatorLost(); }));
    // The worker delivers and saves the state while the executor's thread keeps probing.
    fppCallback(&bench, "playing", 0, 0);
    CHECK(waitUntil([&] { return bench.activations().size() == 1; }));
    fppCallback(&bench, "query_next", 0, 0);
    fppCallback(&bench, "playing", 1, 0);
    CHECK(waitUntil([&] { return bench.activations().size() == 2; }));

    bench.network.coordinatorUp = true;
    CHECK(advanceUntil([&] { return bench.executor->status().coordinatorReachable; }));
    CHECK(waitUntil([&] { return bench.network.count("PUT", "/fallback-state") >= 1; }));

    // The worker hands back while the executor's thread reports and then fetches.
    bench.runtime->observeCallback("", "stop", "", 0, "", "");
    CHECK(waitUntil([&] { return bench.executor->status().state.mode == FallbackMode::kNormal; }));
    CHECK(advanceUntil([&] { return bench.network.count("POST", "/acknowledge") >= 1; }));
    bench.runtime->stop();

    CHECK(!std::filesystem::exists(bench.statePath()));
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(2));
    CHECK_EQ(bench.sink.published.size(), static_cast<std::size_t>(0));
    CHECK_EQ(member(lastReport(&bench), "state"), std::string("normal"));
}

TEST(TwoThreads_TheCutoffArrivesWhileEntriesKeepStarting) {
    Bench bench;
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.network.coordinatorUp = false;
    bench.runtime->start();
    CHECK(advanceUntil([&] { return bench.executor->coordinatorLost(); }));
    fppCallback(&bench, "playing", 0, 0);
    CHECK(waitUntil([&] { return bench.executor->status().state.mode == FallbackMode::kFallback; }));

    // Entries keep starting on the worker while the clock crosses the cutoff under the executor's thread.
    gClock = kCompiledAtMillis + 15 * 60 * 1000 - 2000;
    for (int pass = 1; pass <= 40; ++pass) {
        fppCallback(&bench, "query_next", pass % 2, pass);
        fppCallback(&bench, "playing", (pass + 1) % 2, pass);
        gClock += 250;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(waitUntil([&] { return bench.executor->status().state.mode == FallbackMode::kResting; }));
    CHECK(waitUntil([&] { return bench.runtime->handoff().pending() == 0; }));
    bench.runtime->stop();

    // Whichever thread saw the cutoff first, the file agrees with the state and nothing was sent after it.
    CHECK(modeOnDisk(&bench) == FallbackMode::kResting);
    const std::size_t sent = bench.activations().size();
    for (const FallbackRecord& record : bench.executor->status().recentActivations) {
        if (record.outcome == "authorized") CHECK(record.atMillis < kCompiledAtMillis + 15 * 60 * 1000);
    }
    bench.runtime->start();
    fppCallback(&bench, "query_next", 0, 99);
    fppCallback(&bench, "playing", 1, 99);
    CHECK(waitUntil([&] { return bench.runtime->handoff().pending() == 0; }));
    bench.runtime->stop();
    CHECK_EQ(bench.activations().size(), sent);
}

namespace {

// Stands in for the write of the state file. The save that matches `gated`
// is held, with the file lock, until `proceed` says the other thread has
// changed the state, and then a little longer so a second writer that is not
// kept out has time to get in. Every write is counted and remembered.
struct GatedStateWriter {
    std::function<bool(const FallbackExecutionState&)> gated;
    std::function<bool()> proceed;
    std::atomic<bool> gateReached{false};
    std::atomic<bool> gateUsed{false};
    std::atomic<int> writing{0};
    std::atomic<int> mostWritingAtOnce{0};
    std::mutex mutex;
    std::vector<std::pair<std::thread::id, FallbackMode>> writes;

    bool write(const std::string& credentialDir, const FallbackExecutionState& state) {
        const int now = ++writing;
        int most = mostWritingAtOnce.load();
        while (now > most && !mostWritingAtOnce.compare_exchange_weak(most, now)) {
        }
        if (gated(state) && !gateUsed.exchange(true)) {
            gateReached = true;
            for (int waited = 0; waited < 8000 && !proceed(); waited += 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            for (int waited = 0; waited < 300 && writing.load() < 2; waited += 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        const bool saved = SaveFallbackState(credentialDir, state);
        {
            std::lock_guard<std::mutex> lock(mutex);
            writes.emplace_back(std::this_thread::get_id(), state.mode);
        }
        --writing;
        return saved;
    }

    // How many distinct threads wrote the file.
    std::size_t writerThreads() {
        std::lock_guard<std::mutex> lock(mutex);
        std::set<std::thread::id> ids;
        for (const auto& w : writes) ids.insert(w.first);
        return ids.size();
    }
    FallbackMode lastWritten() {
        std::lock_guard<std::mutex> lock(mutex);
        return writes.empty() ? FallbackMode::kNormal : writes.back().second;
    }
};

// One probe a second and loss after two, so a case can walk the executor's own thread by the clock.
void useAFastDetector(Bench* bench) {
    bench->detectorConfig.probeIntervalMillis = 1000;
    bench->detectorConfig.failedProbesToConfirm = 2;
    bench->detectorConfig.minimumLossMillis = 0;
}

// Moves the clock a second at a time, each time waiting for the executor's own thread to probe.
bool probeUntil(Bench* bench, const std::function<bool()>& done) {
    for (int step = 0; step < 40; ++step) {
        if (done()) return true;
        const std::size_t probes = bench->network.count("GET", "/healthz");
        gClock += 1000;
        if (!waitUntil([&] { return bench->network.count("GET", "/healthz") > probes; }, 3000)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
}

}  // namespace

// The cutoff transition and its save run on the executor's own thread while
// the runtime worker is inside a save of the entry it is starting.
TEST(TwoThreads_TheCutoffOnTheExecutorsThreadOverlapsASaveOnTheWorkerAndTheNewerStateIsWhatIsOnDisk) {
    Bench bench;
    GatedStateWriter writer;
    writer.gated = [](const FallbackExecutionState& state) { return state.mode == FallbackMode::kFallback; };
    writer.proceed = [&] { return bench.executor->status().state.mode == FallbackMode::kResting; };
    bench.writeState = [&](const std::string& dir, const FallbackExecutionState& state) {
        return writer.write(dir, state);
    };
    useAFastDetector(&bench);
    bench.makeExecutor();
    bench.writeFile(bench.installPath(), signedProgram(twoEntryProgram(bench)));
    bench.network.coordinatorUp = false;
    bench.runtime->start();
    CHECK(probeUntil(&bench, [&] { return bench.executor->coordinatorLost(); }));

    // The worker enters fallback and is held inside its first save, before the activation leaves.
    fppCallback(&bench, "playing", 0, 0);
    CHECK(waitUntil([&] { return writer.gateReached.load(); }));
    // The clock crosses the cutoff: the executor's own thread makes the transition and goes to save.
    gClock = kCompiledAtMillis + 15 * 60 * 1000;
    CHECK(waitUntil([&] { return bench.executor->status().state.mode == FallbackMode::kResting; }));
    CHECK(waitUntil([&] { return bench.runtime->handoff().pending() == 0 && writer.writing.load() == 0; }));
    CHECK(waitUntil([&] { return writer.lastWritten() == FallbackMode::kResting; }));
    bench.runtime->stop();

    CHECK_EQ(writer.mostWritingAtOnce.load(), 1);
    CHECK_EQ(writer.writerThreads(), static_cast<std::size_t>(2));
    CHECK(modeOnDisk(&bench) == FallbackMode::kResting);
    CHECK(bench.executor->status().state.mode == FallbackMode::kResting);
    // The entry the worker was starting got no activation after the cutoff.
    CHECK_EQ(bench.activations().size(), static_cast<std::size_t>(0));
}

// The settle-window hand-back and its save run on the executor's own thread
// while the runtime worker enters fallback again and saves.
TEST(TwoThreads_AHandBackOnTheExecutorsThreadOverlapsASaveOnTheWorkerAndTheNewerStateIsWhatIsOnDisk) {
    Bench bench;
    ProgramSpec longLived = twoEntryProgram(bench);
    longLived.expiresAt = "2026-10-06T12:00:00Z";
    bench.writeFile(bench.installPath(), signedProgram(longLived));
    bench.loseCoordinator();
    bench.callback("playing", 0);
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);

    GatedStateWriter writer;
    writer.gated = [](const FallbackExecutionState& state) { return state.mode == FallbackMode::kNormal; };
    writer.proceed = [&] { return bench.executor->status().state.mode == FallbackMode::kFallback; };
    bench.writeState = [&](const std::string& dir, const FallbackExecutionState& state) {
        return writer.write(dir, state);
    };
    useAFastDetector(&bench);
    restartPlugin(&bench);
    bench.runtime->start();
    CHECK(probeUntil(&bench, [&] { return bench.executor->status().coordinatorLost; }));
    CHECK(bench.executor->status().waitingForFpp);

    // FPP stays idle: the executor's own thread hands back and is held inside the save of normal.
    gClock += kHypothesisRestartSettleMillis;
    CHECK(waitUntil([&] { return writer.gateReached.load(); }));
    CHECK(bench.executor->status().state.mode == FallbackMode::kNormal);
    // FPP starts the playlist, the coordinator is still lost, and the worker enters fallback and goes to save.
    fppCallback(&bench, "playing", 1, 0);
    CHECK(waitUntil([&] { return bench.activations().size() == 2; }));
    CHECK(waitUntil([&] { return bench.runtime->handoff().pending() == 0 && writer.writing.load() == 0; }));
    bench.runtime->stop();

    CHECK_EQ(writer.mostWritingAtOnce.load(), 1);
    CHECK_EQ(writer.writerThreads(), static_cast<std::size_t>(2));
    CHECK(bench.executor->status().state.mode == FallbackMode::kFallback);
    CHECK(modeOnDisk(&bench) == FallbackMode::kFallback);
    std::size_t handBacks = 0;
    for (const std::string& line : bench.logs) handBacks += line.find("handed back") == 0 ? 1 : 0;
    CHECK_EQ(handBacks, static_cast<std::size_t>(1));
}

namespace {

void spinFor(std::int64_t nanos) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(nanos);
    while (std::chrono::steady_clock::now() < until) {
    }
}

}  // namespace

// The settle tick on one thread and FPP's first callback on another, aimed
// at the same instant: whichever takes the lock first decides, and the other
// does nothing. No round may both resume and hand back. Each round moves the
// aim toward the edge between the two outcomes, so the rounds stay close to it.
TEST(TwoThreads_TheSettleTickAndFppsFirstCallbackNeverBothDecideInThreeThousandRounds) {
    Bench bench;
    ProgramSpec longLived = twoEntryProgram(bench);
    longLived.expiresAt = "2026-10-06T12:00:00Z";
    bench.writeFile(bench.installPath(), signedProgram(longLived));
    bench.loseCoordinator();
    bench.callback("playing", 0);
    const std::string saved = readFile(bench.statePath());
    bench.network.coordinatorUp = true;
    const TimeMillis startAt = gClock;

    // The probe is the tick's last step before the settle check. The callback
    // is released from inside it, and one side is then held back by `lead`.
    std::atomic<bool> armed{false};
    std::atomic<bool> released{false};
    std::atomic<std::int64_t> leadNanos{0};
    bench.network.beforeAnswer = [&](const std::string&, const std::string& url) {
        if (url.find("/healthz") == std::string::npos || !armed.exchange(false)) return;
        released = true;
        if (leadNanos.load() > 0) spinFor(leadNanos.load());
    };

    int resumedRounds = 0;
    int handedBackRounds = 0;
    int bothRounds = 0;
    int otherwiseWrongRounds = 0;
    for (int round = 0; round < 3000; ++round) {
        gClock = startAt;
        bench.writeFile(bench.statePath(), saved);
        ::chmod(bench.statePath().c_str(), 0600);
        restartPlugin(&bench);
        bench.tick();
        {
            std::lock_guard<std::mutex> lock(bench.recordMutex);
            bench.logs.clear();
        }
        const std::size_t activationsBefore = bench.activations().size();
        const std::size_t observationsBefore = bench.sink.published.size();
        gClock += kHypothesisRestartSettleMillis;

        released = false;
        armed = true;
        std::thread calling([&] {
            while (!released.load()) {
            }
            if (leadNanos.load() < 0) spinFor(-leadNanos.load());
            bench.runtime->observeCallback(kPlaylistName, "playing", "mainPlaylist", 1, "a.fseq", "", 0);
            bench.runtime->drainOnce();
        });
        bench.executor->tick(fixtureClock());
        calling.join();

        bool resumed = false;
        bool handedBack = false;
        {
            std::lock_guard<std::mutex> lock(bench.recordMutex);
            for (const std::string& line : bench.logs) {
                resumed = resumed || line.find("resumed the saved state") == 0;
                handedBack = handedBack || line.find("handed back") == 0;
            }
        }
        const FallbackMode mode = bench.executor->status().state.mode;
        const bool activated = bench.activations().size() > activationsBefore;
        const bool observed = bench.sink.published.size() > observationsBefore;
        if (resumed && handedBack) ++bothRounds;
        if (resumed && !handedBack) ++resumedRounds;
        if (handedBack && !resumed) ++handedBackRounds;
        // Resumed: still the executor, the cue sent, nothing posted, the file kept.
        // Handed back: normal, nothing sent, the entry posted, the file gone.
        const bool resumedRight = resumed && mode == FallbackMode::kFallback && activated && !observed &&
                                  modeOnDisk(&bench) == FallbackMode::kFallback;
        const bool handedBackRight = handedBack && mode == FallbackMode::kNormal && !activated && observed &&
                                     !std::filesystem::exists(bench.statePath());
        if (resumedRight == handedBackRight) ++otherwiseWrongRounds;
        // The callback won: let the tick go sooner next time. The tick won: hold it back.
        leadNanos += resumed ? -500 : 500;
    }
    bench.network.beforeAnswer = nullptr;
    CHECK_EQ(bothRounds, 0);
    CHECK_EQ(otherwiseWrongRounds, 0);
    CHECK_EQ(resumedRounds + handedBackRounds, 3000);
    // Both orders happened often, so the rounds did race.
    CHECK(resumedRounds > 300);
    CHECK(handedBackRounds > 300);
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
    // Nothing about what FPP is playing is read at construction: FPP loads plugins before any playlist starts.
    CHECK(delivery.find("Player::INSTANCE") == std::string::npos);
    CHECK(delivery.find("showmesh::fallback::FallbackExecutor executor_;") != std::string::npos);
    CHECK(delivery.find("recorder() { return &executor_; }") != std::string::npos);
}
