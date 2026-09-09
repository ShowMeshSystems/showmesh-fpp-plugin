#pragma once

// Wires the ADR-048 fallback path into the shipping plugin: fetches the
// signed fallback program exactly once, on the WORKER THREAD's first
// pass, never at construction (see performStartupFetch() below for why),
// and never again after that (when to refetch is an operator decision
// that has not been made, and this file does not make it by accident
// through a retry loop), then implements FallbackActivationRecorder so
// ShowMeshRuntime's worker can record what the installed program says
// about every entry it resolves.
//
// This is the recording half only. It never sends anything to a node:
// ADR-048 decision 3's node ingress and per-host executor credential do
// not exist in any repository this one can build against, so there is
// nothing to deliver to. Resolving and recording is the whole job.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "curl_http_transport.h"
#include "fallback_activation_resolver.h"
#include "fallback_pinned_key_loader.h"
#include "fallback_program_fetch.h"
#include "fallback_program_installer.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/runtime.h"
#include "showmesh/sequence_store.h"

#include "log.h"

namespace showmesh {
namespace adapter {

// ONE named constant, never a literal repeated at each of this file's
// log call sites: a repeated literal drifts, and grep then silently
// stops finding some call sites while still looking like it works.
inline const char* kFallbackActivationLogPrefix = "ShowMesh fallback activation: ";

// requestStop() is deliberately NOT overridden here: CurlHttpTransport has
// no mid-transfer cancellation today, so there is nothing this class could
// interrupt an in-flight call with. The worst case for a truly blocked
// fetch stays bounded by the transport's own CURLOPT_TIMEOUT_MS
// (HttpRequest::timeoutMillis, 10000ms by default here since neither
// request below overrides it), comfortably under FPP 10's 60 second
// shutdown deadline even though it is a bound rather than a genuine
// interruption. Giving CurlHttpTransport real cancellation is future work,
// not something this relocation needs to be safe.
class FallbackActivationDelivery : public showmesh::FallbackActivationRecorder {
 public:
    // fppInstanceUuid is read once, here, at construction (the identical
    // "fetch once at worker start" cadence this whole file follows): the
    // fetch route is keyed by it and the installed program's own signed
    // copy is checked against it once, at fetch time, by
    // FetchAndInstallFallbackProgram itself.
    //
    // credentialDir is a constructor parameter defaulting to
    // resolveCredentialDir(), the same shape FileCredentialSource already
    // uses (coordinator_config.h), deliberately NOT an environment
    // variable override the way resolveSequenceStateDir()'s
    // SHOWMESH_FPP_PLUGIN_CONFIG_DIR is. resolveCredentialDir() itself
    // has no override either (coordinator_config.cpp: it returns the
    // kCredentialDir constant outright), so this is not an inconsistency
    // with that directory, only with the unrelated state directory. The
    // asymmetry is deliberate: an environment override on the path to a
    // security anchor widens who can choose which file gets treated as
    // the pinned key to anything that can set fppd's process
    // environment, on the one file in this design whose entire job is to
    // be the thing an attacker cannot substitute, and it buys nothing a
    // constructor parameter does not already give, since enrollment,
    // tests, and any later wiring can already point this wherever it
    // needs to go, decided at wiring time rather than at runtime.
    // Construction only loads the pinned key: a local file stat and read,
    // never a network call, so it is safe to run on whatever thread
    // constructs the plugin (FPP's load path on both majors). The signed
    // program fetch itself does NOT happen here; see
    // performStartupFetch() below.
    FallbackActivationDelivery(showmesh::Clock clock, std::string fppInstanceUuid,
                                std::string credentialDir = showmesh::resolveCredentialDir())
        : clock_(clock),
          fppInstanceUuid_(std::move(fppInstanceUuid)),
          // Resolved into credentialDir above, then checked below exactly
          // where it points: never a check against the default while
          // reading from an override, because there is no override to
          // read from anywhere but this parameter.
          keyResult_(showmesh::fallback::LoadPinnedCoordinatorPublicKey(credentialDir)) {}

    // Called once by ShowMeshRuntime, on the worker thread, before its
    // first pass over drainOnce(): see
    // FallbackActivationRecorder::performStartupFetch()'s own doc
    // comment for why this used to run in the constructor and why that
    // was wrong. A host with no usable pinned key attempts nothing here,
    // ever, until a restart finds one (ADR-025 decision 7: a node with no
    // pinned key has no usable cache, and that is legitimate, visible,
    // and must not block the agent).
    void performStartupFetch() override {
        if (keyResult_.status != showmesh::fallback::PinnedKeyLoadStatus::kLoaded) {
            LogErr(VB_PLUGIN, "%sno usable fallback program at worker start: %s (%s)\n",
                   kFallbackActivationLogPrefix, showmesh::fallback::PinnedKeyLoadStatusName(keyResult_.status),
                   keyResult_.error.c_str());
            return;
        }

        const showmesh::CoordinatorUrlLoad urlLoad =
            showmesh::loadCoordinatorBaseUrl(showmesh::resolveSequenceStateDir());
        if (!urlLoad.ok) {
            LogErr(VB_PLUGIN, "%scould not resolve coordinator base URL for the one-time fetch: %s\n",
                   kFallbackActivationLogPrefix, urlLoad.error.c_str());
            return;
        }
        baseUrl_ = urlLoad.baseUrl;

        const showmesh::fallback::FallbackFetchOutcome outcome = showmesh::fallback::FetchAndInstallFallbackProgram(
            &transport_, &credentials_, baseUrl_, fppInstanceUuid_, keyResult_.publicKey, InstallPath(), clock_);
        LogInfo(VB_PLUGIN, "%sone-time fetch at worker start: %s: %s\n", kFallbackActivationLogPrefix,
                showmesh::fallback::FallbackFetchOutcomeKindName(outcome.kind), outcome.detail.c_str());
        if (showmesh::fallback::ShouldAcknowledgeFallbackFetchOutcome(outcome)) {
            const showmesh::fallback::AcknowledgeResult ack = showmesh::fallback::AcknowledgeFallbackProgram(
                &transport_, &credentials_, baseUrl_, fppInstanceUuid_, outcome, clock_);
            if (!ack.ok) {
                LogErr(VB_PLUGIN, "%sacknowledge failed: %s\n", kFallbackActivationLogPrefix, ack.error.c_str());
            }
        }
    }

    // The seven resolver outcome strings, and the pinned-key outcome
    // strings when there is no usable key, are each the enum
    // value's own spelling (ActivationResolveKindName(),
    // PinnedKeyLoadStatusName()): never renamed for readability here,
    // because that is the only link between this log line and the code
    // that produced it.
    void recordEntryKeyResolution(const std::string& entryKey, showmesh::TimeMillis observedAtMillis) override {
        if (keyResult_.status != showmesh::fallback::PinnedKeyLoadStatus::kLoaded) {
            // A missing or untrusted pinned key is never reported as
            // kNoProgramInstalled: that name means the coordinator has
            // nothing published for this host, which may be entirely
            // correct, while this means an enrollment gap or a tampered
            // key file, which a person has to fix. Folding the two
            // together would erase exactly the distinction recording
            // exists to carry, one level above where the resolver's own
            // seven outcomes already carry it.
            LogInfo(VB_PLUGIN, "%s%s entryKey=%s observedAtMillis=%lld\n", kFallbackActivationLogPrefix,
                    showmesh::fallback::PinnedKeyLoadStatusName(keyResult_.status), entryKey.c_str(),
                    static_cast<long long>(observedAtMillis));
            return;
        }

        const auto now = std::chrono::system_clock::time_point(std::chrono::milliseconds(observedAtMillis));
        const showmesh::fallback::ActivationResolution resolution =
            showmesh::fallback::ResolveInstalledActivation(entryKey, InstallPath(), keyResult_.publicKey, now);
        LogInfo(VB_PLUGIN, "%s%s entryKey=%s observedAtMillis=%lld\n", kFallbackActivationLogPrefix,
                showmesh::fallback::ActivationResolveKindName(resolution.kind), entryKey.c_str(),
                static_cast<long long>(observedAtMillis));
    }

 private:
    static std::string InstallPath() { return showmesh::fallback::DefaultFallbackProgramPath(); }

    showmesh::Clock clock_;
    std::string fppInstanceUuid_;
    showmesh::fallback::PinnedKeyLoadResult keyResult_;
    std::string baseUrl_;
    CurlHttpTransport transport_;
    showmesh::FileCredentialSource credentials_;
};

}  // namespace adapter
}  // namespace showmesh
