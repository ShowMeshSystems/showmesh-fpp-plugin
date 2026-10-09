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
            const bool storedKeyRemains =
                fallback::LoadPinnedCoordinatorPublicKey(trustDir).status == fallback::PinnedKeyLoadStatus::kLoaded;
            if (!present) {
                if (storedKeyRemains) {
                    LogInfo(VB_PLUGIN, "The coordinator sent no key, so the key stored earlier stays in use.\n");
                } else {
                    LogErr(VB_PLUGIN,
                           "The coordinator sent no key, so fallback is not available. Update the coordinator and pair this player again.\n");
                }
                return;
            }
            LogErr(VB_PLUGIN, "The coordinator's key was not stored: %s. Fallback is not available until this player pairs again.\n",
                   error.c_str());
        };
    }

    CurlHttpTransport transport_;
    PairingWorker worker_;
};

}  // namespace adapter
}  // namespace showmesh
