#include "showmesh/config_watcher.h"

#include <sys/stat.h>

#include <fstream>
#include <sstream>

#include "showmesh/atomic_write.h"
#include "showmesh/sha256.h"

namespace showmesh {

namespace {
constexpr const char* kConfigFilename = "config.json";

// struct stat's nanosecond mtime field is spelled differently on BSD-
// derived platforms (macOS's st_mtimespec) than on Linux (the POSIX.1-2008
// st_mtim); this is the one place that difference is expressed, since the
// native core has to build (and its tests have to run) on either.
long long mtimeNanosOf(const struct ::stat& info) {
#if defined(__APPLE__)
    return static_cast<long long>(info.st_mtimespec.tv_sec) * 1000000000LL +
          static_cast<long long>(info.st_mtimespec.tv_nsec);
#else
    return static_cast<long long>(info.st_mtim.tv_sec) * 1000000000LL +
          static_cast<long long>(info.st_mtim.tv_nsec);
#endif
}

}  // namespace

ConfigFileSnapshot snapshotConfigFile(const std::string& path) {
    ConfigFileSnapshot snapshot;
    struct ::stat info {};
    if (::stat(path.c_str(), &info) != 0) return snapshot;
    snapshot.exists = true;
    snapshot.mtimeNanos = mtimeNanosOf(info);
    snapshot.sizeBytes = static_cast<long long>(info.st_size);

    // config.json is a handful of bytes; hashing it on every tick is
    // negligible next to the stat() call itself, and is what catches two
    // rewrites landing within one mtime tick (some filesystems only keep
    // whole-second resolution despite struct stat's nanosecond field)
    // with the same resulting size.
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::ostringstream contents;
        contents << in.rdbuf();
        snapshot.contentHashHex = sha256Hex(contents.str());
    }
    return snapshot;
}

ConfigWatcher::ConfigWatcher(std::string stateDir, CoordinatorClient* client)
    : stateDir_(std::move(stateDir)), client_(client) {
    lastSnapshot_ = snapshotConfigFile(joinPath(stateDir_, kConfigFilename));
    everObserved_ = lastSnapshot_.exists;
    // Established without applying anything: whatever the caller already
    // loaded from this same file (CoordinatorDelivery's own construction
    // read) is what client currently reflects, and currentBaseUrl_ below
    // is populated the identical way so PairingWorker sees the same value
    // from its very first tick, not an empty one until config.json next
    // changes.
    const CoordinatorUrlLoad load = loadCoordinatorBaseUrl(stateDir_);
    if (load.ok) {
        std::lock_guard<std::mutex> guard(mutex_);
        currentBaseUrl_ = load.baseUrl;
    }
}

void ConfigWatcher::tick() {
    const ConfigFileSnapshot snapshot = snapshotConfigFile(joinPath(stateDir_, kConfigFilename));
    if (everObserved_ && snapshot == lastSnapshot_) return;
    if (!snapshot.exists && !everObserved_) return;

    everObserved_ = snapshot.exists;
    lastSnapshot_ = snapshot;
    reload();
}

void ConfigWatcher::reload() {
    const CoordinatorUrlLoad load = loadCoordinatorBaseUrl(stateDir_);
    if (load.ok) {
        if (client_ != nullptr) client_->setBaseUrl(load.baseUrl);
        std::lock_guard<std::mutex> guard(mutex_);
        currentBaseUrl_ = load.baseUrl;
    } else {
        // Clears baseUrl_ too, not only the configured flag: see
        // setUnconfigured()'s own doc comment for the bug this closes.
        if (client_ != nullptr) client_->setUnconfigured(load.error);
        std::lock_guard<std::mutex> guard(mutex_);
        currentBaseUrl_.clear();
    }
}

std::string ConfigWatcher::currentBaseUrl() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return currentBaseUrl_;
}

}  // namespace showmesh
