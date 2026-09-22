#pragma once

#include <string>

#include "curl_http_transport.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/pairing.h"
#include "showmesh/sequence_store.h"

// Assembling the plugin's pairing half (contract section 1), identically
// for both FPP majors. urlSource is CoordinatorDelivery's own
// ConfigWatcher, never a second one: pairing and the observation client
// must never resolve a different coordinator URL from each other.

namespace showmesh {
namespace adapter {

class PairingDelivery {
 public:
    explicit PairingDelivery(CoordinatorUrlSource* urlSource)
        : worker_(resolveSequenceStateDir(), resolveCredentialDir(), &transport_, urlSource) {}

    PairingWorker* worker() { return &worker_; }

 private:
    CurlHttpTransport transport_;
    PairingWorker worker_;
};

}  // namespace adapter
}  // namespace showmesh
