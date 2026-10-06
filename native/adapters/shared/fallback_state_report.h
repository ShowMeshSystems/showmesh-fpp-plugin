#pragma once

// The plugin's state report, contract section 5.15, and nothing else: this is
// the one place its wire shape lives.
//
//   PUT /api/v1/fallback-programs/{fppInstanceId}/fallback-state
//   Authorization: Bearer <pairing token>
//
//   {"schemaVersion":1,"bootId":"<uuid>","sequence":1,"state":"normal",
//    "since":"<RFC 3339>"}
//   and, in fallback and resting only:
//    "playlistName","packageId","packageRevision","cutoffAt"
//
// No answer, and no missing answer, ever changes the plugin's state.

#include <cstdint>
#include <string>
#include <vector>

#include "fallback_program_fetch.h"
#include "fallback_state_store.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/http_transport.h"
#include "showmesh/json.h"

namespace showmesh {
namespace fallback {

constexpr int kStateReportSchemaVersion = 1;
// Section 5.13: the plugin waits for the hand-back report's answer for at most this long.
constexpr int kStateReportTimeoutMillis = 5000;
// Section 5.15 rule 3, and the bound on the probe while the coordinator is lost.
constexpr TimeMillis kStateReportIntervalMillis = 10000;
constexpr int kLostProbeIntervalCapMillis = 10000;

struct StateReport {
    std::string bootId;
    std::int64_t sequence = 0;
    FallbackExecutionState state;
};

inline std::string StateReportPath(const std::string& fppInstanceUuid) {
    return "/api/v1/fallback-programs/" + fppInstanceUuid + "/fallback-state";
}

// The playlist name as FPP's own status spells it (current_playlist.playlist):
// no directory and no .json suffix. The coordinator compares the two readings.
inline std::string FppStatusPlaylistName(const std::string& name) {
    std::string plain = name.substr(name.find_last_of("\\/") + 1);
    const std::string suffix = ".json";
    if (plain.size() >= suffix.size() && plain.compare(plain.size() - suffix.size(), suffix.size(), suffix) == 0) {
        plain = plain.substr(0, plain.find_last_of('.'));
    }
    return plain;
}

inline std::string StateReportBody(const StateReport& report) {
    using showmesh::json::Value;
    std::vector<Value::Member> members = {
        {"schemaVersion", Value::makeNumber(kStateReportSchemaVersion)},
        {"bootId", Value::makeString(report.bootId)},
        {"sequence", Value::makeNumber(static_cast<double>(report.sequence))},
        {"state", Value::makeString(FallbackModeName(report.state.mode))},
        {"since", Value::makeString(detail::formatEpochMillisAsRfc3339(report.state.sinceMillis))},
    };
    if (report.state.mode != FallbackMode::kNormal) {
        members.emplace_back("playlistName", Value::makeString(FppStatusPlaylistName(report.state.playlistName)));
        members.emplace_back("packageId", Value::makeString(report.state.packageId));
        members.emplace_back("packageRevision", Value::makeString(report.state.packageRevision));
        members.emplace_back("cutoffAt", Value::makeString(report.state.cutoffAt));
    }
    return showmesh::json::canonicalize(Value::makeObject(std::move(members))).text;
}

enum class StateReportAnswerKind {
    // 200 with recorded true.
    kRecorded,
    // 200 with recorded false: the coordinator kept a report it already held.
    kKeptEarlier,
    // 400: the coordinator could not read this report.
    kInvalid,
    // 401 or 403, as section 5.2.
    kNotAllowed,
    // 409: the coordinator has not read this player's identity yet.
    kNotYet,
    // 404: the coordinator is older than this contract.
    kOlderCoordinator,
    kOtherStatus,
    kCredentialUnavailable,
    kUnreachable,
};

struct StateReportAnswer {
    StateReportAnswerKind kind = StateReportAnswerKind::kUnreachable;
    int statusCode = 0;
    // The state the coordinator now holds for this player. Only on a 200.
    std::string coordinatorState;
    // The coordinator's own reason text for a refusal, as given.
    std::string detail;
};

// One attempt. A report that fails is not queued and not replayed.
inline StateReportAnswer SendStateReport(HttpTransport* transport, CredentialSource* credentials,
                                         const std::string& baseUrl, const std::string& fppInstanceUuid,
                                         const StateReport& report) {
    StateReportAnswer answer;
    std::string token;
    std::string credentialError;
    if (credentials == nullptr || !credentials->token(&token, &credentialError)) {
        answer.kind = StateReportAnswerKind::kCredentialUnavailable;
        answer.detail = credentialError;
        return answer;
    }
    HttpRequest request;
    request.url = joinUrlPath(baseUrl, StateReportPath(fppInstanceUuid));
    request.body = StateReportBody(report);
    request.bearerToken = token;
    request.timeoutMillis = kStateReportTimeoutMillis;
    const HttpResponse response = transport->put(request);
    if (!response.transportOk) {
        answer.detail = response.error;
        return answer;
    }
    answer.statusCode = response.statusCode;
    const showmesh::json::ParseResult parsed = showmesh::json::parse(response.body);
    const bool haveObject = parsed.ok && parsed.value.type() == showmesh::json::Type::kObject;
    auto text = [&](const char* name) {
        const showmesh::json::Value* v = haveObject ? detail::findMember(parsed.value, name) : nullptr;
        return v != nullptr && v->type() == showmesh::json::Type::kString ? v->string() : std::string();
    };
    if (response.statusCode == 200) {
        const showmesh::json::Value* recorded = haveObject ? detail::findMember(parsed.value, "recorded") : nullptr;
        const bool kept = recorded != nullptr && recorded->type() == showmesh::json::Type::kBool && !recorded->boolean();
        answer.kind = kept ? StateReportAnswerKind::kKeptEarlier : StateReportAnswerKind::kRecorded;
        answer.coordinatorState = text("state");
        return answer;
    }
    answer.detail = text("detail").empty() ? text("title") : text("detail");
    switch (response.statusCode) {
        case 400:
            answer.kind = StateReportAnswerKind::kInvalid;
            break;
        case 401:
        case 403:
            answer.kind = StateReportAnswerKind::kNotAllowed;
            break;
        case 409:
            answer.kind = StateReportAnswerKind::kNotYet;
            break;
        case 404:
            answer.kind = StateReportAnswerKind::kOlderCoordinator;
            break;
        default:
            answer.kind = StateReportAnswerKind::kOtherStatus;
            break;
    }
    return answer;
}

// What an operator reads when a report did not land. Empty when it did.
inline std::string StateReportProblem(const StateReportAnswer& answer) {
    switch (answer.kind) {
        case StateReportAnswerKind::kRecorded:
        case StateReportAnswerKind::kKeptEarlier:
        case StateReportAnswerKind::kCredentialUnavailable:
            return std::string();
        case StateReportAnswerKind::kOlderCoordinator:
            return "The coordinator is too old to be told this player's state. Update the coordinator.";
        case StateReportAnswerKind::kUnreachable:
            return "The coordinator could not be told this player's state: " + answer.detail +
                   ". Check the network between this player and the coordinator.";
        case StateReportAnswerKind::kInvalid:
        case StateReportAnswerKind::kNotAllowed:
        case StateReportAnswerKind::kNotYet:
        case StateReportAnswerKind::kOtherStatus:
            break;
    }
    // The action fits the answer: pairing advice only when the pairing was refused.
    const char* action = "Check the coordinator's log.";
    if (answer.kind == StateReportAnswerKind::kNotAllowed) {
        action = "Check this player's pairing on the coordinator.";
    } else if (answer.kind == StateReportAnswerKind::kInvalid) {
        action = "Check that the coordinator and this plugin are versions that work together.";
    } else if (answer.kind == StateReportAnswerKind::kNotYet) {
        action = "Check that the coordinator can reach this player.";
    } else if (answer.statusCode >= 500) {
        action = "Check the coordinator; the plugin keeps sending this player's state.";
    }
    return "The coordinator answered " + std::to_string(answer.statusCode) + " to this player's state" +
           (answer.detail.empty() ? std::string("") : ": " + answer.detail) + ". " + action;
}

}  // namespace fallback
}  // namespace showmesh
