#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

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

// The claim POST's own request timeout, both connect and total (the one
// knob HttpRequest exposes covers both; see http_transport.h). Short on
// purpose: this bounds how long a black-holing coordinator URL can hold a
// single attempt, which in turn bounds how long requestStop()'s join has
// to wait for whatever attempt is already in flight. Far below the 10
// minute pairing window, so it costs nothing against a normal coordinator.
constexpr int kPairingClaimTimeoutMillis = 3000;

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
    // Populated only in kWaiting; cleared in every other state, including
    // kIdle, so a stale code can never be read back out of a status this
    // worker itself wrote for a pairing that is no longer open.
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
// route, and installing the credential.
//
// tick() is the pure, synchronous state-machine step; a test calls it
// directly and never needs a real thread. start() spawns PairingWorker's
// own background thread, which calls tick() in a loop on its own clock
// reading -- deliberately never the resident worker's thread that runs
// ShowMeshRuntime's observation pipeline. attemptClaim()'s POST is a
// blocking network call (bounded by kPairingClaimTimeoutMillis, but still
// blocking for up to that long against a black-holing coordinator), and
// running it on the resident thread would starve CallbackHandoff (16
// slots) of the drainOnce() passes that keep it from silently coalescing
// observations. See ShowMeshRuntime::workerLoop(), which ticks
// ConfigWatcher (cheap, local-file-only) but not this.
class PairingWorker {
 public:
    // credentialDir is where the paired credential is written; stateDir is
    // where the three pairing files live. urlSource answers "the
    // coordinator base URL right now", shared with config reload (section
    // 2) so a claim attempt always targets whatever URL is currently
    // configured, never a URL cached from construction. Never null in
    // production; a null urlSource reads as "no coordinator URL
    // configured" for every attempt, which is what a caller with nothing
    // else to hand it wants. clock is required, like ShowMeshRuntime's own.
    //
    // Reconciles on-disk state synchronously, here, before returning: if
    // pairing-status.json says a pairing was left "waiting", the secret
    // that pairing needed lived only in the previous process's memory and
    // is gone now, so this pairing can never complete. It is reconciled to
    // expired and pairing-code is deleted, rather than left claiming to
    // wait for a secret that no longer exists. This is a local file read,
    // never a network call, so it is safe at construction the same way
    // BrightnessFileStore's and SequenceFileStore's own construction-time
    // reads already are.
    PairingWorker(std::string stateDir, std::string credentialDir, HttpTransport* transport,
                  CoordinatorUrlSource* urlSource, Clock clock, RandomBytesFn randomBytes = readRandomBytes);
    ~PairingWorker();

    // Starts the background thread. Safe to call more than once (a repeat
    // is a no-op), mirroring ShowMeshRuntime::start().
    void start();
    // Interrupts the background thread's wait and, when a claim attempt is
    // about to run or already in flight, tells it to give up as soon as
    // its own bounded request timeout returns rather than starting
    // another. Called before stop()'s join so an unload is never held
    // past one bounded claim attempt. Safe to call more than once, and
    // safe to call with no thread running.
    void requestStop();
    // requestStop() plus joining the background thread. Safe to call more
    // than once.
    void stop();

    // The pure state-machine step: picks up a fresh or repeated
    // pairing-request (restarting pairing from any state), ages out an
    // expired wait, and, while waiting, attempts a claim at most once
    // every kPairingClaimIntervalMillis. Exposed so a test can drive it
    // directly, synchronously, without starting a thread.
    void tick(TimeMillis now);

    PairingStatus status() const;

 private:
    void run();
    void reconcileStartupState();
    void checkPairingRequest(TimeMillis now);
    void startPairing(TimeMillis now);
    void attemptClaim(TimeMillis now);
    void writeStatusFile();
    // Forgets the in-memory secret and, when requested, deletes
    // pairing-code: called on every path off kWaiting (paired, expired,
    // failed), since a claim attempt is single use in every one of those
    // outcomes, and at the start of startPairing() so a pairing this call
    // is about to replace -- successfully or not -- never leaves the
    // previous one's file or secret behind.
    void endPairing(bool deleteCodeFile);
    // Sets state and lastError. Clears code whenever state is anything
    // but kWaiting: code is only ever meaningful while a pairing is open,
    // and every non-waiting state (idle, paired, expired, failed) must
    // never echo a code from a pairing that already ended.
    void setState(PairingState state, TimeMillis now, std::string lastError);

    std::string stateDir_;
    std::string credentialDir_;
    HttpTransport* transport_;
    CoordinatorUrlSource* urlSource_;
    Clock clock_;
    RandomBytesFn randomBytes_;

    // Touched only from run() (or directly from a test that never calls
    // start()): secretHex_, expiresAtMillis_, lastClaimAttemptMillis_ and
    // everAttemptedClaim_ are never read or written from any other
    // thread, so they need no lock of their own. status_ is guarded by
    // mutex_ because status() is a public, cross-thread accessor.
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

    std::thread thread_;
    // Whether start() has already spawned the thread; guards start()'s
    // own idempotency only.
    std::atomic<bool> started_{false};
    // Set only by requestStop(), never implied by "the thread was never
    // started": a test that drives tick() directly, synchronously, with
    // no thread running must keep attempting a claim exactly as before.
    // Checked both by run()'s loop and, inside tick(), before starting a
    // new claim attempt, so requestStop() does not have to wait out an
    // attempt already due to begin.
    std::atomic<bool> stopRequested_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
};

}  // namespace showmesh
