#pragma once

// The only writer of the pinned coordinator key. Pairing calls it with the key
// the coordinator's claim answer carried; nothing else writes or fetches it.

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fallback_pinned_key_loader.h"

namespace showmesh {
namespace fallback {

inline const char* kCoordinatorKeyNotStoredMessage =
    "The coordinator's key was not stored, so fallback is not available until this player pairs again.";

namespace detail {

inline bool writeAll(int fd, const std::string& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// Opens trustDir without following a symlink at that path, creating it 0755 when missing.
inline int openTrustDirectory(const std::string& trustDir, std::string* error) {
    const int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int fd = ::open(trustDir.c_str(), flags);
    if (fd < 0 && errno == ENOENT) {
        if (::mkdir(trustDir.c_str(), 0755) != 0 && errno != EEXIST) {
            *error = "could not create " + trustDir + ": " + std::strerror(errno);
            return -1;
        }
        fd = ::open(trustDir.c_str(), flags);
        if (fd >= 0) ::fchmod(fd, 0755);
    }
    if (fd < 0) *error = "could not open " + trustDir + " as a directory: " + std::strerror(errno);
    return fd;
}

}  // namespace detail

// Stores keyBase64 (the base64 of a raw 32-byte Ed25519 public key, exactly as
// the coordinator sent it) under trustDir and loads it back with the loader.
// On any failure the previous stored key is left as it was.
inline bool StoreCoordinatorPublicKey(const std::string& trustDir, const std::string& keyBase64,
                                      std::string* error) {
    std::vector<uint8_t> key;
    if (!detail::base64Decode(keyBase64, &key) || key.size() != 32) {
        *error = "the key is not a base64-encoded 32-byte Ed25519 public key";
        return false;
    }
    if (::geteuid() != 0) {
        *error = "the plugin is not running as root";
        return false;
    }

    const int dirFd = detail::openTrustDirectory(trustDir, error);
    if (dirFd < 0) return false;

    const std::string tempName = std::string(".") + kPinnedCoordinatorPublicKeyFilename + ".tmp";
    ::unlinkat(dirFd, tempName.c_str(), 0);
    const int fd = ::openat(dirFd, tempName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        *error = "could not create a temporary file in " + trustDir + ": " + std::strerror(errno);
        ::close(dirFd);
        return false;
    }
    const bool written = ::fchmod(fd, 0644) == 0 && detail::writeAll(fd, keyBase64 + "\n") && ::fsync(fd) == 0;
    const int writeErrno = errno;
    ::close(fd);
    if (!written || ::renameat(dirFd, tempName.c_str(), dirFd, kPinnedCoordinatorPublicKeyFilename) != 0) {
        *error = std::string("could not write the key file: ") + std::strerror(written ? errno : writeErrno);
        ::unlinkat(dirFd, tempName.c_str(), 0);
        ::close(dirFd);
        return false;
    }
    ::fsync(dirFd);
    ::close(dirFd);

    const PinnedKeyLoadResult loaded = LoadPinnedCoordinatorPublicKey(trustDir);
    if (loaded.status != PinnedKeyLoadStatus::kLoaded || loaded.publicKey != key) {
        *error = loaded.status != PinnedKeyLoadStatus::kLoaded ? loaded.error : "the stored key did not read back the same";
        return false;
    }
    return true;
}

}  // namespace fallback
}  // namespace showmesh
