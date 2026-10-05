#pragma once

// The plugin's half of the node fallback ingress, contract sections 5.5 to
// 5.8: handing a node its signed program, building and signing one activation
// request, and acting on the node's answer exactly as the 5.8 table says.
//
//   PUT  http://{address}/showmesh/v1/fallback/programs/{fppInstanceUuid}
//   POST http://{address}/showmesh/v1/fallback/activations
//
// The request names a program entry and nothing else. Nothing here ever sends
// a different Cue, entry, program or command in place of a refused one.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "fallback_activation_resolver.h"
#include "fallback_executor_key.h"
#include "showmesh/http_transport.h"
#include "showmesh/json.h"

namespace showmesh {
namespace fallback {

constexpr const char* kNodeProgramPathPrefix = "/showmesh/v1/fallback/programs/";
constexpr const char* kNodeActivationPath = "/showmesh/v1/fallback/activations";

// Section 5.8's bounds.
constexpr int kTransientAttemptLimit = 3;
constexpr int kTransientRetryGapMillis = 250;
constexpr int kRateLimitedRetryGapMillis = 1000;

// Bounds one request to a node, so a node that does not answer holds a
// delivery, and a plugin shutdown, for at most this long per attempt.
constexpr int kNodeRequestTimeoutMillis = 2000;

// Words this plugin records when a node gave no outcome word of its own.
constexpr const char* kOutcomeNoResponse = "no-response";
constexpr const char* kOutcomeUnrecognizedAnswer = "unrecognized-answer";
constexpr const char* kOutcomeStopped = "plugin-stopping";

// A UUID in its 36 character lowercase form from 16 random bytes (version 4).
inline std::string formatExecutionId(const uint8_t randomBytes[16]) {
    uint8_t b[16];
    for (int i = 0; i < 16; ++i) b[i] = randomBytes[i];
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
    char out[37];
    std::snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out);
}

struct ActivationRequestBuild {
    bool ok = false;
    // Why the installed program cannot name this request. Populated when !ok.
    std::string refusal;
    // The RFC 8785 canonical bytes of the request object: the signed bytes.
    std::string canonical;
};

// Builds the section 5.6 request for one target of a match. Every member
// comes from the installed program; a member the program lacks is a refusal.
inline ActivationRequestBuild BuildActivationRequest(const ActivationMatch& match, const ActivationTarget& target,
                                                      const std::string& executionId) {
    ActivationRequestBuild build;
    if (!match.generation().has_value()) {
        build.refusal = "the installed program states no generation";
        return build;
    }
    if (!target.catalogRevision.has_value()) {
        build.refusal = "the installed program states no catalog revision for this node";
        return build;
    }
    using showmesh::json::Value;
    const showmesh::json::CanonicalResult canonical = showmesh::json::canonicalize(Value::makeObject({
        {"schemaVersion", Value::makeNumber(1)},
        {"executionId", Value::makeString(executionId)},
        {"fppInstanceUuid", Value::makeString(match.fppInstanceUuid())},
        {"packageId", Value::makeString(match.packageId())},
        {"packageRevision", Value::makeString(match.revision())},
        {"programExpiresAt", Value::makeString(match.programExpiresAt())},
        {"generation", Value::makeNumber(static_cast<double>(*match.generation()))},
        {"catalogRevision", Value::makeString(*target.catalogRevision)},
        {"entryKey", Value::makeString(match.entryKey())},
        {"cueId", Value::makeString(match.cueId())},
        {"cueRevision", Value::makeNumber(static_cast<double>(match.cueRevision()))},
        {"nodeId", Value::makeString(target.nodeId)},
    }));
    if (!canonical.ok) {
        build.refusal = "the request could not be serialized: " + canonical.error;
        return build;
    }
    build.ok = true;
    build.canonical = canonical.text;
    return build;
}

// The wire body: the exact canonical request bytes that were signed, and the signature.
inline std::string ActivationRequestBody(const std::string& canonicalRequest, const std::string& signatureBase64) {
    return "{\"request\":" + canonicalRequest + ",\"signature\":\"" + signatureBase64 + "\"}";
}

// One node answer on either route. outcome is empty when the node gave none.
struct NodeAnswer {
    bool responded = false;
    int statusCode = 0;
    bool accepted = false;
    std::string outcome;
    std::string reason;
    std::string firstOutcome;
};

inline NodeAnswer ParseNodeAnswer(const HttpResponse& response) {
    NodeAnswer answer;
    if (!response.transportOk) {
        answer.reason = response.error;
        return answer;
    }
    answer.responded = true;
    answer.statusCode = response.statusCode;
    const showmesh::json::ParseResult parsed = showmesh::json::parse(response.body);
    if (!parsed.ok || parsed.value.type() != showmesh::json::Type::kObject) return answer;
    auto text = [&](const char* name) {
        const showmesh::json::Value* v = detail::findMember(parsed.value, name);
        return v != nullptr && v->type() == showmesh::json::Type::kString ? v->string() : std::string();
    };
    const showmesh::json::Value* accepted = detail::findMember(parsed.value, "accepted");
    answer.accepted = accepted != nullptr && accepted->type() == showmesh::json::Type::kBool && accepted->boolean();
    answer.outcome = text("outcome");
    answer.reason = text("reason");
    answer.firstOutcome = text("firstOutcome");
    return answer;
}

// Section 5.5: sends the installed signed program, byte for byte, to one node.
inline NodeAnswer HandProgramToNode(HttpTransport* transport, const std::string& address,
                                     const std::string& fppInstanceUuid, const std::string& signedDocument) {
    HttpRequest request;
    request.url = "http://" + address + kNodeProgramPathPrefix + fppInstanceUuid;
    request.body = signedDocument;
    request.timeoutMillis = kNodeRequestTimeoutMillis;
    return ParseNodeAnswer(transport->put(request));
}

// The outcome word and reason to record for one answer. A node's own word and
// reason are kept as given; where it gave none, the sentence is this player's.
inline void DescribeNodeAnswer(const NodeAnswer& answer, std::string* outcome, std::string* reason) {
    if (!answer.outcome.empty()) {
        *outcome = answer.outcome;
        *reason = answer.reason;
    } else if (answer.responded) {
        *outcome = kOutcomeUnrecognizedAnswer;
        *reason = "The address answered with status " + std::to_string(answer.statusCode) +
                  ", but not as a ShowMesh node. Check the node's address on the coordinator.";
    } else {
        *outcome = kOutcomeNoResponse;
        *reason = "The node did not answer: " + answer.reason +
                  ". Check the node and the network between it and this player.";
    }
}

enum class NodeAnswerClass { kAuthorized, kReplayed, kTransient, kRateLimited, kProgramMissing, kFinal };

// The section 5.8 table's left column. A caller decides on the outcome word;
// the status only matters for a 5xx that carries no word this table names.
inline NodeAnswerClass ClassifyNodeAnswer(const NodeAnswer& answer) {
    if (!answer.responded) return NodeAnswerClass::kTransient;
    if (answer.outcome == "authorized") return NodeAnswerClass::kAuthorized;
    if (answer.outcome == "replayed-execution") return NodeAnswerClass::kReplayed;
    if (answer.outcome == "rate-limited") return NodeAnswerClass::kRateLimited;
    if (answer.outcome == "program-not-installed" || answer.outcome == "program-not-current") {
        return NodeAnswerClass::kProgramMissing;
    }
    if (answer.outcome == "storage-unavailable" || answer.outcome == "not-ready") return NodeAnswerClass::kTransient;
    if (answer.outcome == "no-coordinator-key") return NodeAnswerClass::kFinal;
    if (answer.statusCode >= 500 && answer.statusCode <= 599) return NodeAnswerClass::kTransient;
    return NodeAnswerClass::kFinal;
}

struct NodeDeliveryResult {
    // The node's outcome word for this execution, or one of this file's own
    // words when the node gave none. "authorized" is the only success.
    std::string outcome;
    std::string reason;
    // True when outcome is a word a node sent.
    bool nodeAnswered() const {
        return outcome != kOutcomeNoResponse && outcome != kOutcomeUnrecognizedAnswer && outcome != kOutcomeStopped;
    }
    int attempts = 0;
    bool programResent = false;
    // True when the node said it had already processed this execution id.
    bool replayed = false;
    bool activated() const { return outcome == "authorized"; }
};

// Waits millis, returning false when the plugin is stopping.
using DeliveryPause = std::function<bool(int millis)>;

// Sends one signed activation and follows the section 5.8 table. Every retry
// carries the identical body, and so the identical execution id.
inline NodeDeliveryResult DeliverActivation(HttpTransport* transport, const std::string& address,
                                             const std::string& body, const std::string& fppInstanceUuid,
                                             const std::string& signedDocument, const DeliveryPause& pause) {
    NodeDeliveryResult result;
    HttpRequest request;
    request.url = "http://" + address + kNodeActivationPath;
    request.body = body;
    request.timeoutMillis = kNodeRequestTimeoutMillis;

    bool rateLimitedRetryUsed = false;
    auto finish = [&](const NodeAnswer& answer) {
        DescribeNodeAnswer(answer, &result.outcome, &result.reason);
        return result;
    };
    auto stopped = [&] {
        result.outcome = kOutcomeStopped;
        result.reason = "The plugin stopped before the node answered.";
        return result;
    };

    for (;;) {
        ++result.attempts;
        const NodeAnswer answer = ParseNodeAnswer(transport->post(request));
        switch (ClassifyNodeAnswer(answer)) {
            case NodeAnswerClass::kAuthorized:
                return finish(answer);
            case NodeAnswerClass::kReplayed:
                result.replayed = true;
                result.outcome = answer.firstOutcome.empty() ? "unknown" : answer.firstOutcome;
                result.reason = answer.reason;
                return result;
            case NodeAnswerClass::kTransient:
                if (result.attempts >= kTransientAttemptLimit) return finish(answer);
                if (!pause(kTransientRetryGapMillis)) return stopped();
                continue;
            case NodeAnswerClass::kRateLimited:
                if (rateLimitedRetryUsed) return finish(answer);
                rateLimitedRetryUsed = true;
                if (!pause(kRateLimitedRetryGapMillis)) return stopped();
                continue;
            case NodeAnswerClass::kProgramMissing:
                if (result.programResent) return finish(answer);
                result.programResent = true;
                HandProgramToNode(transport, address, fppInstanceUuid, signedDocument);
                continue;
            case NodeAnswerClass::kFinal:
                return finish(answer);
        }
    }
}

}  // namespace fallback
}  // namespace showmesh
