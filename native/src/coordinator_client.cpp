#include "showmesh/coordinator_client.h"

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "showmesh/atomic_write.h"
#include "showmesh/json.h"
#include "showmesh/observation_payload.h"

namespace showmesh {

namespace {

constexpr const char* kStatusFilename = "observation-status.json";

bool isSuccess(int statusCode) { return statusCode >= 200 && statusCode < 300; }

// Retryable means "the same bytes could still be accepted later". A 4xx
// other than 429 says the coordinator understood the request and refused
// it, so repeating it only spends the show LAN and the audit log.
bool isRetryable(int statusCode) { return statusCode == 429 || statusCode >= 500; }

// Reads reconciliation (and, if present, operatorInstruction) out of an
// accepted observation receipt. Returns false when the receipt carries no
// reconciliation field at all -- the coordinator's best-effort lookup
// failed -- which the caller must treat as "unknown", never as "resolved".
bool parseReconciliationVerdict(const std::string& body, std::string* reconciliation,
                                std::string* operatorInstruction) {
    json::ParseResult parsed = json::parse(body);
    if (!parsed.ok || parsed.value.type() != json::Type::kObject) return false;
    bool found = false;
    for (const auto& member : parsed.value.members()) {
        if (member.first == "reconciliation" && member.second.type() == json::Type::kString) {
            *reconciliation = member.second.string();
            found = true;
        } else if (member.first == "operatorInstruction" && member.second.type() == json::Type::kString) {
            *operatorInstruction = member.second.string();
        }
    }
    return found;
}

// The four outcomes api/openapi.yaml documents as carrying an
// operatorInstruction. identity-unavailable and unbound are neither a
// mismatch nor a resolution, so a verdict of either leaves whatever
// notice state already stands, exactly like an absent verdict.
bool isMismatchOutcome(const std::string& reconciliation) {
    return reconciliation == "stale-import" || reconciliation == "unknown-entry" ||
           reconciliation == "evidence-mismatch" || reconciliation == "cross-show";
}

void addString(std::vector<json::Value::Member>* members, const char* name, const std::string& value) {
    members->emplace_back(name, json::Value::makeString(value));
}

void addNumber(std::vector<json::Value::Member>* members, const char* name, double value) {
    members->emplace_back(name, json::Value::makeNumber(value));
}

}  // namespace

void sleepMillis(int millis, const std::atomic<bool>* stopRequested) {
    // Slept in small chunks rather than one call so a stop request lands
    // within kPollMillis rather than at the end of the full backoff.
    // FPP 10's shutdown predicate gives up after 60 seconds and a single
    // backoff step can be as large as maxBackoffMillis (30 seconds by
    // default), so a single uninterruptible sleep_for here is exactly
    // wide enough to blow through that deadline.
    constexpr int kPollMillis = 50;
    int remaining = millis;
    while (remaining > 0) {
        if (stopRequested != nullptr && stopRequested->load(std::memory_order_relaxed)) return;
        const int chunk = remaining < kPollMillis ? remaining : kPollMillis;
        std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
        remaining -= chunk;
    }
}

std::string renderCoordinatorStatus(const CoordinatorStatus& status) {
    std::vector<json::Value::Member> members;
    addNumber(&members, "schemaVersion", 1);
    members.emplace_back("configured", json::Value::makeBool(status.configured));
    addString(&members, "configurationError", status.configurationError);
    addNumber(&members, "attempts", static_cast<double>(status.attempts));
    addNumber(&members, "retries", static_cast<double>(status.retries));
    addNumber(&members, "observationsAccepted", static_cast<double>(status.observationsAccepted));
    addNumber(&members, "observationsRefused", static_cast<double>(status.observationsRefused));
    addNumber(&members, "definitionsAccepted", static_cast<double>(status.definitionsAccepted));
    addNumber(&members, "definitionsRefused", static_cast<double>(status.definitionsRefused));
    addNumber(&members, "definitionsAlreadyHeld", static_cast<double>(status.definitionsAlreadyHeld));
    addNumber(&members, "unauthorized", static_cast<double>(status.unauthorized));
    addNumber(&members, "forbidden", static_cast<double>(status.forbidden));
    addNumber(&members, "schemaRefused", static_cast<double>(status.schemaRefused));
    addNumber(&members, "conflicts", static_cast<double>(status.conflicts));
    addNumber(&members, "payloadTooLarge", static_cast<double>(status.payloadTooLarge));
    addNumber(&members, "transportFailures", static_cast<double>(status.transportFailures));
    addNumber(&members, "coalescedAcknowledged", static_cast<double>(status.coalescedAcknowledged));
    addNumber(&members, "lastStatusCode", status.lastStatusCode);
    addString(&members, "lastOutcome", status.lastOutcome);
    addString(&members, "lastError", status.lastError);
    addNumber(&members, "lastSuccessAtMillis", static_cast<double>(status.lastSuccessAtMillis));
    addNumber(&members, "lastFailureAtMillis", static_cast<double>(status.lastFailureAtMillis));

    json::CanonicalResult canonical = json::canonicalize(json::Value::makeObject(std::move(members)));
    return canonical.ok ? canonical.text : std::string();
}

FileStatusSink::FileStatusSink(std::string stateDir) : path_(joinPath(stateDir, kStatusFilename)) {}

void FileStatusSink::writeStatus(const std::string& json) { writeFileAtomically(path_, json); }

CoordinatorClient::CoordinatorClient(HttpTransport* transport, CredentialSource* credentials, std::string baseUrl,
                                     Clock clock, StatusSink* statusSink, Sleeper sleeper, RetryPolicy policy,
                                     PlaylistMismatchNotifier* mismatchNotifier)
    : transport_(transport),
      credentials_(credentials),
      baseUrl_(std::move(baseUrl)),
      clock_(clock),
      statusSink_(statusSink),
      sleeper_(sleeper == nullptr ? sleepMillis : sleeper),
      policy_(policy),
      mismatchNotifier_(mismatchNotifier) {
    status_.configured = transport_ != nullptr && credentials_ != nullptr && !baseUrl_.empty();
    if (!status_.configured && status_.configurationError.empty()) {
        status_.configurationError = "no coordinator base URL or credential source is configured";
    }
}

void CoordinatorClient::setConfigurationError(std::string error) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.configurationError = std::move(error);
        if (!status_.configurationError.empty()) status_.configured = false;
    }
    publishStatus();
}

CoordinatorStatus CoordinatorClient::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_;
}

bool CoordinatorClient::holdsDefinition(const std::string& instanceUuid, const std::string& playlistHash) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return heldDefinitions_.count(std::make_pair(instanceUuid, playlistHash)) != 0;
}

bool CoordinatorClient::definitionIsRefusedTerminally(const std::string& instanceUuid,
                                                       const std::string& playlistHash) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return refusedDefinitions_.count(std::make_pair(instanceUuid, playlistHash)) != 0;
}

void CoordinatorClient::publishStatus() {
    if (statusSink_ == nullptr) return;
    std::string rendered;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        rendered = renderCoordinatorStatus(status_);
        // Written only when it says something new. The counters change on
        // every post, so this is a change of outcome, not of volume.
        if (rendered == lastWrittenStatus_) return;
        lastWrittenStatus_ = rendered;
    }
    statusSink_->writeStatus(rendered);
}

bool CoordinatorClient::publish(const PlaylistEntryObservation& observation) { return sendObservation(observation); }

bool CoordinatorClient::publishUnavailable(const PlaylistEntryObservation& observation) {
    // The same route and the same body shape. An unavailable observation
    // is an observation the coordinator stores and streams, not an error
    // report, so it is delivered exactly like an available one.
    return sendObservation(observation);
}

bool CoordinatorClient::sendObservation(const PlaylistEntryObservation& observation) {
    PayloadResult payload = buildObservationBody(observation);
    if (!payload.ok) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.observationsRefused;
            status_.lastOutcome = "payload-refused-locally";
            status_.lastError = payload.error;
            status_.lastFailureAtMillis = clock_();
        }
        publishStatus();
        return false;
    }
    if (payload.body.size() > kObservationBodyLimitBytes) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.observationsRefused;
            ++status_.payloadTooLarge;
            status_.lastOutcome = "payload-too-large-locally";
            status_.lastError = "the observation body exceeds the contract's 16384 byte bound";
            status_.lastFailureAtMillis = clock_();
        }
        publishStatus();
        return false;
    }

    const Outcome outcome = postWithRetry(kObservationPath, payload.body);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (outcome.accepted) {
            ++status_.observationsAccepted;
            status_.coalescedAcknowledged += observation.coalescedSincePreviousAcknowledged;
        } else {
            ++status_.observationsRefused;
        }
    }
    publishStatus();
    if (mismatchNotifier_ != nullptr) {
        // Every accepted receipt carries this instance's current
        // reconciliation verdict, including on an idempotent replay, so
        // the plugin's polling model (each FPP callback re-posts) is
        // never blind between genuine state changes.
        if (outcome.accepted) {
            applyMismatchVerdict(outcome.body, clock_());
        } else {
            checkMismatchAgeOut(clock_());
        }
    }
    return outcome.accepted;
}

void CoordinatorClient::applyMismatchVerdict(const std::string& body, TimeMillis now) {
    // The coordinator answered, whatever it said: this is the reachability
    // signal the age-out clock runs against, not the verdict itself.
    lastVerdictAtMillis_ = now;

    std::string reconciliation;
    std::string operatorInstruction;
    if (!parseReconciliationVerdict(body, &reconciliation, &operatorInstruction)) return;

    if (isMismatchOutcome(reconciliation)) {
        // The contract pairs a mismatch outcome with a non-empty
        // operatorInstruction; both are omitted together otherwise. A
        // receipt that violates that (a mismatch outcome with no
        // instruction text) is treated the same as an absent verdict
        // rather than raised: FPP's own notification centre would show a
        // blank notice, which is visible, alarming, and tells the
        // operator nothing they can act on.
        if (!operatorInstruction.empty()) raiseMismatchNotice(operatorInstruction);
    } else if (reconciliation == "resolved") {
        clearMismatchNotice();
    }
}

void CoordinatorClient::checkMismatchAgeOut(TimeMillis now) {
    if (!mismatchActive_) return;
    if (now - lastVerdictAtMillis_ < kMismatchVerdictAgeOutMillis) return;
    clearMismatchNotice();
}

void CoordinatorClient::raiseMismatchNotice(const std::string& instruction) {
    if (mismatchActive_ && lastRaisedMessage_ == instruction) return;
    if (mismatchActive_) mismatchNotifier_->clearMismatch(ShowMesh_PlaylistMismatch, lastRaisedMessage_);
    mismatchNotifier_->raiseMismatch(ShowMesh_PlaylistMismatch, instruction);
    lastRaisedMessage_ = instruction;
    mismatchActive_ = true;
}

void CoordinatorClient::clearMismatchNotice() {
    if (!mismatchActive_) return;
    mismatchNotifier_->clearMismatch(ShowMesh_PlaylistMismatch, lastRaisedMessage_);
    mismatchActive_ = false;
    lastRaisedMessage_.clear();
}

bool CoordinatorClient::publishDefinition(const std::string& instanceUuid, const std::string& playlistName,
                                          const std::string& playlistHash, const std::string& canonicalDefinition,
                                          TimeMillis capturedAtMillis) {
    if (holdsDefinition(instanceUuid, playlistHash)) {
        std::lock_guard<std::mutex> guard(mutex_);
        ++status_.definitionsAlreadyHeld;
        return true;
    }
    if (definitionIsRefusedTerminally(instanceUuid, playlistHash)) {
        // Already known to be unacceptable; spending a request on it again
        // only adds latency ahead of the observation citing it, for a
        // result that cannot change until the plugin restarts.
        std::lock_guard<std::mutex> guard(mutex_);
        ++status_.definitionsRefused;
        return false;
    }

    PayloadResult payload =
        buildDefinitionBody(instanceUuid, playlistName, playlistHash, canonicalDefinition, capturedAtMillis);
    if (!payload.ok || payload.body.size() > kDefinitionBodyLimitBytes) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            ++status_.definitionsRefused;
            if (!payload.ok) {
                status_.lastOutcome = "definition-refused-locally";
                status_.lastError = payload.error;
            } else {
                ++status_.payloadTooLarge;
                status_.lastOutcome = "definition-too-large-locally";
                status_.lastError = "the playlist definition body exceeds the contract's 1048576 byte bound";
            }
            status_.lastFailureAtMillis = clock_();
        }
        publishStatus();
        return false;
    }

    const Outcome outcome = postWithRetry(kDefinitionPath, payload.body);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (outcome.accepted) {
            ++status_.definitionsAccepted;
            heldDefinitions_.insert(std::make_pair(instanceUuid, playlistHash));
        } else {
            ++status_.definitionsRefused;
            // "schema-refused" (400, including definition-hash-mismatch)
            // is the coordinator judging this exact content unacceptable;
            // retrying the identical bytes gets the identical answer. A
            // stop, an unreachable coordinator, an auth problem, or a 5xx
            // are all conditions that can differ on the next attempt, so
            // only the content judgment itself is cached.
            if (outcome.label == "schema-refused") {
                refusedDefinitions_.insert(std::make_pair(instanceUuid, playlistHash));
            }
        }
    }
    publishStatus();
    return outcome.accepted;
}

DefinitionHoldings CoordinatorClient::clearHeldDefinitions() {
    std::lock_guard<std::mutex> guard(mutex_);
    DefinitionHoldings holdings;
    holdings.cleared = heldDefinitions_.size();
    heldDefinitions_.clear();
    holdings.held = heldDefinitions_.size();
    // refusedDefinitions_ survives on purpose: a terminal refusal cannot
    // change until the plugin restarts, so re-sending those bytes would
    // spend the retry budget ahead of what would succeed.
    holdings.refusedTerminally = refusedDefinitions_.size();
    return holdings;
}

DefinitionHoldings CoordinatorClient::definitionHoldings() const {
    std::lock_guard<std::mutex> guard(mutex_);
    DefinitionHoldings holdings;
    holdings.held = heldDefinitions_.size();
    holdings.refusedTerminally = refusedDefinitions_.size();
    return holdings;
}

CoordinatorClient::Outcome CoordinatorClient::postWithRetry(const char* path, const std::string& body) {
    Outcome outcome;
    if (transport_ == nullptr || credentials_ == nullptr || baseUrl_.empty()) {
        std::lock_guard<std::mutex> guard(mutex_);
        outcome.label = "not-configured";
        outcome.error = status_.configurationError;
        status_.configured = false;
        status_.lastOutcome = outcome.label;
        status_.lastError = outcome.error;
        status_.lastFailureAtMillis = clock_();
        return outcome;
    }

    const std::string url = joinUrlPath(baseUrl_, path);
    int backoffMillis = policy_.initialBackoffMillis;
    std::uint64_t attempts = 0;
    std::uint64_t retries = 0;
    std::uint64_t unauthorized = 0;
    std::uint64_t forbidden = 0;
    std::uint64_t schemaRefused = 0;
    std::uint64_t conflicts = 0;
    std::uint64_t payloadTooLarge = 0;
    std::uint64_t transportFailures = 0;
    bool configurationFailure = false;
    std::string configurationError;

    for (int attempt = 0; attempt < policy_.maxAttempts; ++attempt) {
        if (stopRequested_.load(std::memory_order_relaxed)) {
            // Give up the remaining retry budget rather than spend it: a
            // caller only asks for this during shutdown, where a prompt
            // return matters more than one more attempt.
            outcome.label = "stopped";
            outcome.error = "stop requested before the retry budget was spent";
            break;
        }

        std::string token;
        std::string credentialError;
        if (!credentials_->token(&token, &credentialError)) {
            // Not retried: a credential file that does not exist, or
            // carries the wrong mode, is fixed by an operator, not by
            // waiting. It stays visible in local status until it is.
            outcome.label = "no-credential";
            outcome.error = credentialError;
            configurationFailure = true;
            configurationError = credentialError;
            break;
        }

        HttpRequest request;
        request.url = url;
        request.body = body;
        request.bearerToken = token;
        // The token is not kept alive in this frame past the call that
        // needs it.
        token.assign(token.size(), '\0');

        ++attempts;
        const HttpResponse response = transport_->post(request);

        if (!response.transportOk) {
            ++transportFailures;
            outcome.statusCode = 0;
            outcome.label = "coordinator-unreachable";
            outcome.error = response.error;
        } else if (isSuccess(response.statusCode)) {
            outcome.accepted = true;
            outcome.statusCode = response.statusCode;
            outcome.label = "accepted";
            outcome.error.clear();
            outcome.body = response.body;
            break;
        } else {
            outcome.statusCode = response.statusCode;
            outcome.error = response.error.empty() ? response.body : response.error;
            switch (response.statusCode) {
                case 401:
                    ++unauthorized;
                    outcome.label = "unauthorized";
                    // The file may have been replaced with a working
                    // credential since it was last read.
                    credentials_->invalidate();
                    break;
                case 403:
                    ++forbidden;
                    outcome.label = "forbidden-missing-fpp-observe-scope";
                    break;
                case 400:
                    ++schemaRefused;
                    outcome.label = "schema-refused";
                    break;
                case 409:
                    ++conflicts;
                    outcome.label = "conflict";
                    break;
                case 413:
                    ++payloadTooLarge;
                    outcome.label = "payload-too-large";
                    break;
                default:
                    outcome.label = isRetryable(response.statusCode) ? "coordinator-error" : "refused";
                    break;
            }
            if (!isRetryable(response.statusCode)) break;
        }

        if (attempt + 1 < policy_.maxAttempts) {
            ++retries;
            sleeper_(backoffMillis, &stopRequested_);
            if (stopRequested_.load(std::memory_order_relaxed)) {
                outcome.label = "stopped";
                outcome.error = "stop requested during backoff";
                break;
            }
            backoffMillis = backoffMillis >= policy_.maxBackoffMillis ? policy_.maxBackoffMillis
                                                                     : backoffMillis * 2;
            if (backoffMillis > policy_.maxBackoffMillis) backoffMillis = policy_.maxBackoffMillis;
        }
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.attempts += attempts;
        status_.retries += retries;
        status_.unauthorized += unauthorized;
        status_.forbidden += forbidden;
        status_.schemaRefused += schemaRefused;
        status_.conflicts += conflicts;
        status_.payloadTooLarge += payloadTooLarge;
        status_.transportFailures += transportFailures;
        status_.lastStatusCode = outcome.statusCode;
        status_.lastOutcome = outcome.label;
        if (outcome.accepted) {
            status_.lastError.clear();
            status_.lastSuccessAtMillis = clock_();
            // A successful post proves the client is fully configured
            // right now. Without this, a plugin that started before the
            // credential file existed kept reporting "configured: false"
            // and the stale configuration error text forever, even once
            // posts were succeeding.
            status_.configured = true;
            status_.configurationError.clear();
        } else {
            status_.lastError = outcome.error;
            status_.lastFailureAtMillis = clock_();
        }
        if (configurationFailure) {
            status_.configured = false;
            status_.configurationError = configurationError;
        }
    }
    return outcome;
}

}  // namespace showmesh
