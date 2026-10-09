#pragma once

#include <string>

#include "curl_http_transport.h"
#include "fallback_pinned_key_store.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/pairing.h"
#include "showmesh/sequence_store.h"

#include "log.h"

// Assembling the plugin's pairing half (contract section 1), identically
// for both FPP majors. urlSource is CoordinatorDelivery's own
// ConfigWatcher, never a second one: pairing and the observation client
// must never resolve a different coordinator URL from each other.

namespace showmesh {
namespace adapter {

class PairingDelivery {
 public:
    PairingDelivery(CoordinatorUrlSource* urlSource, Clock clock, std::string trustDir = resolveTrustDir())
        : worker_(resolveSequenceStateDir(), resolveCredentialDir(), &transport_, urlSource, clock, readRandomBytes,
                  storeCoordinatorKey(std::move(trustDir))) {}

    PairingWorker* worker() { return &worker_; }

 private:
    static CoordinatorKeySink storeCoordinatorKey(std::string trustDir) {
        return [trustDir](bool present, const std::string& keyBase64) {
            std::string error;
            if (present && fallback::StoreCoordinatorPublicKey(trustDir, keyBase64, &error)) return;
            LogErr(VB_PLUGIN, "%s\n", fallback::kCoordinatorKeyNotStoredMessage);
        };
    }

    CurlHttpTransport transport_;
    PairingWorker worker_;
};

}  // namespace adapter
}  // namespace showmesh
