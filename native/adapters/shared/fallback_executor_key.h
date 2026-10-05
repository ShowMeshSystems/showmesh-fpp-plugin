#pragma once

// The FPP host's fallback executor key (contract section 5.1 and 5.2): an
// Ed25519 key pair created once a pairing token exists, stored beside that
// token, and registered with the coordinator. The private key never leaves here.

#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "fallback_program_verifier.h"
#include "showmesh/atomic_write.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/http_transport.h"
#include "showmesh/json.h"
#include "showmesh/pairing.h"

namespace showmesh {
namespace fallback {

constexpr const char* kExecutorKeyFilename = "fallback-executor-key";
constexpr ::mode_t kExecutorKeyFileMode = 0600;
constexpr std::size_t kEd25519SeedBytes = 32;
constexpr std::size_t kEd25519PublicKeyBytes = 32;
constexpr std::size_t kEd25519SignatureBytes = 64;

// RFC 4648 standard base64 with padding, the encoding section 5.1 fixes.
inline std::string base64Encode(const uint8_t* data, std::size_t size) {
    const char* const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
        const uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < size ? kAlphabet[triple & 0x3F] : '=');
    }
    return out;
}

inline bool hexDecode(const std::string& hex, std::vector<uint8_t>* out) {
    if (hex.size() % 2 != 0) return false;
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    out->clear();
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

inline std::string hexEncode(const std::vector<uint8_t>& bytes) {
    const char* const kHex = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

struct ExecutorKey {
    std::vector<uint8_t> seed;
    std::string publicKeyBase64;
};

// Fills key from a 32 byte Ed25519 seed. False when the seed is not one.
inline bool executorKeyFromSeed(const std::vector<uint8_t>& seed, ExecutorKey* key) {
    if (seed.size() != kEd25519SeedBytes) return false;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    if (pkey == nullptr) return false;
    uint8_t publicKey[kEd25519PublicKeyBytes];
    std::size_t publicKeySize = sizeof(publicKey);
    const bool ok = EVP_PKEY_get_raw_public_key(pkey, publicKey, &publicKeySize) == 1 &&
                    publicKeySize == kEd25519PublicKeyBytes;
    EVP_PKEY_free(pkey);
    if (!ok) return false;
    key->seed = seed;
    key->publicKeyBase64 = base64Encode(publicKey, publicKeySize);
    return true;
}

// Pure Ed25519 over message, no prehash and no context. The signature is base64.
inline bool signWithExecutorKey(const ExecutorKey& key, const std::string& message, std::string* signatureBase64) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key.seed.data(), key.seed.size());
    if (pkey == nullptr) return false;
    bool ok = false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx != nullptr) {
        uint8_t signature[kEd25519SignatureBytes];
        std::size_t signatureSize = sizeof(signature);
        if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
            EVP_DigestSign(ctx, signature, &signatureSize, reinterpret_cast<const uint8_t*>(message.data()),
                           message.size()) == 1 &&
            signatureSize == kEd25519SignatureBytes) {
            *signatureBase64 = base64Encode(signature, signatureSize);
            ok = true;
        }
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);
    return ok;
}

enum class ExecutorKeyStatus {
    kLoaded,
    kCreated,
    // No pairing token yet, so no key pair is created.
    kNotPaired,
    // A key file exists and cannot be used, or a new one could not be written.
    kUnusable,
};

struct ExecutorKeyResult {
    ExecutorKeyStatus status = ExecutorKeyStatus::kNotPaired;
    ExecutorKey key;
    std::string detail;
    bool usable() const { return status == ExecutorKeyStatus::kLoaded || status == ExecutorKeyStatus::kCreated; }
};

namespace detail {

inline bool writeExecutorKeyFile(const std::string& path, const std::string& contents) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, kExecutorKeyFileMode);
    if (fd < 0) return false;
    bool ok = ::fchmod(fd, kExecutorKeyFileMode) == 0;
    std::size_t written = 0;
    while (ok && written < contents.size()) {
        const ::ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
        ok = n > 0;
        if (ok) written += static_cast<std::size_t>(n);
    }
    if (ok) ok = ::fsync(fd) == 0;
    ::close(fd);
    if (ok) ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) std::remove(tmp.c_str());
    return ok;
}

}  // namespace detail

// Loads the key pair beside the pairing token, creating it only when a token
// exists and no key file does. An existing file is never replaced.
inline ExecutorKeyResult LoadOrCreateExecutorKey(const std::string& credentialDir,
                                                  showmesh::RandomBytesFn randomBytes = showmesh::readRandomBytes) {
    ExecutorKeyResult result;
    const std::string path = showmesh::joinPath(credentialDir, kExecutorKeyFilename);

    struct ::stat info {};
    if (::stat(path.c_str(), &info) == 0) {
        result.status = ExecutorKeyStatus::kUnusable;
        std::string raw;
        std::vector<uint8_t> seed;
        if (!S_ISREG(info.st_mode) || (info.st_mode & 07777) != kExecutorKeyFileMode) {
            result.detail = "executor key file " + path + " is not a regular file with mode 0600";
        } else if (!showmesh::readFileWhole(path, &raw) || !hexDecode(raw, &seed) ||
                   !executorKeyFromSeed(seed, &result.key)) {
            result.detail = "executor key file " + path + " does not hold a key";
        } else {
            result.status = ExecutorKeyStatus::kLoaded;
        }
        return result;
    }

    if (!showmesh::loadCoordinatorCredential(credentialDir).ok) {
        result.detail = "no pairing token yet";
        return result;
    }

    std::vector<uint8_t> seed(kEd25519SeedBytes);
    result.status = ExecutorKeyStatus::kUnusable;
    if (randomBytes == nullptr || !randomBytes(seed.data(), seed.size()) || !executorKeyFromSeed(seed, &result.key)) {
        result.detail = "could not generate an executor key";
        return result;
    }
    if (!detail::writeExecutorKeyFile(path, hexEncode(seed))) {
        result.detail = "could not write executor key file " + path;
        result.key = ExecutorKey();
        return result;
    }
    result.status = ExecutorKeyStatus::kCreated;
    return result;
}

enum class ExecutorRegistrationKind {
    kRegistered,
    kCredentialUnavailable,
    kUnreachable,
    // Any answer other than 200. The caller stays unregistered and asks again later.
    kRefused,
};

struct ExecutorRegistration {
    ExecutorRegistrationKind kind = ExecutorRegistrationKind::kUnreachable;
    int statusCode = 0;
    // True when this call stored a first key or replaced a different one.
    bool changed = false;
    // For kRefused, the coordinator's own reason text, as given.
    std::string detail;
};

// PUT /api/v1/fallback-programs/{fppInstanceId}/executor-key, section 5.2. One attempt.
inline ExecutorRegistration RegisterExecutorKey(HttpTransport* transport, CredentialSource* credentials,
                                                 const std::string& baseUrl, const std::string& fppInstanceUuid,
                                                 const std::string& publicKeyBase64, int timeoutMillis) {
    ExecutorRegistration result;
    std::string token;
    std::string credentialError;
    if (credentials == nullptr || !credentials->token(&token, &credentialError)) {
        result.kind = ExecutorRegistrationKind::kCredentialUnavailable;
        result.detail = credentialError;
        return result;
    }

    const showmesh::json::CanonicalResult body = showmesh::json::canonicalize(
        showmesh::json::Value::makeObject({{"publicKey", showmesh::json::Value::makeString(publicKeyBase64)}}));
    HttpRequest request;
    request.url = joinUrlPath(baseUrl, "/api/v1/fallback-programs/" + fppInstanceUuid + "/executor-key");
    request.body = body.text;
    request.bearerToken = token;
    request.timeoutMillis = timeoutMillis;

    const HttpResponse response = transport->put(request);
    if (!response.transportOk) {
        result.detail = response.error;
        return result;
    }
    result.statusCode = response.statusCode;
    const showmesh::json::ParseResult parsed = showmesh::json::parse(response.body);
    const bool haveObject = parsed.ok && parsed.value.type() == showmesh::json::Type::kObject;
    if (response.statusCode != 200) {
        result.kind = ExecutorRegistrationKind::kRefused;
        for (const char* name : {"detail", "title"}) {
            const showmesh::json::Value* text = haveObject ? detail::findMember(parsed.value, name) : nullptr;
            if (result.detail.empty() && text != nullptr && text->type() == showmesh::json::Type::kString) {
                result.detail = text->string();
            }
        }
        return result;
    }
    result.kind = ExecutorRegistrationKind::kRegistered;
    const showmesh::json::Value* changed = haveObject ? detail::findMember(parsed.value, "changed") : nullptr;
    result.changed = changed != nullptr && changed->type() == showmesh::json::Type::kBool && changed->boolean();
    return result;
}

}  // namespace fallback
}  // namespace showmesh
