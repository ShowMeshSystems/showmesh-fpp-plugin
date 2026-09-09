#pragma once

// Resolves what the installed, verified ADR-048 fallback program (Track
// J, J1) says to do for the playlist entry currently playing. This is
// the LOCAL half only: given a deterministic entry key
// (showmesh::deriveEntryKey) and the program on disk, it produces
// exactly one of a match, or a refusal with a stated reason. It never
// sends anything anywhere (decision 3's node ingress does not exist yet
// in any repository this one can build against) and it never
// substitutes a different Cue, a nearest match, or a default: a refusal
// is always the answer when the program does not say, unambiguously,
// exactly one thing for this key.
//
// Every refusal kind below stays separate on purpose, even though a
// caller could collapse several into one "not available" bucket. Each
// one is a different situation with a different operator response: "no
// program has ever arrived" is not "one arrived and expired" is not "one
// arrived and no longer verifies" (which means the file changed after
// this process wrote it: corruption or tampering, the single most
// alarming thing this resolver can discover), is not "a genuinely
// current program has nothing for this key" is not "a genuinely current
// program has two conflicting things for this key" (a coordinator
// defect, not an ordinary gap), is not "a genuinely current program
// names a Cue with no node to send it to" (a match with nothing to send
// it to is functionally the same as no delivery, but every caller above
// this resolver would otherwise read a returned match as success).

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fallback_program_installer.h"
#include "fallback_program_verifier.h"
#include "showmesh/json.h"

namespace showmesh {
namespace fallback {

enum class ActivationResolveKind {
    // Nothing installed at all: ReadInstalledFallbackProgram found no
    // file, or could not read it. The fetch/install path has never
    // succeeded for this host, or its result was removed.
    kNoProgramInstalled,
    // A file exists at the installed path, but re-verifying it just now
    // (the identical VerifyFallbackProgram check the installer's own
    // caller ran before ever writing it) refused. The file on disk is
    // not the thing this process last verified and installed: it
    // changed underneath the process, which is corruption or tampering,
    // never something this resolver treats as merely stale.
    kProgramFailedReverification,
    // The installed program verifies, but its own signed expiresAt has
    // already passed as of the caller's clock. ADR-048 (RESTING-MODE.md
    // section 11): "The fallback program ends at its declared cutoff."
    // An expired program authorizes nothing, the identical posture as no
    // program at all, but reported separately because the operator
    // response differs: this one aged out waiting for a coordinator
    // that has not published a fresh one, rather than never having
    // fetched one.
    kProgramExpired,
    // The program is current and verifies, but no entry in it carries
    // this entryKey. This is the ordinary case for most entries in a
    // real playlist: the program covers only the pre-authorized subset
    // ADR-048 decision 1 compiled, not every entry that could ever play.
    // A program compiled for a different show or a different playlist
    // reduces to exactly this outcome too: deriveEntryKey hashes
    // playlist name, playlist definition, section, and position
    // together (playlist_identity.h), so an entry from a different show
    // or playlist never collides with this key in the first place.
    // There is no separate "wrong show" check to write, because the key
    // derivation already makes a cross-show match structurally
    // impossible rather than merely unlikely.
    kUnknownEntry,
    // Two or more entries in the program carry this exact entryKey. The
    // wire format does not forbid this (nothing in the coordinator's
    // EntryMapping/Program types rejects a duplicate EntryKey), so a
    // validly signed document can still contain it. This is never
    // treated as "unknown": the key is not absent, the program is
    // internally inconsistent about what it means, which is a
    // coordinator-side defect this resolver can surface but must not
    // paper over by guessing which of the two entries is meant.
    kAmbiguousEntry,
    // Exactly one entry matches, but its targets list is empty. The
    // coordinator's own compiler is not expected to emit this (a Cue
    // with no resolved node targets), but nothing in the wire format
    // forbids it either. This is refused rather than returned as a
    // match with zero targets: a match with nothing to send it to would
    // read as success to every caller above this resolver, and a show
    // running its fallback entry while nothing happens on any node is
    // exactly what a silent outage already looks like.
    kEmptyTargets,
    // Exactly one entry matches, has at least one target, and every
    // field this resolver needs to report it is present.
    kMatch,
};

// One target node and its exact activation, copied verbatim from the
// program: this resolver never re-derives or filters what a target
// carries. A target naming neither render nor audio (the coordinator's
// own NodeTarget doc comment says its compiler never emits this, but
// nothing in the wire format forbids it) is copied through unchanged
// rather than refused here: ADR-048 decision 3 has the node ingress run
// "the same Cue activation validation ... as normal coordinator
// dispatch" before it ever acts on a target, so rejecting a
// no-activation target is that validation's job, not this resolver's.
// Refusing it here would be this resolver quietly doing part of a job
// that belongs on the other side of the boundary it does not cross.
struct ActivationTarget {
    std::string nodeId;
    std::optional<showmesh::json::Value> render;
    std::optional<showmesh::json::Value> audio;
};

// Everything a MATCH carries, restricted to fields copied verbatim from
// the verified program plus the input entryKey echoed back. See the
// header comment in adapters/shared/fallback_program_fetch.h and this
// file's own top-level comment for what ADR-048 decision 3's delivery
// path additionally needs that this type cannot supply (execution id,
// the per-host executor credential): this type is not that payload, it
// is the local half feeding one.
struct ActivationMatch {
    std::string packageId;
    std::string revision;
    std::string fppInstanceUuid;
    std::string entryKey;
    std::string cueId;
    std::int64_t cueRevision = 0;
    std::optional<std::int64_t> generation;
    std::vector<ActivationTarget> targets;
};

struct ActivationResolution {
    ActivationResolveKind kind = ActivationResolveKind::kNoProgramInstalled;
    // Populated for every kind except kMatch: why this document, right
    // now, does not authorize an activation for this key.
    std::string reason;
    // Populated only for kMatch.
    std::optional<ActivationMatch> match;
};

namespace detail {

// Strict parse of the one RFC 3339 shape the coordinator's own
// time.Time JSON encoding always produces for a UTC instant:
// YYYY-MM-DDTHH:MM:SSZ. Anything else (a fractional second, a numeric
// offset, a lowercase 'z', trailing garbage) is refused rather than
// guessed at: this field gates whether a program still authorizes
// anything, so a caller must never fail open on a shape this parser did
// not expect.
inline bool parseRfc3339Utc(const std::string& text, std::chrono::system_clock::time_point* out) {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != 'Z') {
        return false;
    }
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18}) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    const int year = std::stoi(text.substr(0, 4));
    const int month = std::stoi(text.substr(5, 2));
    const int day = std::stoi(text.substr(8, 2));
    const int hour = std::stoi(text.substr(11, 2));
    const int minute = std::stoi(text.substr(14, 2));
    const int second = std::stoi(text.substr(17, 2));
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;

    // Days-from-civil, Howard Hinnant's public-domain algorithm: exact
    // for the proleptic Gregorian calendar, no libc timezone/DST state
    // involved, so this is deterministic across build hosts.
    const int y = (month <= 2) ? year - 1 : year;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;

    const long long seconds = days * 86400LL + hour * 3600LL + minute * 60LL + second;
    *out = std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
    return true;
}

inline const showmesh::json::Value* findEntryMember(const showmesh::json::Value& object, const char* name) {
    return showmesh::fallback::detail::findMember(object, name);
}

}  // namespace detail

// ResolveActivationFromDocument is the pure core: given rawDocument (the
// exact bytes read from disk, unverified) and coordinatorPublicKey, it
// re-verifies (never trusting disk content because it was once written
// by this process), checks expiry against now, and looks up entryKey in
// the program's own entries. ResolveInstalledActivation below is the
// thin wrapper that actually reads the installed file; this function
// exists separately so a caller (or a test) that already has the bytes
// never re-reads a file to get an answer this function can already
// give.
inline ActivationResolution ResolveActivationFromDocument(const std::string& entryKey,
                                                            const std::string& rawDocument,
                                                            const std::vector<uint8_t>& coordinatorPublicKey,
                                                            std::chrono::system_clock::time_point now) {
    ActivationResolution result;

    const FallbackVerifyResult verified = VerifyFallbackProgram(rawDocument, coordinatorPublicKey);
    if (!verified.accepted) {
        result.kind = ActivationResolveKind::kProgramFailedReverification;
        result.reason = "fallback: installed program no longer verifies on read: " + verified.refusalReason;
        return result;
    }

    std::chrono::system_clock::time_point expiresAt;
    if (!detail::parseRfc3339Utc(verified.program->expiresAt(), &expiresAt) || now >= expiresAt) {
        result.kind = ActivationResolveKind::kProgramExpired;
        result.reason = "fallback: installed program's expiresAt (" + verified.program->expiresAt() +
                         ") is not a valid future RFC 3339 UTC instant as of resolution time";
        return result;
    }

    // Re-parse the verified document's own program object to reach
    // entries/generation: VerifiedFallbackProgram deliberately exposes
    // only the identity fields VerifyFallbackProgram itself needed
    // (packageId, revision, fppInstanceUuid, expiresAt), not the whole
    // tree, so this resolver parses rawDocument() again rather than
    // asking that type to widen its surface for one caller.
    const showmesh::json::ParseResult parsed = showmesh::json::parse(verified.program->rawDocument());
    const showmesh::json::Value* programValue =
        parsed.ok ? detail::findEntryMember(parsed.value, "program") : nullptr;
    if (programValue == nullptr || programValue->type() != showmesh::json::Type::kObject) {
        // Unreachable in practice: VerifyFallbackProgram already parsed
        // this exact document and required "program" to be an object.
        // Handled anyway because a resolver over signed input never
        // assumes a sibling parse of the identical bytes cannot fail.
        result.kind = ActivationResolveKind::kProgramFailedReverification;
        result.reason = "fallback: installed program document could not be re-parsed for its entries";
        return result;
    }

    const showmesh::json::Value* entriesValue = detail::findEntryMember(*programValue, "entries");
    std::vector<const showmesh::json::Value*> matches;
    if (entriesValue != nullptr && entriesValue->type() == showmesh::json::Type::kArray) {
        for (const showmesh::json::Value& entry : entriesValue->items()) {
            if (entry.type() != showmesh::json::Type::kObject) continue;
            const showmesh::json::Value* keyValue = detail::findEntryMember(entry, "entryKey");
            if (keyValue != nullptr && keyValue->type() == showmesh::json::Type::kString &&
                keyValue->string() == entryKey) {
                matches.push_back(&entry);
            }
        }
    }
    // A missing or malformed entries array matches nothing: no key was
    // ever going to be found in it, which is the same conclusion as an
    // empty array reaches, so this falls through to the same
    // kUnknownEntry path below rather than a fourth branch.

    if (matches.empty()) {
        result.kind = ActivationResolveKind::kUnknownEntry;
        result.reason = "fallback: installed program has no entry for this playlist entry's key";
        return result;
    }
    if (matches.size() > 1) {
        result.kind = ActivationResolveKind::kAmbiguousEntry;
        result.reason = "fallback: installed program has " + std::to_string(matches.size()) +
                         " entries claiming this playlist entry's key; the program is internally "
                         "inconsistent and this resolver will not guess which one is meant";
        return result;
    }

    const showmesh::json::Value& entry = *matches.front();
    const showmesh::json::Value* cueIdValue = detail::findEntryMember(entry, "cueId");
    const showmesh::json::Value* cueRevisionValue = detail::findEntryMember(entry, "cueRevision");
    const showmesh::json::Value* targetsValue = detail::findEntryMember(entry, "targets");
    if (cueIdValue == nullptr || cueIdValue->type() != showmesh::json::Type::kString || cueRevisionValue == nullptr ||
        cueRevisionValue->type() != showmesh::json::Type::kNumber || targetsValue == nullptr ||
        targetsValue->type() != showmesh::json::Type::kArray) {
        result.kind = ActivationResolveKind::kProgramFailedReverification;
        result.reason = "fallback: the one entry matching this key is missing cueId, cueRevision, or targets";
        return result;
    }

    if (targetsValue->items().empty()) {
        result.kind = ActivationResolveKind::kEmptyTargets;
        result.reason = "fallback: the entry matching this key names no node targets";
        return result;
    }

    ActivationMatch match;
    match.packageId = verified.program->packageId();
    match.revision = verified.program->revision();
    match.fppInstanceUuid = verified.program->fppInstanceUuid();
    match.entryKey = entryKey;
    match.cueId = cueIdValue->string();
    match.cueRevision = static_cast<std::int64_t>(cueRevisionValue->number());

    const showmesh::json::Value* generationValue = detail::findEntryMember(*programValue, "generation");
    if (generationValue != nullptr && generationValue->type() == showmesh::json::Type::kNumber) {
        match.generation = static_cast<std::int64_t>(generationValue->number());
    }

    for (const showmesh::json::Value& targetValue : targetsValue->items()) {
        if (targetValue.type() != showmesh::json::Type::kObject) continue;
        const showmesh::json::Value* nodeIdValue = detail::findEntryMember(targetValue, "nodeId");
        if (nodeIdValue == nullptr || nodeIdValue->type() != showmesh::json::Type::kString) continue;

        ActivationTarget target;
        target.nodeId = nodeIdValue->string();
        const showmesh::json::Value* renderValue = detail::findEntryMember(targetValue, "render");
        if (renderValue != nullptr && renderValue->type() != showmesh::json::Type::kNull) {
            target.render = *renderValue;
        }
        const showmesh::json::Value* audioValue = detail::findEntryMember(targetValue, "audio");
        if (audioValue != nullptr && audioValue->type() != showmesh::json::Type::kNull) {
            target.audio = *audioValue;
        }
        match.targets.push_back(std::move(target));
    }

    if (match.targets.empty()) {
        // Every item in a non-empty targets array was itself malformed
        // (not an object, or missing/wrong-typed nodeId): the same
        // "nothing to send this to" situation kEmptyTargets exists for,
        // reached by a different route.
        result.kind = ActivationResolveKind::kEmptyTargets;
        result.reason = "fallback: the entry matching this key names targets, but none carry a usable nodeId";
        return result;
    }

    result.kind = ActivationResolveKind::kMatch;
    result.match = std::move(match);
    return result;
}

// ResolveInstalledActivation reads the installed program from path
// (ReadInstalledFallbackProgram, fallback_program_installer.h) and
// resolves entryKey against it. A file that cannot be read at all is
// kNoProgramInstalled; everything else is
// ResolveActivationFromDocument's answer.
inline ActivationResolution ResolveInstalledActivation(const std::string& entryKey, const std::string& path,
                                                         const std::vector<uint8_t>& coordinatorPublicKey,
                                                         std::chrono::system_clock::time_point now) {
    const ReadInstalledResult read = ReadInstalledFallbackProgram(path);
    if (!read.ok) {
        ActivationResolution result;
        result.kind = ActivationResolveKind::kNoProgramInstalled;
        result.reason = read.error;
        return result;
    }
    return ResolveActivationFromDocument(entryKey, read.rawDocument, coordinatorPublicKey, now);
}

}  // namespace fallback
}  // namespace showmesh
