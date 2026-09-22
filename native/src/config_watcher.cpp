#include "showmesh/config_watcher.h"

#include <sys/stat.h>

#include "showmesh/atomic_write.h"

namespace showmesh {

namespace {
constexpr const char* kConfigFilename = "config.json";
}  // namespace

ConfigWatcher::ConfigWatcher(std::string stateDir, CoordinatorClient* client)
    : stateDir_(std::move(stateDir)), client_(client) {
    struct ::stat info {};
    const std::string path = joinPath(stateDir_, kConfigFilename);
    if (::stat(path.c_str(), &info) == 0) {
        everObserved_ = true;
        lastMtimeSeconds_ = static_cast<long long>(info.st_mtime);
        lastSizeBytes_ = static_cast<long long>(info.st_size);
    }
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
    struct ::stat info {};
    const std::string path = joinPath(stateDir_, kConfigFilename);
    const bool exists = ::stat(path.c_str(), &info) == 0;
    const long long mtimeSeconds = exists ? static_cast<long long>(info.st_mtime) : 0;
    const long long sizeBytes = exists ? static_cast<long long>(info.st_size) : -1;

    if (exists && everObserved_ && mtimeSeconds == lastMtimeSeconds_ && sizeBytes == lastSizeBytes_) return;
    if (!exists && !everObserved_) return;

    everObserved_ = exists;
    lastMtimeSeconds_ = mtimeSeconds;
    lastSizeBytes_ = sizeBytes;
    reload();
}

void ConfigWatcher::reload() {
    const CoordinatorUrlLoad load = loadCoordinatorBaseUrl(stateDir_);
    if (load.ok) {
        if (client_ != nullptr) client_->setBaseUrl(load.baseUrl);
        std::lock_guard<std::mutex> guard(mutex_);
        currentBaseUrl_ = load.baseUrl;
    } else {
        if (client_ != nullptr) client_->setConfigurationError(load.error);
        std::lock_guard<std::mutex> guard(mutex_);
        currentBaseUrl_.clear();
    }
}

std::string ConfigWatcher::currentBaseUrl() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return currentBaseUrl_;
}

}  // namespace showmesh
