#pragma once

// Verifies a coordinator-signed ADR-048 fallback program (Track J, J1):
// checks the Ed25519 signature over the program's RFC 8785 JSON
// Canonicalization Scheme (JCS) bytes, the identical check the
// coordinator's own pkg/fallbackprogram.SignedProgram.Verify makes,
// using this repository's own JCS implementation (native/src/json.cpp,
// showmesh::json::canonicalize) so both sides hash identical bytes
// rather than a second, independently derived serialization.
//
// This file verifies. It does not decide what a refusal means to a
// caller, does not choose a replacement Cue, and does not touch
// playback: ADR-048 confines the fallback program to node agents, and
// this repository's own boundary stops at "does this document verify."

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "showmesh/json.h"

namespace showmesh {
namespace fallback {

struct FallbackVerifyResult;

// Forward declaration only: VerifiedFallbackProgram's constructor names
// this exact signature as its one friend, and the definition below (the
// same declaration, repeated, as C++ requires for a function defined
// after the class that friends it) is what a caller actually calls.
inline FallbackVerifyResult VerifyFallbackProgram(const std::string& signedProgramDocument,
                                                   const std::vector<uint8_t>& coordinatorPublicKey);

// One accepted fallback program: the exact bytes verified, byte for
// byte, never a re-serialization, mirroring the coordinator's own
// schemaV25 rule that a re-fetch replays the exact signed bytes rather
// than re-marshaling them at read time.
//
// THE PRIVATE CONSTRUCTOR IS THE POINT, NOT AN IMPLEMENTATION DETAIL.
// This type has no public constructor and no setters: the only way one
// of these exists is that VerifyFallbackProgram's friend constructor
// call ran, checked the Ed25519 signature, and accepted. Holding a
// VerifiedFallbackProgram is therefore itself the proof that a signature
// check happened, not a claim a caller could otherwise fabricate; a
// caller cannot express "install this document I did not verify" in
// code that compiles. InstallFallbackProgram (fallback_program_
// installer.h) trusts exactly this and nothing more. Widening this
// class (a public constructor, a setter, a non-const accessor exposing
// the fields by reference) removes the one thing standing between "the
// installer only ever writes a verified program" and "the installer
// writes whatever a caller hands it," so do not widen it without
// re-deriving why that guarantee still holds.
class VerifiedFallbackProgram {
 public:
    const std::string& packageId() const { return packageId_; }
    const std::string& revision() const { return revision_; }
    const std::string& rawDocument() const { return rawDocument_; }
    // The signed content's OWN claim of which FPP host this program is
    // for. The Ed25519 check never evaluates this: a program honestly
    // signed by the coordinator for a different host verifies cleanly
    // here too, so a caller that fetched this program on behalf of a
    // specific host must compare this against that host's own
    // configured instance id itself. Slice two's fetch does exactly
    // that.
    const std::string& fppInstanceUuid() const { return fppInstanceUuid_; }
    // The signed content's OWN claimed expiry (RFC 3339). The Ed25519
    // check never evaluates this either: a validly signed, long-expired
    // program stays validly signed forever, so "is this still current"
    // is a freshness check a caller makes against this field, never
    // something a signature can answer by itself.
    const std::string& expiresAt() const { return expiresAt_; }

 private:
    friend FallbackVerifyResult VerifyFallbackProgram(const std::string&, const std::vector<uint8_t>&);

    VerifiedFallbackProgram(std::string packageId, std::string revision, std::string rawDocument,
                             std::string fppInstanceUuid, std::string expiresAt)
        : packageId_(std::move(packageId)),
          revision_(std::move(revision)),
          rawDocument_(std::move(rawDocument)),
          fppInstanceUuid_(std::move(fppInstanceUuid)),
          expiresAt_(std::move(expiresAt)) {}

    std::string packageId_;
    std::string revision_;
    std::string rawDocument_;
    std::string fppInstanceUuid_;
    std::string expiresAt_;
};

struct FallbackVerifyResult {
    bool accepted = false;
    // Populated only when !accepted: why this document was refused. A
    // caller reports this as-is; it is not free-form diagnostic text
    // meant only for a log.
    std::string refusalReason;
    // Populated only when accepted: std::optional rather than a plain
    // value because VerifiedFallbackProgram deliberately has no default
    // constructor for a refusal case to default-initialize.
    std::optional<VerifiedFallbackProgram> program;
};

namespace detail {

// Strict RFC 4648 base64 decode with required padding, the identical
// encoding Go's encoding/json gives a []byte field (coordsig.Signature)
// via encoding/base64.StdEncoding. The signature is a trust boundary, so
// this rejects anything that is not a well-formed encoding rather than
// tolerating garbage input the way a permissive decoder would.
inline const std::array<int8_t, 256>& base64LookupTable() {
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        const char* alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
        }
        return t;
    }();
    return table;
}

inline bool base64Decode(const std::string& in, std::vector<uint8_t>* out) {
    if (in.empty() || in.size() % 4 != 0) return false;
    const auto& table = base64LookupTable();
    out->clear();
    out->reserve((in.size() / 4) * 3);

    for (size_t i = 0; i < in.size(); i += 4) {
        int vals[4];
        for (int j = 0; j < 4; ++j) {
            const char c = in[i + j];
            if (c == '=') {
                // Padding is only ever valid in the document's last group.
                if (i + 4 != in.size()) return false;
                vals[j] = -2;
            } else {
                const int8_t v = table[static_cast<unsigned char>(c)];
                if (v < 0) return false;
                vals[j] = v;
            }
        }
        // The first two characters of a group are never padding, and a
        // padded third character requires the fourth to be padded too:
        // the only valid shapes are XXXX, XXX=, XX==.
        if (vals[0] == -2 || vals[1] == -2) return false;
        if (vals[2] == -2 && vals[3] != -2) return false;

        uint32_t triple = (static_cast<uint32_t>(vals[0]) << 18) | (static_cast<uint32_t>(vals[1]) << 12);
        out->push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
        if (vals[2] != -2) {
            triple |= static_cast<uint32_t>(vals[2]) << 6;
            out->push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
            if (vals[3] != -2) {
                triple |= static_cast<uint32_t>(vals[3]);
                out->push_back(static_cast<uint8_t>(triple & 0xFF));
            }
        }
    }
    return true;
}

inline const showmesh::json::Value* findMember(const showmesh::json::Value& object, const char* name) {
    for (const auto& member : object.members()) {
        if (member.first == name) return &member.second;
    }
    return nullptr;
}

}  // namespace detail

// Verifies signedProgramDocument (the exact bytes fetched from
// GET /fallback-programs/{fppInstanceId}) against coordinatorPublicKey
// (a raw 32-byte Ed25519 public key). Every failure mode, a parse
// failure, a missing or wrong-typed field, a malformed or wrong-length
// key or signature, or a signature mismatch, is a stated refusal.
// Nothing here ever partially trusts a document or throws: a caller gets
// exactly one accept/refuse answer.
inline FallbackVerifyResult VerifyFallbackProgram(const std::string& signedProgramDocument,
                                                   const std::vector<uint8_t>& coordinatorPublicKey) {
    FallbackVerifyResult result;

    if (coordinatorPublicKey.size() != 32) {
        result.refusalReason = "fallback: coordinator public key is not 32 bytes";
        return result;
    }

    const showmesh::json::ParseResult parsed = showmesh::json::parse(signedProgramDocument);
    if (!parsed.ok) {
        result.refusalReason = "fallback: malformed signed program document: " + parsed.error;
        return result;
    }
    if (parsed.value.type() != showmesh::json::Type::kObject) {
        result.refusalReason = "fallback: signed program document is not a JSON object";
        return result;
    }

    const showmesh::json::Value* programValue = detail::findMember(parsed.value, "program");
    const showmesh::json::Value* signatureValue = detail::findMember(parsed.value, "signature");
    if (programValue == nullptr || signatureValue == nullptr) {
        result.refusalReason = "fallback: signed program document is missing program or signature";
        return result;
    }
    if (programValue->type() != showmesh::json::Type::kObject) {
        result.refusalReason = "fallback: program field is not a JSON object";
        return result;
    }
    if (signatureValue->type() != showmesh::json::Type::kString) {
        result.refusalReason = "fallback: signature field is not a string";
        return result;
    }

    std::vector<uint8_t> signatureBytes;
    if (!detail::base64Decode(signatureValue->string(), &signatureBytes)) {
        result.refusalReason = "fallback: signature is not valid base64";
        return result;
    }
    if (signatureBytes.size() != 64) {
        result.refusalReason = "fallback: signature is not 64 bytes";
        return result;
    }

    const showmesh::json::CanonicalResult canon = showmesh::json::canonicalize(*programValue);
    if (!canon.ok) {
        result.refusalReason = "fallback: cannot serialize program per RFC 8785 (JCS): " + canon.error;
        return result;
    }

    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, coordinatorPublicKey.data(),
                                                  coordinatorPublicKey.size());
    if (pkey == nullptr) {
        result.refusalReason = "fallback: could not build Ed25519 public key";
        return result;
    }

    bool verified = false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx != nullptr) {
        if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
            const int rc = EVP_DigestVerify(ctx, signatureBytes.data(), signatureBytes.size(),
                                             reinterpret_cast<const uint8_t*>(canon.text.data()), canon.text.size());
            verified = (rc == 1);
        }
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);

    if (!verified) {
        result.refusalReason = "fallback: signature does not verify";
        return result;
    }

    // A program that verifies but does not carry its own reported
    // identity is not something this function reports as accepted with
    // blank fields: the caller's report (ADR-048 section 1) needs both,
    // and a caller matching this program against a specific host or
    // clock needs fppInstanceUuid/expiresAt too.
    const showmesh::json::Value* packageIdValue = detail::findMember(*programValue, "packageId");
    const showmesh::json::Value* revisionValue = detail::findMember(*programValue, "revision");
    const showmesh::json::Value* fppInstanceUuidValue = detail::findMember(*programValue, "fppInstanceUuid");
    const showmesh::json::Value* expiresAtValue = detail::findMember(*programValue, "expiresAt");
    if (packageIdValue == nullptr || packageIdValue->type() != showmesh::json::Type::kString ||
        revisionValue == nullptr || revisionValue->type() != showmesh::json::Type::kString ||
        fppInstanceUuidValue == nullptr || fppInstanceUuidValue->type() != showmesh::json::Type::kString ||
        expiresAtValue == nullptr || expiresAtValue->type() != showmesh::json::Type::kString) {
        result.refusalReason = "fallback: verified program is missing packageId, revision, fppInstanceUuid, or expiresAt";
        return result;
    }

    result.accepted = true;
    result.program.emplace(VerifiedFallbackProgram(packageIdValue->string(), revisionValue->string(),
                                                     signedProgramDocument, fppInstanceUuidValue->string(),
                                                     expiresAtValue->string()));
    return result;
}

}  // namespace fallback
}  // namespace showmesh
