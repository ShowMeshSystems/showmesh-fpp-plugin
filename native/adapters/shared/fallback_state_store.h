#pragma once

// The plugin's one record of where it stands in ADR-048's three states, and
// its copy on disk (contract section 5.13). The file sits beside the pairing
// token at mode 0600 and is what lets a restarted plugin resume, not repeat.
//
//   {"state":"fallback","playlistName":"...","sinceMillis":0,"packageId":"...",
//    "packageRevision":"...","cutoffAt":"<expiresAt as written>",
//    "occurrence":{"entryKey":"...","identityResolved":true,"playlistLoop":2,
//                  "delivered":false,"executionIds":{"<nodeId>":"<uuid>"}}}

#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fallback_executor_key.h"
#include "showmesh/atomic_write.h"
#include "showmesh/fading_value.h"
#include "showmesh/json.h"

namespace showmesh {
namespace fallback {

constexpr const char* kFallbackStateFilename = "fallback-state.json";

enum class FallbackMode { kNormal, kFallback, kResting };

// The wire words of section 5.15.
inline const char* FallbackModeName(FallbackMode mode) {
    switch (mode) {
        case FallbackMode::kNormal:
            return "normal";
        case FallbackMode::kFallback:
            return "fallback";
        case FallbackMode::kResting:
            return "resting";
    }
    return "normal";
}

// What the last entry boundary in fallback came to.
enum class BoundaryResult {
    kNone,
    kStarted,
    // The plan maps this entry to no cue. Ordinary, and not a failure.
    kNothingToStart,
    kStartedOnSomeNodes,
    kNotStarted,
};

// The entry occurrence this player last acted on, with the execution id it
// used for each node. A retry after a restart reuses these ids.
struct RecordedOccurrence {
    bool present = false;
    bool identityResolved = false;
    std::string entryKey;
    std::optional<int> playlistLoop;
    std::vector<std::pair<std::string, std::string>> executionIds;
    // True once every node's delivery for this occurrence has finished.
    bool delivered = false;
};

// The one record of where this host stands. Everything above the last two
// members is what the state file holds.
struct FallbackExecutionState {
    FallbackMode mode = FallbackMode::kNormal;
    // When this state was entered. For normal from plugin start, the start time.
    TimeMillis sinceMillis = 0;
    // The playlist fallback was entered under, and the program copy it was
    // entered with. Empty in normal. cutoffAt is that copy's expiresAt as written.
    std::string playlistName;
    std::string packageId;
    std::string packageRevision;
    std::string cutoffAt;
    RecordedOccurrence occurrence;

    BoundaryResult lastBoundary = BoundaryResult::kNone;
    // This player's outcome word for a boundary that started nothing. Empty when a node refused.
    std::string lastBoundaryProblem;

    void enter(TimeMillis now, const std::string& playlist, const std::string& package, const std::string& revision,
               const std::string& cutoff) {
        *this = FallbackExecutionState();
        mode = FallbackMode::kFallback;
        sinceMillis = now;
        playlistName = playlist;
        packageId = package;
        packageRevision = revision;
        cutoffAt = cutoff;
    }
    // The cutoff: nothing more is sent until the hand-back.
    void rest(TimeMillis now) {
        mode = FallbackMode::kResting;
        sinceMillis = now;
    }
    // The hand-back: FPP stopped the playlist fallback was entered under.
    void handBack(TimeMillis now) {
        *this = FallbackExecutionState();
        sinceMillis = now;
    }
};

inline std::string RenderFallbackState(const FallbackExecutionState& state) {
    using showmesh::json::Value;
    std::vector<Value::Member> ids;
    for (const auto& id : state.occurrence.executionIds) ids.emplace_back(id.first, Value::makeString(id.second));
    std::vector<Value::Member> occurrence = {
        {"entryKey", Value::makeString(state.occurrence.entryKey)},
        {"identityResolved", Value::makeBool(state.occurrence.identityResolved)},
        {"delivered", Value::makeBool(state.occurrence.delivered)},
        {"executionIds", Value::makeObject(std::move(ids))},
    };
    if (state.occurrence.playlistLoop.has_value()) {
        occurrence.emplace_back("playlistLoop", Value::makeNumber(*state.occurrence.playlistLoop));
    }
    std::vector<Value::Member> members = {
        {"state", Value::makeString(FallbackModeName(state.mode))},
        {"playlistName", Value::makeString(state.playlistName)},
        {"sinceMillis", Value::makeNumber(static_cast<double>(state.sinceMillis))},
        {"packageId", Value::makeString(state.packageId)},
        {"packageRevision", Value::makeString(state.packageRevision)},
        {"cutoffAt", Value::makeString(state.cutoffAt)},
    };
    if (state.occurrence.present) members.emplace_back("occurrence", Value::makeObject(std::move(occurrence)));
    return showmesh::json::canonicalize(Value::makeObject(std::move(members))).text;
}

// False when text is not a fallback or resting record this build can resume.
inline bool ParseFallbackState(const std::string& text, FallbackExecutionState* out) {
    using showmesh::json::Type;
    const showmesh::json::ParseResult parsed = showmesh::json::parse(text);
    if (!parsed.ok || parsed.value.type() != Type::kObject) return false;
    auto find = [](const showmesh::json::Value& object, const char* name) { return detail::findMember(object, name); };
    auto text_ = [&](const showmesh::json::Value& object, const char* name, std::string* value) {
        const showmesh::json::Value* v = find(object, name);
        if (v == nullptr || v->type() != Type::kString) return false;
        *value = v->string();
        return true;
    };
    FallbackExecutionState state;
    std::string mode;
    const showmesh::json::Value* since = find(parsed.value, "sinceMillis");
    if (!text_(parsed.value, "state", &mode) || !text_(parsed.value, "playlistName", &state.playlistName) ||
        !text_(parsed.value, "packageId", &state.packageId) ||
        !text_(parsed.value, "packageRevision", &state.packageRevision) ||
        !text_(parsed.value, "cutoffAt", &state.cutoffAt) || since == nullptr || since->type() != Type::kNumber) {
        return false;
    }
    if (mode == FallbackModeName(FallbackMode::kFallback)) {
        state.mode = FallbackMode::kFallback;
    } else if (mode == FallbackModeName(FallbackMode::kResting)) {
        state.mode = FallbackMode::kResting;
    } else {
        return false;
    }
    if (state.playlistName.empty()) return false;
    state.sinceMillis = static_cast<TimeMillis>(since->number());

    const showmesh::json::Value* occurrence = find(parsed.value, "occurrence");
    if (occurrence != nullptr && occurrence->type() == Type::kObject) {
        const showmesh::json::Value* resolved = find(*occurrence, "identityResolved");
        const showmesh::json::Value* delivered = find(*occurrence, "delivered");
        const showmesh::json::Value* loop = find(*occurrence, "playlistLoop");
        const showmesh::json::Value* ids = find(*occurrence, "executionIds");
        if (!text_(*occurrence, "entryKey", &state.occurrence.entryKey) || resolved == nullptr ||
            resolved->type() != Type::kBool || delivered == nullptr || delivered->type() != Type::kBool ||
            ids == nullptr || ids->type() != Type::kObject) {
            return false;
        }
        state.occurrence.present = true;
        state.occurrence.identityResolved = resolved->boolean();
        state.occurrence.delivered = delivered->boolean();
        if (loop != nullptr && loop->type() == Type::kNumber) state.occurrence.playlistLoop = static_cast<int>(loop->number());
        for (const auto& id : ids->members()) {
            if (id.second.type() != Type::kString) return false;
            state.occurrence.executionIds.emplace_back(id.first, id.second.string());
        }
    }
    *out = state;
    return true;
}

inline std::string FallbackStatePath(const std::string& credentialDir) {
    return showmesh::joinPath(credentialDir, kFallbackStateFilename);
}

// Atomic, mode 0600. A normal state has no file: it is removed instead.
inline bool SaveFallbackState(const std::string& credentialDir, const FallbackExecutionState& state) {
    const std::string path = FallbackStatePath(credentialDir);
    if (state.mode == FallbackMode::kNormal) {
        std::remove(path.c_str());
        return true;
    }
    return detail::writeExecutorKeyFile(path, RenderFallbackState(state));
}

// A file that is missing or cannot be read means normal: false is returned.
inline bool LoadFallbackState(const std::string& credentialDir, FallbackExecutionState* out) {
    std::string text;
    if (!showmesh::readFileWhole(FallbackStatePath(credentialDir), &text)) return false;
    return ParseFallbackState(text, out);
}

}  // namespace fallback
}  // namespace showmesh
