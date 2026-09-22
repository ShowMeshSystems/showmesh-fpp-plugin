// The FPP 9 adapter. It owns nothing but FPP's lifecycle: every decision
// about brightness, identity, and delivery lives in the host-neutral
// runtime this file forwards into. It is compiled separately from the FPP
// 10 adapter and the two never appear in one binary, because FPP 10
// changes the plugin ABI, removes the libhttpserver registration
// surface, and adds a shutdown virtual.

// FPP 9 expects a plugin to include its precompiled-header umbrella
// first: it sets _FILE_OFFSET_BITS before anything else and decides
// HTTP_RESPONSE_CONST from the libhttpserver version installed, which
// Commands.h then uses in its own declarations.
#include "fpp-pch.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "Plugin.h"
#include "Plugins.h"
#include "Sequence.h"
#include "commands/Commands.h"

#include "brightness_command.h"
#include "callback_fields.h"
#include "channel_ranges.h"
#include "coordinator_delivery.h"
#include "fallback_activation_delivery.h"
#include "fpp_definition_source.h"
#include "pairing_delivery.h"
#include "safe_ceiling.h"
#include "section_names.h"
#include "showmesh/definition_republish.h"
#include "showmesh/transition_gain.h"
#include "showmesh/brightness_query.h"
#include "showmesh/brightness_store.h"
#include "showmesh/runtime.h"

namespace {

showmesh::TimeMillis nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Reports what the constructor found on disk for brightness state, once,
// at startup. A silent settle to the safe ceiling is worse than an
// unexplained dim rig: it sends an operator debugging the wrong subsystem
// on show night. See BrightnessEngine::settleSafeAfterUntrustedRestart
// and ShowMeshRuntime::brightnessRestartTrust(). safeCeilingPercent is
// the value the settle actually used, so the log line names it rather
// than a stale literal.
void logBrightnessRestartTrust(showmesh::BrightnessRestartTrust trust, int safeCeilingPercent) {
    switch (trust) {
        case showmesh::BrightnessRestartTrust::kTrustedOrNoRecord:
            return;
        case showmesh::BrightnessRestartTrust::kPrimaryUnreadableBackupRecovered:
            LogErr(VB_PLUGIN,
                   "ShowMesh: the primary brightness record could not be read; only a superseded backup "
                   "was found. Refusing to trust it, this plugin has deliberately settled at the "
                   "configured safe ceiling of %d%% rather than risk restoring brighter than what was "
                   "actually applied. This clears on the next ShowMesh brightness command or an adopted "
                   "MultiSync full state.\n",
                   safeCeilingPercent);
            return;
        case showmesh::BrightnessRestartTrust::kNeitherRecordReadable:
            LogErr(VB_PLUGIN,
                   "ShowMesh: neither the primary nor the backup brightness record could be read. This "
                   "plugin has deliberately settled at the configured safe ceiling of %d%% rather than "
                   "guess what was last applied. This clears on the next ShowMesh brightness command or "
                   "an adopted MultiSync full state.\n",
                   safeCeilingPercent);
            return;
    }
}

class ShowMeshFpp9Plugin : public FPPPlugin {
 public:
    ShowMeshFpp9Plugin()
        : FPPPlugin(showmesh::kPluginName),
          sequenceStore_(showmesh::resolveSequenceStateDir()),
          brightnessStore_(showmesh::resolveSequenceStateDir()),
          delivery_(nowMillis),
          fallbackDelivery_(nowMillis, definitions_.instanceUuid()),
          // Shares delivery_'s ConfigWatcher as its coordinator URL
          // source: see pairing_delivery.h.
          pairingDelivery_(delivery_.configWatcher(), nowMillis),
          safeCeilingPercent_(showmesh::adapter::resolveSafeCeilingPercent()),
          runtime_(&definitions_, delivery_.client(), nowMillis, &sequenceStore_, delivery_.client(),
                  &brightnessStore_, safeCeilingPercent_, &fallbackDelivery_, pairingDelivery_.worker(),
                  delivery_.configWatcher()) {
        logBrightnessRestartTrust(runtime_.brightnessRestartTrust(), safeCeilingPercent_);
        command_ = new showmesh::adapter::SetBrightnessCeilingCommand(&runtime_);
        CommandManager::INSTANCE.addCommand(command_);
        showmesh::adapter::configureChannelRanges(&*runtime_.brightness(), FPPD_MAX_CHANNELS);
        // start() before registering the settings listener: start() spawns
        // the worker thread and can throw, and a throwing constructor never
        // runs this object's destructor, so a listener registered first
        // would leave the global settings registry holding a callback that
        // captures a freed this.
        runtime_.setBrightnessFlushFailureHandler([] {
            LogErr(VB_PLUGIN,
                   "ShowMesh brightness state flush failed; the darker-safe restart guarantee may not hold\n");
        });
        runtime_.start();
        registerSettingsListener(showmesh::kPluginName, showmesh::adapter::kChannelRangesSettingName,
                                  [this](const std::string&) {
                                      showmesh::adapter::configureChannelRanges(&*runtime_.brightness(),
                                                                                FPPD_MAX_CHANNELS);
                                  });
    }

    ~ShowMeshFpp9Plugin() override {
        // PluginManager::INSTANCE is itself a static, and its destructor
        // calls Cleanup(), which deletes this. If fppd exits by a path that
        // skips the explicit Cleanup() call in fppd.cpp, that delete happens
        // during static destruction instead, at which point settings.cpp's
        // own SettingsConfig static may already be destroyed: locking its
        // destroyed mutex throws out of this noexcept destructor and calls
        // std::terminate. Nothing this destructor can check tells it which
        // case it is in, so the settings-registry call is guarded rather
        // than assumed safe.
        try {
            unregisterSettingsListener(showmesh::kPluginName, showmesh::adapter::kChannelRangesSettingName);
        } catch (...) {
        }
        runtime_.stop();
        // FPP 9 has no runtime reload, so this only ever runs at process
        // exit: the only restart this guards is a full fppd restart or
        // reboot, but the guarantee is the same one the FPP 10 adapter's
        // unload path makes, and every accepted observation is already
        // durable on its own (drainOnce() persists its sequence number
        // immediately), so a missed flush here is not a data-loss risk --
        // only a lost "extra guarantee" on whatever changed since the last
        // one. Both are no-ops that cannot throw when unconfigured or
        // already durable.
        runtime_.flushSequenceState();
        runtime_.flushBrightnessState();
        // Withdrawn by NAME, and never deleted, because on FPP 9 this
        // destructor runs after CommandManager has already deleted every
        // registered command: fppd.cpp calls CommandManager::Cleanup()
        // immediately before PluginManager::Cleanup(), and that Cleanup()
        // deletes what it holds. Reaching through command_ to unregister
        // would read a freed object's name, and deleting it would free it
        // twice. Removing by name is a no-op in that ordering and still
        // withdraws the registration in the reverse one, where this object
        // would otherwise be left holding a vtable in a library about to be
        // unmapped. The command is deliberately not deleted in either case:
        // ownership passed to CommandManager at addCommand(), and the only
        // path that reaches here is process shutdown.
        command_ = nullptr;
        CommandManager::INSTANCE.removeCommand(showmesh::kBrightnessCommandName);
    }

    // FPP's callback thread. Copy the bounded evidence and return: no
    // definition fetch, no hash, no persistence, no network, no sleep.
    void playlistCallback(const Json::Value& playlist, const std::string& action, const std::string& section,
                          int item) override {
        const std::string name = showmesh::adapter::playlistNameOf(playlist);
        const std::string canonicalSection = showmesh::adapter::canonicalPlaylistSection(section);
        const std::string sequenceFilename = showmesh::adapter::sequenceFilenameOf(playlist);
        const std::string mediaFilename = showmesh::adapter::mediaFilenameOf(playlist);
        runtime_.observeCallback(name.c_str(), action.c_str(), canonicalSection.c_str(), item,
                                 sequenceFilename.c_str(), mediaFilename.c_str(),
                                 showmesh::adapter::playlistLoopOf(playlist));
    }

    // FPP's output thread, immediately before data goes to the outputs.
    void modifyChannelData(int, uint8_t* seqData) override {
        runtime_.modifyChannelData(seqData, FPPD_MAX_CHANNELS);
        publishFullStateIfChanged();
    }

    // Complete versioned state from another node, never a relative
    // adjustment, so a duplicate or delayed payload cannot apply twice.
    void multiSyncData(const uint8_t* data, int len) override { runtime_.adoptEncodedFullState(data, len); }

    // Contract section 2.2's transition-gain write and section 3.9's
    // definition republish. FPP 9 registers on the libhttpserver instance
    // FPP hands in, which is the same one carrying /fppd and /commands, so
    // the routes answer at the same LAN addresses FPP 10 serves them at
    // even though the two registration APIs share nothing.
    void registerApis(httpserver::webserver* ws) override {
        if (ws == nullptr) return;
        ws->register_resource(showmesh::kTransitionGainPath, &gainResource_, false);
        ws->register_resource(showmesh::kDefinitionRepublishPath, &republishResource_, false);
        ws->register_resource(showmesh::kBrightnessQueryPath, &brightnessQueryResource_, false);
    }

    // FPP 9 has no runtime unload endpoint, so this runs only on the
    // process-teardown path. Every route is withdrawn anyway: libhttpserver
    // holds a bare pointer to each resource member, and one left registered
    // past this object's life would be a call into a destroyed member.
    void unregisterApis(httpserver::webserver* ws) override {
        if (ws == nullptr) return;
        ws->unregister_resource(showmesh::kTransitionGainPath);
        ws->unregister_resource(showmesh::kDefinitionRepublishPath);
        ws->unregister_resource(showmesh::kBrightnessQueryPath);
    }

 private:
    // The libhttpserver side of the transition-gain route. A resource
    // object rather than a lambda because that is the shape FPP 9's
    // webserver takes, and it is a member so its lifetime is this
    // plugin's: register_resource keeps a bare pointer.
    class TransitionGainResource : public httpserver::http_resource {
     public:
        explicit TransitionGainResource(showmesh::ShowMeshRuntime* runtime) : runtime_(runtime) {
            disallow_all();
            set_allowing("POST", true);
        }

        std::shared_ptr<httpserver::http_response> render_POST(const httpserver::http_request& req) override {
            // Synchronous, and it must stay that way. Nothing here hands
            // work to another thread, so the resource is done being
            // touched by the time render_POST returns and unregistering
            // it later cannot race a request still inside it.
            const showmesh::TransitionGainResponse result =
                runtime_->applyTransitionGain(std::string(req.get_content()));
            return std::shared_ptr<httpserver::http_response>(new httpserver::string_response(
                result.body, result.status, "application/json"));
        }

     private:
        showmesh::ShowMeshRuntime* runtime_;
    };

    // The libhttpserver side of the republish route, a member for the same
    // lifetime reason gainResource_ is.
    class DefinitionRepublishResource : public httpserver::http_resource {
     public:
        explicit DefinitionRepublishResource(showmesh::ShowMeshRuntime* runtime) : runtime_(runtime) {
            disallow_all();
            set_allowing("POST", true);
        }

        std::shared_ptr<httpserver::http_response> render_POST(const httpserver::http_request& req) override {
            // Synchronous, and it must stay that way. It records that a
            // sweep is owed and returns; the worker thread performs the
            // sweep, so nothing here is still running when this resource
            // is unregistered.
            const showmesh::DefinitionRepublishResponse result =
                runtime_->applyDefinitionRepublish(std::string(req.get_content()));
            return std::shared_ptr<httpserver::http_response>(new httpserver::string_response(
                result.body, result.status, "application/json"));
        }

     private:
        showmesh::ShowMeshRuntime* runtime_;
    };

    // The libhttpserver side of contract section 3's read route, a member
    // for the same lifetime reason the two resources above are.
    class BrightnessQueryResource : public httpserver::http_resource {
     public:
        explicit BrightnessQueryResource(showmesh::ShowMeshRuntime* runtime) : runtime_(runtime) {
            disallow_all();
            set_allowing("GET", true);
        }

        std::shared_ptr<httpserver::http_response> render_GET(const httpserver::http_request&) override {
            const showmesh::BrightnessQueryResponse result = runtime_->queryBrightness();
            return std::shared_ptr<httpserver::http_response>(new httpserver::string_response(
                result.body, result.status, "application/json"));
        }

     private:
        showmesh::ShowMeshRuntime* runtime_;
    };

    void publishFullStateIfChanged() {
        const std::uint64_t revision = runtime_.brightness()->revision();
        if (revision == publishedRevision_) return;
        publishedRevision_ = revision;
        // Marked dirty here, on a revision change, not only at teardown.
        // A revision bumps when a target changes (a command, or an adopted
        // full state), never per interpolated frame, so this is a rare
        // mark and not an SD-wear problem: the sequence store already
        // writes more often than this, once per accepted playlist event.
        //
        // Teardown alone was not enough. On FPP 9 there is no unload or
        // shutdown hook at all, so the only flush was in the destructor,
        // and an fppd killed by a signal never runs it: setting a ceiling
        // and then stopping fppd gracefully was observed to persist
        // nothing, leaving the darker-safe restart guarantee not holding
        // on the version the deployed fleet runs. Marking dirty on change
        // makes the guarantee independent of how the process ends. The
        // persisted record carries the fade window, so one write at the
        // start of a fade is enough to restore it darker-safely.
        //
        // This is markBrightnessDirty(), not a direct flush: this method
        // runs on FPP's per-frame output thread, and the store's write is
        // a full read, a SHA-256, two fsyncs, and a rename against an SD
        // card -- work that must never run inside the frame budget. The
        // worker thread performs the actual write.
        runtime_.markBrightnessDirty();
        std::string payload = runtime_.encodeFullState();
        if (payload.empty()) return;
        PluginManager::INSTANCE.multiSyncData(showmesh::kPluginName,
                                              reinterpret_cast<uint8_t*>(&payload[0]),
                                              static_cast<int>(payload.size()));
    }

    showmesh::adapter::FppDefinitionSource definitions_;
    // Declared before runtime_ so it is constructed first: member
    // initialization follows declaration order regardless of the
    // constructor's init-list order, and runtime_'s constructor reads
    // sequenceStore_->load() immediately.
    showmesh::SequenceFileStore sequenceStore_;
    // Same reasoning and the same directory as sequenceStore_ (see
    // resolveSequenceStateDir(), sequence_store.h): runtime_'s
    // constructor reads brightnessStore_->load() immediately too.
    showmesh::BrightnessFileStore brightnessStore_;
    // Declared before runtime_ for the same reason sequenceStore_ is: the
    // runtime holds pointers into it from construction onward.
    showmesh::adapter::CoordinatorDelivery delivery_;
    // Declared after definitions_ (its constructor reads
    // definitions_.instanceUuid() once) and before runtime_ (which holds
    // a pointer into it), the same two reasons delivery_ above is placed
    // where it is.
    showmesh::adapter::FallbackActivationDelivery fallbackDelivery_;
    // Declared after delivery_: its constructor holds a pointer into
    // delivery_'s ConfigWatcher, and before runtime_, which holds a
    // pointer into this.
    showmesh::adapter::PairingDelivery pairingDelivery_;
    // Declared before runtime_ for the same reason: resolved from the
    // "ShowMeshSafeCeilingPercent" setting once, here, before runtime_'s
    // constructor uses it to settle an untrusted restart.
    int safeCeilingPercent_;
    showmesh::ShowMeshRuntime runtime_;
    // Declared after runtime_ on purpose: members initialise in
    // declaration order, so this captures a runtime_ that already exists.
    TransitionGainResource gainResource_{&runtime_};
    DefinitionRepublishResource republishResource_{&runtime_};
    BrightnessQueryResource brightnessQueryResource_{&runtime_};
    Command* command_ = nullptr;
    std::uint64_t publishedRevision_ = 0;
};

}  // namespace

extern "C" {
FPPPlugin* createPlugin() { return new ShowMeshFpp9Plugin(); }
}
