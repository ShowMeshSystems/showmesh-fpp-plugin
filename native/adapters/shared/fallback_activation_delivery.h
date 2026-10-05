#pragma once

// Wires the ADR-048 fallback executor (fallback_executor.h) into the shipping
// plugin, identically for both FPP majors: the real transport, FPP's log and
// warning list, the plugin's own directories, and the outage detector settings.

#include <string>

#include "Warnings.h"
#include "curl_http_transport.h"
#include "fallback_executor.h"
#include "fallback_pinned_key_loader.h"
#include "fallback_program_installer.h"
#include "settings.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/runtime.h"
#include "showmesh/sequence_store.h"

#include "log.h"

namespace showmesh {
namespace adapter {

// ONE named constant, never a literal repeated at each log call site.
inline const char* kFallbackActivationLogPrefix = "ShowMesh fallback activation: ";

// The same WarningHolder identity scheme as ShowMesh_PlaylistMismatch.
constexpr int ShowMesh_FallbackState = 0;

class WarningHolderFallbackStateNotifier : public showmesh::fallback::FallbackStateNotifier {
 public:
    void raise(const std::string& message) override {
        WarningHolder::AddWarningTimeout(-1, ShowMesh_FallbackState, message, {}, kPluginName);
    }

    void clear(const std::string& message) override {
        WarningHolder::RemoveWarning(ShowMesh_FallbackState, message, kPluginName);
    }
};

// A setting that is absent keeps its hypothesis default. One that is present
// and unusable is logged, because an operator did not get what they asked for.
inline int resolveOutageSetting(const char* name, int minimum, int maximum, int fallbackValue) {
    const std::string setting = getSetting(name);
    if (setting.empty()) return fallbackValue;
    int value = fallbackValue;
    if (showmesh::fallback::parseOutageSetting(setting, minimum, maximum, &value)) return value;
    LogErr(VB_PLUGIN, "%s setting value is not a whole number between %d and %d; using %d instead\n", name, minimum,
           maximum, fallbackValue);
    return fallbackValue;
}

inline showmesh::fallback::OutageDetectorConfig resolveOutageDetectorConfig() {
    using namespace showmesh::fallback;
    OutageDetectorConfig config;
    config.probeIntervalMillis = resolveOutageSetting(kProbeIntervalSettingName, 1000, 600000, config.probeIntervalMillis);
    config.probeTimeoutMillis = resolveOutageSetting(kProbeTimeoutSettingName, 250, 10000, config.probeTimeoutMillis);
    config.failedProbesToConfirm = resolveOutageSetting(kFailedProbesSettingName, 2, 100, config.failedProbesToConfirm);
    config.minimumLossMillis = resolveOutageSetting(kMinimumLossSettingName, 0, 3600000, config.minimumLossMillis);
    return config;
}

class FallbackActivationDelivery {
 public:
    // Construction reads local files and settings only, never the network, so
    // it is safe on FPP's plugin load path. credentialDir has no environment
    // override: it holds the pinned key, the pairing token and the executor key.
    FallbackActivationDelivery(showmesh::Clock clock, std::string fppInstanceUuid,
                               std::string credentialDir = showmesh::resolveCredentialDir())
        : executor_(makeOptions(clock, std::move(fppInstanceUuid), std::move(credentialDir), &notifier_),
                    &transport_) {}

    // What ShowMeshRuntime drives: every drained playlist callback, and the
    // start and stop of the executor's own background thread.
    showmesh::FallbackActivationRecorder* recorder() { return &executor_; }

 private:
    static showmesh::fallback::FallbackExecutorOptions makeOptions(showmesh::Clock clock, std::string fppInstanceUuid,
                                                                    std::string credentialDir,
                                                                    showmesh::fallback::FallbackStateNotifier* notifier) {
        showmesh::fallback::FallbackExecutorOptions options;
        options.clock = clock;
        options.fppInstanceUuid = std::move(fppInstanceUuid);
        options.stateDir = showmesh::resolveSequenceStateDir();
        options.pinnedKey = showmesh::fallback::LoadPinnedCoordinatorPublicKey(credentialDir);
        options.credentialDir = std::move(credentialDir);
        options.installPath = showmesh::fallback::DefaultFallbackProgramPath();
        options.detector = resolveOutageDetectorConfig();
        options.programRefetchIntervalMillis =
            resolveOutageSetting(showmesh::fallback::kProgramRefetchIntervalSettingName, 5000, 3600000,
                                 showmesh::fallback::kHypothesisProgramRefetchIntervalMillis);
        options.notifier = notifier;
        options.log = [](bool isError, const std::string& line) {
            if (isError) {
                LogErr(VB_PLUGIN, "%s%s\n", kFallbackActivationLogPrefix, line.c_str());
            } else {
                LogInfo(VB_PLUGIN, "%s%s\n", kFallbackActivationLogPrefix, line.c_str());
            }
        };
        return options;
    }

    // Declared before executor_, which holds pointers to both from construction.
    CurlHttpTransport transport_;
    WarningHolderFallbackStateNotifier notifier_;
    showmesh::fallback::FallbackExecutor executor_;
};

}  // namespace adapter
}  // namespace showmesh
