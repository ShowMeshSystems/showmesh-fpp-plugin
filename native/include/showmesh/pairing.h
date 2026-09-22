#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "showmesh/coordinator_config.h"
#include "showmesh/http_transport.h"
#include "showmesh/runtime.h"

// The plugin worker's half of pairing, wire contract section 1 (SM-694),
// mirrored under docs/upstream/showmesh/. Host neutral: it takes an
// HttpTransport and a clock, and knows nothing about which FPP major or
// web framework is running it. Nothing here invents a field, a file name,
// or a spelling the contract did not already fix; a disagreement with the
// coordinator is a blocker raised against both repositories, never a
// unilateral change here.

namespace showmesh {

extern const char* const kPairingRequestFilename;
extern const char* const kPairingCodeFilename;
extern const char* const kPairingStatusFilename;

// What the worker posts the secret to. Unauthenticated, on the same
// coordinator base URL the observation client uses.
extern const char* const kPairingClaimPath;

constexpr TimeMillis kPairingExpiryMillis = 600000;      // 10 minutes
constexpr TimeMillis kPairingClaimIntervalMillis = 3000;

extern const char* const kCrockfordAlphabet;

// Fills count bytes with cryptographically random data. The default
// implementation reads /dev/urandom; a test supplies a fixed-byte fake so
// code derivation and pairing transitions are exercised deterministically.
using RandomBytesFn = bool (*)(std::uint8_t* out, std::size_t count);
bool readRandomBytes(std::uint8_t* out, std::size_t count);

// 32 random bytes, lower-case hex encoded (64 characters). Empty when
// randomBytes fails to fill the buffer.
std::string generateSecretHex(RandomBytesFn randomBytes = readRandomBytes);

// Derives the 8-character pairing code from a 64-character lower-case hex
// secret: the first 8 Crockford base32 characters (alphabet
// "0123456789ABCDEFGHJKMNPQRSTVWXYZ", upper case) of SHA-256(secret
// bytes), formatted "XXXX-XXXX". Those first 8 symbols are fully
// determined by the digest's first 5 bytes (40 bits, exactly 8 groups of
// 5), so this never encodes the whole 32-byte digest. Returns an empty
// string when secretHex is not exactly 64 lower-case hex characters: a
// malformed secret is never guessed at. The coordinator derives the
// identical code from the identical bytes independently; see
// docs/upstream/showmesh/ for the frozen contract both sides implement
// from.
std::string deriveCrockfordPairingCode(const std::string& secretHex);

enum class PairingState { kIdle, kWaiting, kPaired, kExpired, kFailed };

const char* pairingStateWireValue(PairingState state);

struct PairingStatus {
    PairingState state = PairingState::kIdle;
    std::string code;
    std::string principalId;
    TimeMillis pairedAtMillis = 0;
    std::string lastError;
    TimeMillis updatedAtMillis = 0;
};

// Renders pairing-status.json's contents exactly as contract section 1
// fixes them.
std::string renderPairingStatus(const PairingStatus& status);

// PairingWorker owns the whole state machine: watching for
// pairing-request, generating the secret and code, polling the claim
// route, and installing the credential. tick() is what the resident
// worker loop calls once per pass; nothing here spawns a thread of its
// own.
class PairingWorker {
 public:
    // credentialDir is where the paired credential is written; stateDir is
    // where the three pairing files live. urlSource answers "the
    // coordinator base URL right now", shared with config reload (section
    // 2) so a claim attempt always targets whatever URL is currently
    // configured, never a URL cached from construction. Never null in
    // production; a null urlSource reads as "no coordinator URL
    // configured" for every attempt, which is what a caller with nothing
    // else to hand it wants.
    PairingWorker(std::string stateDir, std::string credentialDir, HttpTransport* transport,
                  CoordinatorUrlSource* urlSource, RandomBytesFn randomBytes = readRandomBytes);

    // What the resident worker loop calls once per pass: picks up a fresh
    // or repeated pairing-request (restarting pairing from any state),
    // ages out an expired wait, and, while waiting, attempts a claim at
    // most once every kPairingClaimIntervalMillis. Cheap enough on every
    // other call (one stat() for the request file) to need no gate of its
    // own beyond the claim interval.
    void tick(TimeMillis now);

    PairingStatus status() const;

 private:
    void checkPairingRequest(TimeMillis now);
    void startPairing(TimeMillis now);
    void attemptClaim(TimeMillis now);
    void writeStatusFile();
    // Forgets the in-memory secret and, when requested, deletes
    // pairing-code: called on every path off kWaiting (paired, expired,
    // failed), since a claim attempt is single use in every one of those
    // outcomes.
    void endPairing(bool deleteCodeFile);
    void setState(PairingState state, TimeMillis now, std::string lastError);

    std::string stateDir_;
    std::string credentialDir_;
    HttpTransport* transport_;
    CoordinatorUrlSource* urlSource_;
    RandomBytesFn randomBytes_;

    // Worker-thread only, like CoordinatorClient: tick() is called from
    // exactly one thread in production. mutex_ guards status_ anyway, at
    // negligible cost, so a test can read status() from outside that
    // thread without a documented precondition to violate.
    mutable std::mutex mutex_;
    PairingStatus status_;
    // The secret lives here, in memory only, and nowhere else: never
    // written to a file or a log line. Cleared (overwritten, then
    // cleared) by endPairing().
    std::string secretHex_;
    TimeMillis expiresAtMillis_ = 0;
    TimeMillis lastClaimAttemptMillis_ = 0;
    // Whether attemptClaim() has run since the current wait started. A
    // plain "lastClaimAttemptMillis_ == 0" sentinel cannot tell "never
    // attempted" apart from "attempted at epoch millis 0", which a test
    // clock starting near zero hits directly.
    bool everAttemptedClaim_ = false;
    // What writeStatusFile() last wrote, so a routine 3s poll that changed
    // nothing observable does not rewrite an identical file.
    std::string lastWrittenStatus_;
};

}  // namespace showmesh
