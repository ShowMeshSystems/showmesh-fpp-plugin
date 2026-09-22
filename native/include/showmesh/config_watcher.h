#pragma once

#include <mutex>
#include <string>

#include "showmesh/coordinator_client.h"
#include "showmesh/coordinator_config.h"

// The plugin worker's half of config reload, wire contract section 2
// (SM-695), mirrored under docs/upstream/showmesh/. Host neutral: POSIX
// stat only.

namespace showmesh {

// Re-reads <stateDir>/config.json when its mtime or size changes and
// applies a newly valid coordinatorUrl to the given CoordinatorClient
// without a restart. Also implements CoordinatorUrlSource, so
// PairingWorker's claim attempts always target the same URL the
// observation client is currently configured with; the two can never
// disagree because both read from this one instance.
class ConfigWatcher : public CoordinatorUrlSource {
 public:
    // client is never null. Reads config.json once here, at construction,
    // to establish the baseline mtime/size a caller's own earlier read
    // (CoordinatorDelivery's constructor calling loadCoordinatorBaseUrl)
    // already saw, so the first tick() does not immediately redo a load
    // that just happened.
    ConfigWatcher(std::string stateDir, CoordinatorClient* client);

    // What the resident worker loop calls once per pass, on the same tick
    // as PairingWorker::tick(). One stat() call when nothing changed;
    // config.json is re-read, and the result applied to client, only when
    // the file's mtime or size differs from what was last observed.
    void tick();

    std::string currentBaseUrl() const override;

 private:
    void reload();

    std::string stateDir_;
    CoordinatorClient* client_;
    bool everObserved_ = false;
    long long lastMtimeSeconds_ = 0;
    long long lastSizeBytes_ = -1;

    mutable std::mutex mutex_;
    std::string currentBaseUrl_;
};

}  // namespace showmesh
