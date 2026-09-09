#pragma once

// Fetches the coordinator's signed ADR-048 fallback program (Track J,
// J1), hands the bytes to VerifyFallbackProgram unchanged, installs an
// accepted program, and acknowledges the outcome. This file is a BYTE
// SOURCE and nothing more: it never constructs a VerifiedFallbackProgram
// itself (it cannot: that constructor is private to VerifyFallbackProgram,
// see fallback_program_verifier.h), and it never writes to disk except
// through InstallFallbackProgram. It has no timer, no thread, and no
// retry loop: FetchAndInstallFallbackProgram is a function a caller
// invokes once: when to call it again is that caller's decision, not
// this file's.
//
// The coordinator being unreachable, or having nothing published for
// this host, is the expected steady state for most of this feature's
// life, not a rare edge case, so every branch below ends in a stated,
// reportable outcome. None of them crash and none of them silently do
// nothing.

#include <cstdint>
#include <string>
#include <vector>

#include "fallback_program_installer.h"
#include "fallback_program_verifier.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/http_transport.h"
#include "showmesh/json.h"
#include "showmesh/runtime.h"

namespace showmesh {
namespace fallback {

// GET /api/v1/fallback-programs/{fppInstanceId}'s response
// (v1.FallbackProgramResponse) wraps the signed content: "program" is
// the coordinator's stored program bytes sliced out verbatim (Go
// json.RawMessage, never re-marshaled: see that type's own doc comment
// for why a re-derivation, however faithful it looks, can change the
// VALUE and not merely the formatting, and would then fail the signature
// at this exact hop), and "signatureBase64" travels as that object's own
// SIBLING field, never nested inside "program". That is a different
// shape from the signed document VerifyFallbackProgram expects
// ({"program":...,"signature":"<base64>"} exactly, matching
// pkg/fallbackprogram.SignedProgram's own JSON tags, the shape this
// plugin's own stored/tested fixtures use). Reconciling the two is this
// file's job, not the verifier's: the verifier's contract is "does this
// signed document verify", and it should never need to know the HTTP
// envelope shape a caller happened to fetch it through.
enum class FallbackFetchOutcomeKind {
    // A resolvable credential could not be obtained; no network request
    // was made.
    kCredentialUnavailable,
    // No HTTP response at all: DNS, connect, TLS, or timeout.
    kTransportUnreachable,
    // An HTTP status other than 200. A literal 404 on this route lands
    // here too: the coordinator's own handler never returns one for "no
    // program yet" (see kNotPublished), so a real 404 means a wrong base
    // URL, a proxy, or something else worth an operator's attention, not
    // a shape this file has a specific answer for.
    kUnexpectedStatus,
    // A well-formed, ordinary "nothing to see yet": HTTP 200 with
    // published:false. This is ADR-048's honest-absence case (a fresh
    // plugin, or a host outside any active show), not a failure: a fresh
    // install would otherwise report itself broken for doing nothing
    // wrong.
    kNotPublished,
    // HTTP 200 but the body is not JSON, or is JSON but missing
    // "program" or "signatureBase64", or "program" is not an object, or
    // "signatureBase64" is not a string. Nothing here was ever
    // recognizable as a program, so nothing is acknowledged.
    kMalformedEnvelope,
    // The reconstructed signed document reached VerifyFallbackProgram
    // and it refused: a bad signature, a wrong key, a tampered byte, or
    // similar. This is "fetched, but rejected" (ADR-048's acknowledge
    // vocabulary), reported to the coordinator as "signature-invalid".
    kVerificationRefused,
    // Verified, but the program's OWN signed fppInstanceUuid does not
    // match the host this fetch was made for. The signature was
    // perfectly valid; it simply was not for this host. Reported to the
    // coordinator as "mismatched-program", never "signature-invalid":
    // saying otherwise would report a forgery that did not happen.
    kInstanceMismatch,
    // Verified and matches this host, but the program's OWN signed
    // expiresAt has already passed. Also "fetched, but rejected", also
    // "mismatched-program": the signature holds, the content is simply
    // no longer current.
    kExpired,
    // The only success outcome: verified, matches this host, not
    // expired, and durably installed.
    kInstalled,
};

// The enum value's own spelling, the identical "grep the name, find the
// code and the log" rule ActivationResolveKindName()
// (fallback_activation_resolver.h) states for its own vocabulary.
inline const char* FallbackFetchOutcomeKindName(FallbackFetchOutcomeKind kind) {
    switch (kind) {
        case FallbackFetchOutcomeKind::kCredentialUnavailable:
            return "kCredentialUnavailable";
        case FallbackFetchOutcomeKind::kTransportUnreachable:
            return "kTransportUnreachable";
        case FallbackFetchOutcomeKind::kUnexpectedStatus:
            return "kUnexpectedStatus";
        case FallbackFetchOutcomeKind::kNotPublished:
            return "kNotPublished";
        case FallbackFetchOutcomeKind::kMalformedEnvelope:
            return "kMalformedEnvelope";
        case FallbackFetchOutcomeKind::kVerificationRefused:
            return "kVerificationRefused";
        case FallbackFetchOutcomeKind::kInstanceMismatch:
            return "kInstanceMismatch";
        case FallbackFetchOutcomeKind::kExpired:
            return "kExpired";
        case FallbackFetchOutcomeKind::kInstalled:
            return "kInstalled";
    }
    return "kUnknown";
}

// The vocabulary POST .../acknowledge actually accepts
// (v1.FallbackProgramAcknowledgeRequest.VerificationResult), a CLOSED,
// three-member set that is deliberately coarser than
// FallbackFetchOutcomeKind above: it exists so the coordinator can tell
// "never fetched" apart from "fetched, but rejected", not to carry this
// file's own diagnostic detail. kCredentialUnavailable,
// kTransportUnreachable, kNotPublished, and kMalformedEnvelope never
// reach this vocabulary at all: see ShouldAcknowledge below.
constexpr const char* kVerificationResultVerified = "verified";
constexpr const char* kVerificationResultSignatureInvalid = "signature-invalid";
constexpr const char* kVerificationResultMismatchedProgram = "mismatched-program";

struct FallbackFetchOutcome {
    FallbackFetchOutcomeKind kind = FallbackFetchOutcomeKind::kTransportUnreachable;
    // Operator-facing detail. Never the credential.
    std::string detail;
    // Populated only for kUnexpectedStatus.
    int statusCode = 0;
    // Populated only for kInstalled.
    InstalledFallbackProgramReport installedReport;
    // Populated for kVerificationRefused, kInstanceMismatch, kExpired,
    // and kInstalled: what an acknowledge call, if one is made, reports.
    // Best-effort for the three refusal kinds (read from the fetched but
    // untrusted program bytes, since a rejected program was never handed
    // to VerifyFallbackProgram's accept path and so has no
    // VerifiedFallbackProgram to ask), exact for kInstalled (read from
    // the VerifiedFallbackProgram the installer actually wrote).
    std::string packageId;
    std::string revision;
};

// Whether outcome represents "a program was actually fetched and a
// verdict was reached about it", the only case ADR-048's acknowledge
// route exists to report. The four kinds excluded here (no credential,
// no transport, no program published, not even a recognizable envelope)
// are "nothing to acknowledge", never a rejection: acknowledging one of
// them would tell the coordinator this host rejected a program it never
// received.
inline bool ShouldAcknowledgeFallbackFetchOutcome(const FallbackFetchOutcome& outcome) {
    switch (outcome.kind) {
        case FallbackFetchOutcomeKind::kVerificationRefused:
        case FallbackFetchOutcomeKind::kInstanceMismatch:
        case FallbackFetchOutcomeKind::kExpired:
        case FallbackFetchOutcomeKind::kInstalled:
            return true;
        case FallbackFetchOutcomeKind::kCredentialUnavailable:
        case FallbackFetchOutcomeKind::kTransportUnreachable:
        case FallbackFetchOutcomeKind::kUnexpectedStatus:
        case FallbackFetchOutcomeKind::kNotPublished:
        case FallbackFetchOutcomeKind::kMalformedEnvelope:
            return false;
    }
    return false;
}

// The verificationResult value ShouldAcknowledgeFallbackFetchOutcome's
// true cases report, or an empty string for a kind that is never
// acknowledged (a caller that checks ShouldAcknowledgeFallbackFetchOutcome
// first, as it must, never sees the empty case).
inline std::string FallbackFetchOutcomeVerificationResult(FallbackFetchOutcomeKind kind) {
    switch (kind) {
        case FallbackFetchOutcomeKind::kInstalled:
            return kVerificationResultVerified;
        case FallbackFetchOutcomeKind::kVerificationRefused:
            return kVerificationResultSignatureInvalid;
        case FallbackFetchOutcomeKind::kInstanceMismatch:
        case FallbackFetchOutcomeKind::kExpired:
            return kVerificationResultMismatchedProgram;
        default:
            return std::string();
    }
}

namespace detail {

// Skips exactly one JSON value (string, number, true/false/null, object,
// or array) starting at *pos, advancing *pos past it. This exists ONLY
// to find where a value's bytes END, never to interpret what the value
// MEANS: extractRawRootMember below uses it to slice out "program"'s raw
// span, so the bytes it hands the verifier are never re-derived through
// any parse-and-re-emit step. A malformed input returns false; it never
// reads past text.size().
inline bool skipJsonValue(const std::string& text, std::size_t* pos);

inline void skipJsonWhitespace(const std::string& text, std::size_t* pos) {
    while (*pos < text.size()) {
        const char c = text[*pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++*pos;
        } else {
            break;
        }
    }
}

inline bool skipJsonStringLiteral(const std::string& text, std::size_t* pos) {
    if (*pos >= text.size() || text[*pos] != '"') return false;
    ++*pos;
    while (*pos < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[*pos]);
        if (c == '"') {
            ++*pos;
            return true;
        }
        if (c == '\\') {
            ++*pos;
            if (*pos >= text.size()) return false;
            ++*pos;
            continue;
        }
        if (c < 0x20) return false;  // raw control character: not valid JSON
        ++*pos;
    }
    return false;
}

inline bool skipJsonContainer(const std::string& text, std::size_t* pos, char open, char close) {
    if (*pos >= text.size() || text[*pos] != open) return false;
    ++*pos;
    skipJsonWhitespace(text, pos);
    if (*pos < text.size() && text[*pos] == close) {
        ++*pos;
        return true;
    }
    while (true) {
        if (open == '{') {
            skipJsonWhitespace(text, pos);
            if (!skipJsonStringLiteral(text, pos)) return false;
            skipJsonWhitespace(text, pos);
            if (*pos >= text.size() || text[*pos] != ':') return false;
            ++*pos;
        }
        if (!skipJsonValue(text, pos)) return false;
        skipJsonWhitespace(text, pos);
        if (*pos >= text.size()) return false;
        if (text[*pos] == ',') {
            ++*pos;
            skipJsonWhitespace(text, pos);
            continue;
        }
        if (text[*pos] == close) {
            ++*pos;
            return true;
        }
        return false;
    }
}

inline bool skipJsonValue(const std::string& text, std::size_t* pos) {
    skipJsonWhitespace(text, pos);
    if (*pos >= text.size()) return false;
    const char c = text[*pos];
    if (c == '"') return skipJsonStringLiteral(text, pos);
    if (c == '{') return skipJsonContainer(text, pos, '{', '}');
    if (c == '[') return skipJsonContainer(text, pos, '[', ']');
    if (text.compare(*pos, 4, "true") == 0) {
        *pos += 4;
        return true;
    }
    if (text.compare(*pos, 5, "false") == 0) {
        *pos += 5;
        return true;
    }
    if (text.compare(*pos, 4, "null") == 0) {
        *pos += 4;
        return true;
    }
    const std::size_t start = *pos;
    if (*pos < text.size() && text[*pos] == '-') ++*pos;
    bool sawDigit = false;
    while (*pos < text.size() && text[*pos] >= '0' && text[*pos] <= '9') {
        ++*pos;
        sawDigit = true;
    }
    if (!sawDigit) {
        *pos = start;
        return false;
    }
    if (*pos < text.size() && text[*pos] == '.') {
        ++*pos;
        while (*pos < text.size() && text[*pos] >= '0' && text[*pos] <= '9') ++*pos;
    }
    if (*pos < text.size() && (text[*pos] == 'e' || text[*pos] == 'E')) {
        ++*pos;
        if (*pos < text.size() && (text[*pos] == '+' || text[*pos] == '-')) ++*pos;
        while (*pos < text.size() && text[*pos] >= '0' && text[*pos] <= '9') ++*pos;
    }
    return true;
}

// Finds member's raw JSON text (including surrounding quotes for a
// string, braces for an object, and so on) at the ROOT of a top-level
// JSON object, or returns false if text is not an object, is malformed,
// or has no such member. The member-name comparison is a literal byte
// match against the quoted key exactly as it appears on the wire, with
// no escape decoding: an attacker who escapes a character in a key
// (p for 'p', for instance) makes this function fail to find that
// key, never mistake a different key for it, because the raw bytes this
// function compares against are the un-escaped literal spelling of
// member and nothing else can equal them.
inline bool extractRawRootMember(const std::string& text, const std::string& member, std::string* outRawValue) {
    std::size_t pos = 0;
    skipJsonWhitespace(text, &pos);
    if (pos >= text.size() || text[pos] != '{') return false;
    ++pos;
    skipJsonWhitespace(text, &pos);
    if (pos < text.size() && text[pos] == '}') return false;  // empty object: no member has this name

    const std::string quotedMember = "\"" + member + "\"";
    while (true) {
        skipJsonWhitespace(text, &pos);
        const std::size_t keyStart = pos;
        if (!skipJsonStringLiteral(text, &pos)) return false;
        const bool isMatch = text.compare(keyStart, pos - keyStart, quotedMember) == 0;
        skipJsonWhitespace(text, &pos);
        if (pos >= text.size() || text[pos] != ':') return false;
        ++pos;
        skipJsonWhitespace(text, &pos);
        const std::size_t valueStart = pos;
        if (!skipJsonValue(text, &pos)) return false;
        if (isMatch) {
            *outRawValue = text.substr(valueStart, pos - valueStart);
            return true;
        }
        skipJsonWhitespace(text, &pos);
        if (pos >= text.size()) return false;
        if (text[pos] == ',') {
            ++pos;
            continue;
        }
        if (text[pos] == '}') return false;  // reached the end without finding member
        return false;
    }
}

// Parses an RFC 3339 timestamp (the only shape Go's encoding/json ever
// produces for a time.Time: "YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)")
// into epoch seconds. Sub-second precision is truncated: this function
// exists only to compare "has this expired" against a clock in whole
// seconds, never to reproduce the timestamp exactly.
inline bool parseRfc3339ToEpochSeconds(const std::string& s, std::int64_t* outEpochSeconds) {
    auto digit = [&](std::size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
    if (s.size() < 20) return false;
    for (std::size_t i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18}) {
        if (!digit(i)) return false;
    }
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') return false;

    const int year = std::stoi(s.substr(0, 4));
    const int month = std::stoi(s.substr(5, 2));
    const int day = std::stoi(s.substr(8, 2));
    const int hour = std::stoi(s.substr(11, 2));
    const int minute = std::stoi(s.substr(14, 2));
    const int second = std::stoi(s.substr(17, 2));
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;

    std::size_t pos = 19;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        const std::size_t fractionStart = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        if (pos == fractionStart) return false;  // a bare '.' with no digits
    }

    long offsetSeconds = 0;
    if (pos >= s.size()) return false;
    if (s[pos] == 'Z' || s[pos] == 'z') {
        ++pos;
    } else if (s[pos] == '+' || s[pos] == '-') {
        if (pos + 6 > s.size() || !digit(pos + 1) || !digit(pos + 2) || s[pos + 3] != ':' || !digit(pos + 4) ||
            !digit(pos + 5)) {
            return false;
        }
        const int sign = s[pos] == '-' ? -1 : 1;
        const int offsetHours = std::stoi(s.substr(pos + 1, 2));
        const int offsetMinutes = std::stoi(s.substr(pos + 4, 2));
        offsetSeconds = sign * (offsetHours * 3600 + offsetMinutes * 60);
        pos += 6;
    } else {
        return false;
    }
    if (pos != s.size()) return false;

    // Howard Hinnant's days_from_civil: a standard, well-tested
    // proleptic-Gregorian civil-date-to-days-since-epoch conversion, not
    // a bespoke calendar routine.
    int y = year;
    const unsigned m = static_cast<unsigned>(month);
    const unsigned d = static_cast<unsigned>(day);
    y -= (m <= 2) ? 1 : 0;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097 + static_cast<long>(doe) - 719468;

    *outEpochSeconds = static_cast<std::int64_t>(days) * 86400 + hour * 3600 + minute * 60 + second - offsetSeconds;
    return true;
}

// The inverse of parseRfc3339ToEpochSeconds, always rendering UTC ("Z"):
// installedAt in the acknowledge request is this plugin's own clock
// reading at install time, never a value round-tripped from anything the
// coordinator sent.
inline std::string formatEpochMillisAsRfc3339(TimeMillis epochMillis) {
    std::int64_t epochSeconds = epochMillis / 1000;
    if (epochMillis < 0 && epochMillis % 1000 != 0) --epochSeconds;  // floor toward negative infinity

    // Howard Hinnant's civil_from_days: the inverse of days_from_civil
    // above.
    std::int64_t days = epochSeconds >= 0 ? epochSeconds / 86400 : -((-epochSeconds + 86399) / 86400);
    std::int64_t secondsOfDay = epochSeconds - days * 86400;

    const long z = days + 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long y = static_cast<long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    const long year = y + (m <= 2 ? 1 : 0);

    const int hour = static_cast<int>(secondsOfDay / 3600);
    const int minute = static_cast<int>((secondsOfDay % 3600) / 60);
    const int second = static_cast<int>(secondsOfDay % 60);

    // Sized well past any value these fields can actually take (a
    // four-digit year for centuries around the present, in particular),
    // so GCC's format-truncation checker, which sizes for the full
    // range of `long`, has no worst case left to complain about.
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04ld-%02u-%02uT%02d:%02d:%02dZ", year, m, d, hour, minute, second);
    return std::string(buffer);
}

// Best-effort read of a rejected (never-verified) program's own claimed
// packageId/revision, for the acknowledge call only: reporting WHICH
// package a host rejected is useful evidence even though the plugin
// never trusted its content. Never used for anything that installs or
// verifies; VerifyFallbackProgram alone decides that.
inline void peekUnverifiedIdentity(const std::string& rawProgramJson, std::string* packageId,
                                    std::string* revision) {
    const showmesh::json::ParseResult parsed = showmesh::json::parse(rawProgramJson);
    if (!parsed.ok || parsed.value.type() != showmesh::json::Type::kObject) return;
    for (const auto& member : parsed.value.members()) {
        if (member.first == "packageId" && member.second.type() == showmesh::json::Type::kString) {
            *packageId = member.second.string();
        } else if (member.first == "revision" && member.second.type() == showmesh::json::Type::kString) {
            *revision = member.second.string();
        }
    }
}

}  // namespace detail

// Fetches the fallback program GET /api/v1/fallback-programs/{fppInstanceId}
// publishes for expectedFppInstanceUuid, verifies it against
// coordinatorPublicKey, checks it actually names expectedFppInstanceUuid
// and has not expired, and installs it at installPath if all of that
// holds. transport and credentials are the same seams the observation
// path already uses (CurlHttpTransport, FileCredentialSource in
// production); this function neither retries nor sleeps: one call is
// exactly one attempt, and a caller decides whether and when to call it
// again.
inline FallbackFetchOutcome FetchAndInstallFallbackProgram(HttpTransport* transport, CredentialSource* credentials,
                                                            const std::string& baseUrl,
                                                            const std::string& expectedFppInstanceUuid,
                                                            const std::vector<uint8_t>& coordinatorPublicKey,
                                                            const std::string& installPath, Clock clock) {
    FallbackFetchOutcome outcome;

    std::string token;
    std::string credentialError;
    if (credentials == nullptr || !credentials->token(&token, &credentialError)) {
        outcome.kind = FallbackFetchOutcomeKind::kCredentialUnavailable;
        outcome.detail = "fallback: no usable credential: " + credentialError;
        return outcome;
    }

    HttpRequest request;
    request.url = joinUrlPath(baseUrl, "/api/v1/fallback-programs/" + expectedFppInstanceUuid);
    // A real fallback program is many playlist entries times node targets
    // times asset hashes, not a short refusal body: 1 MiB is generous
    // for that shape while still being a bound, never "however large the
    // coordinator feels like sending".
    request.maxResponseBytes = 1 << 20;
    request.bearerToken = token;

    const HttpResponse response = transport->get(request);
    if (!response.transportOk) {
        outcome.kind = FallbackFetchOutcomeKind::kTransportUnreachable;
        outcome.detail = response.error.empty() ? "fallback: no response from coordinator" : response.error;
        return outcome;
    }
    if (response.statusCode != 200) {
        outcome.kind = FallbackFetchOutcomeKind::kUnexpectedStatus;
        outcome.statusCode = response.statusCode;
        outcome.detail = "fallback: unexpected status " + std::to_string(response.statusCode);
        return outcome;
    }

    const showmesh::json::ParseResult envelope = showmesh::json::parse(response.body);
    if (!envelope.ok || envelope.value.type() != showmesh::json::Type::kObject) {
        outcome.kind = FallbackFetchOutcomeKind::kMalformedEnvelope;
        outcome.detail = "fallback: response body is not a JSON object";
        return outcome;
    }

    bool published = false;
    for (const auto& member : envelope.value.members()) {
        if (member.first == "published" && member.second.type() == showmesh::json::Type::kBool) {
            published = member.second.boolean();
        }
    }
    if (!published) {
        outcome.kind = FallbackFetchOutcomeKind::kNotPublished;
        outcome.detail = "fallback: coordinator has no program published for this host";
        return outcome;
    }

    // Checked against the already-parsed envelope BEFORE the raw-span
    // extraction below, not after: extractRawRootMember only locates
    // program's bytes, it does not know or care what JSON type they are,
    // so a "program" that is a string, a number, or an array would
    // otherwise reach VerifyFallbackProgram, get refused there for a
    // reason that has nothing to do with the signature, and then be
    // acknowledged to the coordinator as "signature-invalid": a forgery
    // report for a forgery that never happened. This is the identical
    // false-forgery mistake kInstanceMismatch/kExpired exist to avoid
    // for a wrong host or an expired program, one layer further out.
    bool haveProgramObject = false;
    for (const auto& member : envelope.value.members()) {
        if (member.first == "program" && member.second.type() == showmesh::json::Type::kObject) {
            haveProgramObject = true;
        }
    }
    if (!haveProgramObject) {
        outcome.kind = FallbackFetchOutcomeKind::kMalformedEnvelope;
        outcome.detail = "fallback: program field is missing or is not a JSON object";
        return outcome;
    }

    std::string rawProgram;
    std::string signatureBase64;
    bool haveSignature = false;
    if (!detail::extractRawRootMember(response.body, "program", &rawProgram)) {
        outcome.kind = FallbackFetchOutcomeKind::kMalformedEnvelope;
        outcome.detail = "fallback: response body has no program field";
        return outcome;
    }
    for (const auto& member : envelope.value.members()) {
        if (member.first == "signatureBase64" && member.second.type() == showmesh::json::Type::kString) {
            signatureBase64 = member.second.string();
            haveSignature = true;
        }
    }
    if (!haveSignature) {
        outcome.kind = FallbackFetchOutcomeKind::kMalformedEnvelope;
        outcome.detail = "fallback: response body has no signatureBase64 field";
        return outcome;
    }

    // Reconstructed by concatenation, never by parsing rawProgram and
    // re-emitting it: rawProgram is already the exact bytes the
    // coordinator signed, and any re-derivation risks producing a
    // different byte sequence for a value that still LOOKS identical
    // (see this file's own top comment).
    std::string reconstructedDocument;
    reconstructedDocument.reserve(rawProgram.size() + signatureBase64.size() + 32);
    reconstructedDocument += "{\"program\":";
    reconstructedDocument += rawProgram;
    reconstructedDocument += ",\"signature\":\"";
    reconstructedDocument += signatureBase64;
    reconstructedDocument += "\"}";

    const FallbackVerifyResult verified = VerifyFallbackProgram(reconstructedDocument, coordinatorPublicKey);
    if (!verified.accepted) {
        outcome.kind = FallbackFetchOutcomeKind::kVerificationRefused;
        outcome.detail = verified.refusalReason;
        detail::peekUnverifiedIdentity(rawProgram, &outcome.packageId, &outcome.revision);
        return outcome;
    }

    if (verified.program->fppInstanceUuid() != expectedFppInstanceUuid) {
        outcome.kind = FallbackFetchOutcomeKind::kInstanceMismatch;
        outcome.detail = "fallback: program is signed for a different fppInstanceUuid";
        outcome.packageId = verified.program->packageId();
        outcome.revision = verified.program->revision();
        return outcome;
    }

    std::int64_t expiresAtEpochSeconds = 0;
    const bool expiryParsed = detail::parseRfc3339ToEpochSeconds(verified.program->expiresAt(), &expiresAtEpochSeconds);
    const std::int64_t nowEpochSeconds = clock() / 1000;
    if (!expiryParsed || expiresAtEpochSeconds < nowEpochSeconds) {
        outcome.kind = FallbackFetchOutcomeKind::kExpired;
        outcome.detail = expiryParsed ? "fallback: program's expiresAt has already passed"
                                       : "fallback: program's expiresAt could not be parsed";
        outcome.packageId = verified.program->packageId();
        outcome.revision = verified.program->revision();
        return outcome;
    }

    const InstallResult installed = InstallFallbackProgram(*verified.program, installPath);
    if (!installed.ok) {
        // Not its own outcome kind: an install failure after a good
        // verification is still "verified", just not durably recorded,
        // and InstallFallbackProgram's own guarantee (the previous
        // program on disk is untouched) already held before this call
        // returns. Reported as a refusal so a caller does not read a
        // failed install as success.
        outcome.kind = FallbackFetchOutcomeKind::kVerificationRefused;
        outcome.detail = installed.refusalReason;
        outcome.packageId = verified.program->packageId();
        outcome.revision = verified.program->revision();
        return outcome;
    }

    outcome.kind = FallbackFetchOutcomeKind::kInstalled;
    outcome.installedReport = installed.report;
    outcome.packageId = installed.report.packageId;
    outcome.revision = installed.report.revision;
    return outcome;
}

struct AcknowledgeResult {
    bool ok = false;
    std::string error;  // populated only when !ok
    int statusCode = 0;
};

// Sends exactly the four fields the coordinator's acknowledge route
// accepts (DisallowUnknownFields server-side: sending a fifth field
// refuses the whole call), never more. Call this only when
// ShouldAcknowledgeFallbackFetchOutcome(outcome) is true: this function
// does not check that itself, so that a caller cannot reach it by
// accident without first reading the one-sentence rule that guards it.
inline AcknowledgeResult AcknowledgeFallbackProgram(HttpTransport* transport, CredentialSource* credentials,
                                                     const std::string& baseUrl,
                                                     const std::string& expectedFppInstanceUuid,
                                                     const FallbackFetchOutcome& outcome, Clock clock) {
    AcknowledgeResult result;

    std::string token;
    std::string credentialError;
    if (credentials == nullptr || !credentials->token(&token, &credentialError)) {
        result.error = "fallback: no usable credential: " + credentialError;
        return result;
    }

    showmesh::json::Value body = showmesh::json::Value::makeObject({
        {"packageId", showmesh::json::Value::makeString(outcome.packageId)},
        {"revision", showmesh::json::Value::makeString(outcome.revision)},
        {"verificationResult",
         showmesh::json::Value::makeString(FallbackFetchOutcomeVerificationResult(outcome.kind))},
        {"installedAt", showmesh::json::Value::makeString(detail::formatEpochMillisAsRfc3339(clock()))},
    });
    const showmesh::json::CanonicalResult rendered = showmesh::json::canonicalize(body);
    if (!rendered.ok) {
        result.error = "fallback: could not render acknowledge body: " + rendered.error;
        return result;
    }

    HttpRequest request;
    request.url = joinUrlPath(baseUrl, "/api/v1/fallback-programs/" + expectedFppInstanceUuid + "/acknowledge");
    request.body = rendered.text;
    request.bearerToken = token;

    const HttpResponse response = transport->post(request);
    if (!response.transportOk) {
        result.error = response.error.empty() ? "fallback: no response from coordinator" : response.error;
        return result;
    }
    result.statusCode = response.statusCode;
    if (response.statusCode != 200) {
        result.error = "fallback: acknowledge refused with status " + std::to_string(response.statusCode);
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace fallback
}  // namespace showmesh
