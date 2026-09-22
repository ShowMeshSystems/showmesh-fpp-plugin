#pragma once

#include <mutex>
#include <string>

#include "showmesh/coordinator_client.h"
#include "showmesh/coordinator_config.h"

// The plugin worker's half of config reload, wire contract section 2
// (SM-695), mirrored under docs/upstream/showmesh/. Host neutral: POSIX
// stat only.

namespace showmesh {

// A cheap fingerprint of config.json's on-disk state, compared field by
// field rather than folded into one hash so the nanosecond-mtime and
// content-hash signals each catch what the other misses: two rewrites
// within the same mtime tick (nanoseconds still normally differ, but not
// on every filesystem) are caught by the hash even if mtime does not
// move, and a rewrite whose new content happens to hash the same as
// something read moments before common config.json edits would never
// produce (an operator pasting a different URL) is still caught by mtime.
struct ConfigFileSnapshot {
    bool exists = false;
    long long mtimeNanos = 0;
    long long sizeBytes = -1;
    std::string contentHashHex;

    bool operator==(const ConfigFileSnapshot& other) const {
        return exists == other.exists && mtimeNanos == other.mtimeNanos && sizeBytes == other.sizeBytes &&
              contentHashHex == other.contentHashHex;
    }
    bool operator!=(const ConfigFileSnapshot& other) const { return !(*this == other); }
};

// Reads path's mtime (nanosecond resolution where the platform's struct
// stat carries one), size, and SHA-256 of its contents. A stat() failure
// (the file does not exist) reports exists=false and leaves the rest at
// their defaults. Exposed for its own test.
ConfigFileSnapshot snapshotConfigFile(const std::string& path);

// Re-reads <stateDir>/config.json when its snapshot changes and applies a
// newly valid coordinatorUrl to the given CoordinatorClient without a
// restart; a config.json that stops parsing puts the client into
// setUnconfigured() (baseUrl_ cleared, not just configured=false), so a
// stale URL can never keep receiving posts once config.json can no
// longer justify it. Also implements CoordinatorUrlSource, so
// PairingWorker's claim attempts always target the same URL the
// observation client is currently configured with; the two can never
// disagree because both read from this one instance.
class ConfigWatcher : public CoordinatorUrlSource {
 public:
    // client is never null. Reads config.json once here, at construction,
    // to establish the baseline snapshot a caller's own earlier read
    // (CoordinatorDelivery's constructor calling loadCoordinatorBaseUrl)
    // already saw, so the first tick() does not immediately redo a load
    // that just happened.
    ConfigWatcher(std::string stateDir, CoordinatorClient* client);

    // What the resident worker loop calls once per pass. One stat() plus
    // one small read (config.json is a handful of bytes) every call;
    // config.json is only re-parsed and the result only applied to
    // client when the file's snapshot differs from what was last
    // observed.
    void tick();

    std::string currentBaseUrl() const override;

 private:
    void reload();

    std::string stateDir_;
    CoordinatorClient* client_;
    bool everObserved_ = false;
    ConfigFileSnapshot lastSnapshot_;

    mutable std::mutex mutex_;
    std::string currentBaseUrl_;
};

}  // namespace showmesh
