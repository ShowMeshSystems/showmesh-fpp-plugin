#pragma once

#include <memory>
#include <string>

#include "curl_http_transport.h"
#include "playlist_mismatch_notifier.h"
#include "showmesh/config_watcher.h"
#include "showmesh/coordinator_client.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/sequence_store.h"

// Assembling the plugin's sending half, identically for both FPP majors.
// Neither adapter resolves the coordinator URL or the credential itself:
// the resolution rules belong with the Go macro helper's, in one place,
// and an adapter that reimplemented them would be a second configuration
// path to keep in step.

namespace showmesh {
namespace adapter {

class CoordinatorDelivery {
 public:
    explicit CoordinatorDelivery(Clock clock)
        : stateDir_(resolveSequenceStateDir()),
          statusSink_(stateDir_),
          credentials_(),
          client_(&transport_, &credentials_, resolveBaseUrl(stateDir_, &configurationError_), clock, &statusSink_,
                  sleepMillis, RetryPolicy(), &mismatchNotifier_),
          // Constructed after client_: it reads config.json itself, at
          // construction, only to establish tick()'s baseline mtime/size
          // for the same file resolveBaseUrl() just read above; it never
          // re-applies that same snapshot to client_.
          configWatcher_(stateDir_, &client_) {
        // A host with no coordinator URL yet still loads the plugin and
        // still runs the show; every post then fails visibly in the local
        // status record rather than silently doing nothing.
        if (!configurationError_.empty()) client_.setConfigurationError(configurationError_);
    }

    CoordinatorClient* client() { return &client_; }
    // Contract section 2's config reload. Also PairingWorker's
    // CoordinatorUrlSource: see PairingDelivery, which is constructed
    // with a pointer into this same watcher so pairing's claim attempts
    // and the observation client never disagree about the current
    // coordinator URL.
    ConfigWatcher* configWatcher() { return &configWatcher_; }

 private:
    static std::string resolveBaseUrl(const std::string& stateDir, std::string* error) {
        CoordinatorUrlLoad load = loadCoordinatorBaseUrl(stateDir);
        if (!load.ok) {
            *error = load.error;
            return std::string();
        }
        return load.baseUrl;
    }

    std::string stateDir_;
    std::string configurationError_;
    CurlHttpTransport transport_;
    FileStatusSink statusSink_;
    FileCredentialSource credentials_;
    // Declared before client_: client_ holds this pointer from
    // construction onward, and member initialization follows declaration
    // order regardless of the initializer list's order.
    WarningHolderMismatchNotifier mismatchNotifier_;
    CoordinatorClient client_;
    // Declared after client_: its constructor holds a pointer into it.
    ConfigWatcher configWatcher_;
};

}  // namespace adapter
}  // namespace showmesh
