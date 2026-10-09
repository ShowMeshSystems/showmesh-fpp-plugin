#pragma once

// Loads the coordinator's Ed25519 public key that ADR-025 decision 3 requires
// be pinned on the node. Pairing writes the file (fallback_pinned_key_store.h);
// this loader only reads it and never contacts the coordinator, and an absent
// file is reported as absent, never replaced with a key of its own.
//
// ADR-025 decision 4's operative sentence is the reason this file stats
// before it reads: "The pinned key must not be writable by anything that
// compromising the agent alone would grant... An implementation that
// cannot meet the ownership requirement on its platform must report that
// it is unverified rather than verify against a key it could have
// written itself." A key file present but writable by fppd's own account
// is not a degraded state, it is the exact failure decision 4 exists to
// prevent: checksum-level protection presenting as signing. So a
// present-but-untrusted file is refused before its bytes are ever read,
// exactly as a missing file is, distinctly from both.
//
// The check covers the CONTAINING DIRECTORY as well as the file. A
// directory writable by fppd's own account lets that account (or
// whatever is running as it) delete the legitimate file out from under
// this loader regardless of the file's own mode, so a perfect file mode
// proves nothing if the directory around it is not equally locked down.
// Both must pass, or this reports unverified and refuses to load,
// exactly as it does for the file alone.
//
// This borrows the "stat before read" shape loadCoordinatorCredential
// uses (showmesh/coordinator_config.h), but not its exact-mode check,
// and the reason is what each file protects. The credential is a
// SECRET: being readable by anyone else is itself the harm, so an exact
// tight mode (0600) is correct there. A pinned key is PUBLIC: its
// confidentiality is worth nothing and its integrity is worth
// everything, and the only property that matters is that nobody who
// should not be able to replace it can. That is what checking the
// writability bits (owner root, no group or other write) expresses, and
// an exact-mode match does not: it would refuse a root-owned key mode
// 0400 or 0444, shapes that are MORE locked down than required, not
// less, and its remedy would have to tell an operator to loosen a
// permission bit inside the one code path that exists to catch a
// filesystem an attacker controls. No value of a required-mode constant
// fixes that; only checking the bits that actually matter does. So this
// file never names a required mode, on the directory or the file, and
// its error text never suggests making anything more permissive.

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "fallback_program_verifier.h"
#include "showmesh/atomic_write.h"

namespace showmesh {
namespace fallback {

// Four ways this refuses to hand back a key, plus success. Named and
// reported distinctly (never folded into one "unavailable", and never
// into kNoProgramInstalled: that name means the coordinator has nothing
// published for this host, which may be entirely correct, while every
// kind below means a person has to go fix something on this host):
//   kMissing            no file at the resolved path: this player has not
//                        paired since the coordinator began sending its key.
//   kOwnershipUntrusted  the file or its containing directory is not
//                        root-owned, or is group- or other-writable: the
//                        ADR-025 decision-4 failure, sends a person to
//                        fix filesystem ownership.
//   kMalformed           ownership and directory checks passed, but the
//                        content is not a well-formed key: sends a
//                        person to re-copy the file correctly.
//   kLoaded              success.
enum class PinnedKeyLoadStatus {
    kMissing,
    kOwnershipUntrusted,
    kMalformed,
    kLoaded,
};

// The enum value's own spelling, the identical "grep the name, find the
// code and the log" rule ActivationResolveKindName() states for the
// resolver's own vocabulary.
inline const char* PinnedKeyLoadStatusName(PinnedKeyLoadStatus status) {
    switch (status) {
        case PinnedKeyLoadStatus::kMissing:
            return "kMissing";
        case PinnedKeyLoadStatus::kOwnershipUntrusted:
            return "kOwnershipUntrusted";
        case PinnedKeyLoadStatus::kMalformed:
            return "kMalformed";
        case PinnedKeyLoadStatus::kLoaded:
            return "kLoaded";
    }
    return "kUnknown";
}

struct PinnedKeyLoadResult {
    PinnedKeyLoadStatus status = PinnedKeyLoadStatus::kMissing;
    // Operator-facing detail, populated for every status except kLoaded.
    // Never contains key material.
    std::string error;
    // Populated only for kLoaded: the raw 32-byte Ed25519 public key.
    std::vector<uint8_t> publicKey;
};

// The filename under the trust directory (showmesh::resolveTrustDir()).
// ADR-025 fixes the key's ownership, not its name; pairing writes this name.
inline const char* kPinnedCoordinatorPublicKeyFilename = "coordinator-fallback-public-key";

namespace detail {

// Owner must be root and neither group nor other may hold the write bit.
// Unlike the file's own check below, this is not an exact-mode match: a
// directory legitimately carries execute bits (0755 is ordinary for
// the trust directory) that would make an exact-match check refuse
// a perfectly safe directory, so only the two bits that actually matter
// (S_IWGRP, S_IWOTH) are checked.
inline bool statPassesOwnershipCheck(const struct ::stat& info) {
    if (info.st_uid != 0) return false;
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) return false;
    return true;
}

}  // namespace detail

// Resolves <trustDir>/kPinnedCoordinatorPublicKeyFilename, checks
// the containing directory's ownership and write protection, then the
// file's, then reads and decodes it. It checks the path it was asked to
// read, never a default it substitutes, so a caller that passes trustDir
// through a constructor (no environment variable) gets checks that apply
// to wherever it pointed.
//
// The file holds the key base64-encoded (RFC 4648, the same strict
// decoder fallback_program_verifier.h's signature check already uses),
// on one line, optionally trailing whitespace.
inline PinnedKeyLoadResult LoadPinnedCoordinatorPublicKey(const std::string& trustDir) {
    PinnedKeyLoadResult result;

    struct ::stat dirInfo {};
    if (::stat(trustDir.c_str(), &dirInfo) != 0) {
        result.status = PinnedKeyLoadStatus::kMissing;
        result.error = "pinned coordinator public key directory " + trustDir +
                       " does not exist; this player has not stored the coordinator's key yet";
        return result;
    }
    if (!S_ISDIR(dirInfo.st_mode)) {
        result.status = PinnedKeyLoadStatus::kOwnershipUntrusted;
        result.error = "pinned coordinator public key path " + trustDir + " is not a directory";
        return result;
    }
    if (!detail::statPassesOwnershipCheck(dirInfo)) {
        result.status = PinnedKeyLoadStatus::kOwnershipUntrusted;
        result.error = "pinned coordinator public key directory " + trustDir +
                       " is not root-owned and non-group/other-writable; refusing to trust a key whose "
                       "containing directory the agent's own account could rewrite";
        return result;
    }

    const std::string path = showmesh::joinPath(trustDir, kPinnedCoordinatorPublicKeyFilename);

    struct ::stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        result.status = PinnedKeyLoadStatus::kMissing;
        result.error = "pinned coordinator public key file " + path +
                       " does not exist; this player has not stored the coordinator's key yet";
        return result;
    }
    if (!S_ISREG(info.st_mode)) {
        result.status = PinnedKeyLoadStatus::kOwnershipUntrusted;
        result.error = "pinned coordinator public key path " + path + " is not a regular file";
        return result;
    }
    if (!detail::statPassesOwnershipCheck(info)) {
        result.status = PinnedKeyLoadStatus::kOwnershipUntrusted;
        result.error = "pinned coordinator public key file " + path +
                       " is not root-owned and non-group/other-writable; refusing to verify against a key "
                       "whose ownership the agent's own account could have produced. Fix this by correcting "
                       "who owns the file and removing group/other write access, never by making it more "
                       "permissive";
        return result;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.status = PinnedKeyLoadStatus::kMalformed;
        result.error = "pinned coordinator public key file " + path + " could not be opened for reading";
        return result;
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    std::string raw = contents.str();
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r' || raw.back() == ' ' || raw.back() == '\t')) {
        raw.pop_back();
    }

    std::vector<uint8_t> key;
    if (!showmesh::fallback::detail::base64Decode(raw, &key) || key.size() != 32) {
        result.status = PinnedKeyLoadStatus::kMalformed;
        result.error = "pinned coordinator public key file " + path +
                       " is not a well-formed base64-encoded 32-byte Ed25519 public key";
        return result;
    }

    result.status = PinnedKeyLoadStatus::kLoaded;
    result.publicKey = std::move(key);
    return result;
}

}  // namespace fallback
}  // namespace showmesh
