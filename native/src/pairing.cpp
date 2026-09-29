#include "showmesh/pairing.h"

#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "showmesh/atomic_write.h"
#include "showmesh/json.h"
#include "showmesh/sha256.h"

namespace showmesh {

const char* const kPairingRequestFilename = "pairing-request";
const char* const kPairingCodeFilename = "pairing-code";
const char* const kPairingStatusFilename = "pairing-status.json";
const char* const kPairingClaimPath = "/api/v1/integrations/fpp/pairing/claim";
const char* const kCrockfordAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

namespace {

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string hexEncodeLower(const std::uint8_t* bytes, std::size_t count) {
    static const char* const kHex = "0123456789abcdef";
    std::string out(count * 2, '0');
    for (std::size_t i = 0; i < count; ++i) {
        out[2 * i] = kHex[(bytes[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[bytes[i] & 0xF];
    }
    return out;
}

const json::Value* memberOf(const json::Value& object, const char* name) {
    for (const json::Value::Member& m : object.members()) {
        if (m.first == name) return &m.second;
    }
    return nullptr;
}

bool stringMember(const json::Value& object, const char* name, std::string* out) {
    const json::Value* v = memberOf(object, name);
    if (v == nullptr || v->type() != json::Type::kString) return false;
    *out = v->string();
    return true;
}

bool readWholeFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream contents;
    contents << in.rdbuf();
    *out = contents.str();
    return true;
}

}  // namespace

bool readRandomBytes(std::uint8_t* out, std::size_t count) {
    std::ifstream in("/dev/urandom", std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(in.gcount()) == count;
}

std::string generateSecretHex(RandomBytesFn randomBytes) {
    std::uint8_t raw[32];
    if (randomBytes == nullptr || !randomBytes(raw, sizeof(raw))) return std::string();
    return hexEncodeLower(raw, sizeof(raw));
}

std::string deriveCrockfordPairingCode(const std::string& secretHex) {
    if (secretHex.size() != 64) return std::string();
    std::uint8_t raw[32];
    for (int i = 0; i < 32; ++i) {
        const int hi = hexNibble(secretHex[2 * i]);
        const int lo = hexNibble(secretHex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::string();
        raw[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }

    Sha256 hasher;
    hasher.update(raw, sizeof(raw));
    std::uint8_t digest[32];
    hasher.finish(digest);

    // The first 8 Crockford base32 symbols are fully determined by the
    // digest's first 5 bytes (40 bits, exactly 8 groups of 5 bits), so the
    // remaining 27 bytes are never touched.
    std::uint64_t bits = 0;
    for (int i = 0; i < 5; ++i) bits = (bits << 8) | digest[i];
    std::string symbols(8, '0');
    for (int i = 7; i >= 0; --i) {
        symbols[i] = kCrockfordAlphabet[bits & 0x1F];
        bits >>= 5;
    }
    return symbols.substr(0, 4) + "-" + symbols.substr(4, 4);
}

const char* pairingStateWireValue(PairingState state) {
    switch (state) {
        case PairingState::kIdle:
            return "idle";
        case PairingState::kWaiting:
            return "waiting";
        case PairingState::kPaired:
            return "paired";
        case PairingState::kExpired:
            return "expired";
        case PairingState::kFailed:
            return "failed";
    }
    return "idle";
}

std::string renderPairingStatus(const PairingStatus& status) {
    std::vector<json::Value::Member> members;
    members.emplace_back("state", json::Value::makeString(pairingStateWireValue(status.state)));
    members.emplace_back("code", json::Value::makeString(status.code));
    members.emplace_back("principalId", json::Value::makeString(status.principalId));
    members.emplace_back("pairedAtMillis", json::Value::makeNumber(static_cast<double>(status.pairedAtMillis)));
    members.emplace_back("lastError", json::Value::makeString(status.lastError));
    members.emplace_back("updatedAtMillis", json::Value::makeNumber(static_cast<double>(status.updatedAtMillis)));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    return rendered.ok ? rendered.text : std::string();
}

PairingWorker::PairingWorker(std::string stateDir, std::string credentialDir, HttpTransport* transport,
                             CoordinatorUrlSource* urlSource, Clock clock, RandomBytesFn randomBytes)
    : stateDir_(std::move(stateDir)),
      credentialDir_(std::move(credentialDir)),
      transport_(transport),
      urlSource_(urlSource),
      clock_(clock),
      randomBytes_(randomBytes == nullptr ? readRandomBytes : randomBytes) {
    reconcileStartupState();
}

PairingWorker::~PairingWorker() { stop(); }

PairingStatus PairingWorker::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_;
}

void PairingWorker::setState(PairingState state, TimeMillis now, std::string lastError) {
    std::lock_guard<std::mutex> guard(mutex_);
    status_.state = state;
    status_.lastError = std::move(lastError);
    status_.updatedAtMillis = now;
    // See the header: code is meaningful only while kWaiting.
    if (state != PairingState::kWaiting) status_.code.clear();
}

void PairingWorker::reconcileStartupState() {
    const std::string statusPath = joinPath(stateDir_, kPairingStatusFilename);
    std::string raw;
    if (!readWholeFile(statusPath, &raw)) return;  // no prior status: stays idle

    json::ParseResult parsed = json::parse(raw);
    if (!parsed.ok || parsed.value.type() != json::Type::kObject) return;
    std::string state;
    if (!stringMember(parsed.value, "state", &state)) return;

    // A terminal state from a previous process (paired, expired, failed)
    // needs no reconciliation: the previous process already forgot its
    // secret on every one of those paths, so restoring the status this
    // worker last wrote is enough for it to be reported unchanged. Only a
    // "waiting" status implies a secret the previous process held only in
    // memory, which this restart has already lost.
    if (state != "waiting") {
        std::string principalId;
        std::string lastError;
        double pairedAtMillis = 0;
        double updatedAtMillis = 0;
        stringMember(parsed.value, "principalId", &principalId);
        stringMember(parsed.value, "lastError", &lastError);
        const json::Value* paired = memberOf(parsed.value, "pairedAtMillis");
        if (paired != nullptr && paired->type() == json::Type::kNumber) pairedAtMillis = paired->number();
        const json::Value* updated = memberOf(parsed.value, "updatedAtMillis");
        if (updated != nullptr && updated->type() == json::Type::kNumber) updatedAtMillis = updated->number();

        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = state == "paired"   ? PairingState::kPaired
                       : state == "failed"  ? PairingState::kFailed
                       : state == "expired" ? PairingState::kExpired
                                             : PairingState::kIdle;
        status_.code.clear();
        status_.principalId = principalId;
        status_.lastError = lastError;
        status_.pairedAtMillis = static_cast<TimeMillis>(pairedAtMillis);
        status_.updatedAtMillis = static_cast<TimeMillis>(updatedAtMillis);
        return;
    }

    // A restart during an open wait: the secret that wait needed lived
    // only in the previous process's memory and is gone now, so it can
    // never complete. See the constructor's doc comment.
    std::remove(joinPath(stateDir_, kPairingCodeFilename).c_str());
    const TimeMillis now = clock_();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = PairingState::kExpired;
        status_.code.clear();
        status_.principalId.clear();
        status_.lastError.clear();
        status_.updatedAtMillis = now;
    }
    writeStatusFile();
}

void PairingWorker::run() {
    while (!stopRequested_.load()) {
        tick(clock_());
        if (stopRequested_.load()) break;
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopRequested_.load(); });
    }
}

void PairingWorker::start() {
    if (started_.exchange(true)) return;
    thread_ = std::thread(&PairingWorker::run, this);
}

void PairingWorker::requestStop() {
    stopRequested_.store(true);
    wake_.notify_all();
}

void PairingWorker::stop() {
    requestStop();
    if (thread_.joinable()) thread_.join();
}

void PairingWorker::tick(TimeMillis now) {
    checkPairingRequest(now);

    PairingState current;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        current = status_.state;
    }
    if (current != PairingState::kWaiting) return;

    if (now >= expiresAtMillis_) {
        endPairing(true);
        setState(PairingState::kExpired, now, std::string());
        writeStatusFile();
        return;
    }
    // requestStop() must be able to interrupt a wait about to start a new
    // claim attempt without waiting out the full 3s interval first. Never
    // true unless requestStop() was actually called, so a test that never
    // starts the background thread is unaffected.
    if (stopRequested_.load()) return;
    if (!everAttemptedClaim_ || now - lastClaimAttemptMillis_ >= kPairingClaimIntervalMillis) {
        attemptClaim(now);
    }
}

void PairingWorker::checkPairingRequest(TimeMillis now) {
    const std::string path = joinPath(stateDir_, kPairingRequestFilename);
    struct ::stat info {};
    if (::stat(path.c_str(), &info) != 0) return;
    // Removed on pickup, whether or not starting the pairing below
    // succeeds: a request this worker could not honor must not be retried
    // forever against a secret-generation failure it will keep hitting.
    std::remove(path.c_str());
    startPairing(now);
}

void PairingWorker::startPairing(TimeMillis now) {
    // Ends whatever pairing (if any) was already open, unconditionally,
    // before this one is attempted: a pairing whose own secret or code
    // derivation fails below must not leave the previous pairing's
    // pairing-code file or in-memory secret behind.
    endPairing(true);

    const std::string secret = generateSecretHex(randomBytes_);
    if (secret.empty()) {
        setState(PairingState::kFailed, now, "could not generate a pairing secret");
        writeStatusFile();
        return;
    }
    const std::string code = deriveCrockfordPairingCode(secret);
    if (code.empty()) {
        setState(PairingState::kFailed, now, "could not derive a pairing code from the generated secret");
        writeStatusFile();
        return;
    }

    secretHex_ = secret;
    expiresAtMillis_ = now + kPairingExpiryMillis;
    lastClaimAttemptMillis_ = 0;
    everAttemptedClaim_ = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = PairingState::kWaiting;
        status_.code = code;
        status_.principalId.clear();
        status_.pairedAtMillis = 0;
        status_.lastError.clear();
        status_.updatedAtMillis = now;
    }

    std::vector<json::Value::Member> members;
    members.emplace_back("code", json::Value::makeString(code));
    members.emplace_back("expiresAtMillis", json::Value::makeNumber(static_cast<double>(expiresAtMillis_)));
    json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (rendered.ok) writeFileAtomically(joinPath(stateDir_, kPairingCodeFilename), rendered.text);
    writeStatusFile();
}

void PairingWorker::attemptClaim(TimeMillis now) {
    lastClaimAttemptMillis_ = now;
    everAttemptedClaim_ = true;

    const std::string baseUrl = urlSource_ == nullptr ? std::string() : urlSource_->currentBaseUrl();
    if (baseUrl.empty() || transport_ == nullptr) {
        setState(PairingState::kWaiting, now, "no coordinator URL is configured");
        writeStatusFile();
        return;
    }

    std::vector<json::Value::Member> members;
    members.emplace_back("secret", json::Value::makeString(secretHex_));
    json::CanonicalResult body = json::canonicalize(json::Value::makeObject(std::move(members)));
    if (!body.ok) {
        setState(PairingState::kWaiting, now, "could not render the pairing claim request");
        writeStatusFile();
        return;
    }

    HttpRequest request;
    request.url = joinUrlPath(baseUrl, kPairingClaimPath);
    request.body = body.text;
    // Bounded so a black-holing coordinator URL can hold this one call for
    // at most kPairingClaimTimeoutMillis, never the whole pairing window:
    // see the header and requestStop().
    request.timeoutMillis = kPairingClaimTimeoutMillis;
    const HttpResponse response = transport_->post(request);

    if (!response.transportOk) {
        setState(PairingState::kWaiting, now, "the coordinator could not be reached: " + response.error);
        writeStatusFile();
        return;
    }
    if (response.statusCode == 404) {
        // Expected while nothing has claimed yet; not surfaced as an
        // error, and the file is not rewritten for a routine poll.
        return;
    }
    if (response.statusCode != 200) {
        setState(PairingState::kWaiting,
                 now, "the coordinator refused the pairing claim with status " + std::to_string(response.statusCode));
        writeStatusFile();
        return;
    }

    json::ParseResult parsed = json::parse(response.body);
    std::string token;
    std::string principalId;
    if (!parsed.ok || parsed.value.type() != json::Type::kObject || !stringMember(parsed.value, "token", &token) ||
        token.empty() || !stringMember(parsed.value, "principalId", &principalId)) {
        // The coordinator already deleted its pending entry when it
        // answered 200: the claim is spent whether or not this side could
        // make sense of the body, so the pairing ends here rather than
        // polling forever for a secret the coordinator no longer holds.
        endPairing(true);
        setState(PairingState::kFailed, now, "the coordinator's pairing claim response could not be parsed");
        writeStatusFile();
        return;
    }

    std::string writeError;
    if (!writeCoordinatorCredentialAtomically(credentialDir_, token, &writeError)) {
        endPairing(true);
        setState(PairingState::kFailed, now, writeError);
        writeStatusFile();
        return;
    }

    // The claim is single use: the coordinator has already deleted its
    // pending entry, so nothing about the secret is ever reused again.
    endPairing(true);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = PairingState::kPaired;
        status_.code.clear();
        status_.principalId = principalId;
        status_.pairedAtMillis = now;
        status_.lastError.clear();
        status_.updatedAtMillis = now;
    }
    writeStatusFile();
}

void PairingWorker::endPairing(bool deleteCodeFile) {
    // Overwritten before clearing, not merely cleared, so the secret does
    // not linger in this object's storage after it stops being used.
    secretHex_.assign(secretHex_.size(), '0');
    secretHex_.clear();
    if (deleteCodeFile) std::remove(joinPath(stateDir_, kPairingCodeFilename).c_str());
}

void PairingWorker::writeStatusFile() {
    PairingStatus snapshot;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        snapshot = status_;
    }
    const std::string rendered = renderPairingStatus(snapshot);
    if (rendered.empty() || rendered == lastWrittenStatus_) return;
    if (writeFileAtomically(joinPath(stateDir_, kPairingStatusFilename), rendered)) lastWrittenStatus_ = rendered;
}

}  // namespace showmesh
