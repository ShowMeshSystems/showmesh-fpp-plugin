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
            if (!present) {
                LogErr(VB_PLUGIN,
                       "The coordinator sent no key. Fallback keeps using a key this player already has. If it has none, update the coordinator and pair this player again.\n");
                return;
            }
            std::string error;
            fallback::PinnedKeyLoadStatus readBack = fallback::PinnedKeyLoadStatus::kLoaded;
            if (fallback::StoreCoordinatorPublicKey(trustDir, keyBase64, &error, &readBack)) return;
            if (readBack == fallback::PinnedKeyLoadStatus::kLoaded) {
                LogErr(VB_PLUGIN,
                       "The coordinator's key could not be saved: %s. Fallback keeps using a key this player already has. If it has none, fix this and pair this player again.\n",
                       error.c_str());
                return;
            }
            const char* reason = readBack == fallback::PinnedKeyLoadStatus::kOwnershipUntrusted
                                     ? "the key directory is not owned by root or can be written by other accounts"
                                     : "the saved key could not be read back correctly";
            LogErr(VB_PLUGIN,
                   "The coordinator's key was saved but cannot be trusted: %s. Fallback keeps using a key this player already has. If it has none, fix this and pair this player again.\n",
                   reason);
        };
    }

    CurlHttpTransport transport_;
    PairingWorker worker_;
};

}  // namespace adapter
}  // namespace showmesh
