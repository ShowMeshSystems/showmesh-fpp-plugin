// The FPP 10 adapter. Same responsibilities as the FPP 9 one and the same
// host-neutral runtime underneath, but FPP 10's plugin contract is
// different enough that sharing one translation unit would mean an
// ifdef'd lifecycle: the ABI is versioned and checked at dlopen, the
// libhttpserver registration surface is gone, and teardown happens in a
// shutdown() virtual rather than in the destructor.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Plugin.h"
#include "Plugins.h"
#include "Sequence.h"
#include "commands/Commands.h"

#include "brightness_command.h"
#include "callback_fields.h"
#include "channel_ranges.h"
#include "coordinator_delivery.h"
#include "fpp_definition_source.h"
#include "showmesh/brightness_store.h"
#include "showmesh/runtime.h"

namespace {

showmesh::TimeMillis nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Reports what the constructor found on disk for brightness state, once,
// at startup. A silent dark settle is worse than a blackout: it is
// indistinguishable from a dead output chain and sends an operator
// debugging the wrong subsystem on show night. See
// BrightnessEngine::settleDarkAfterUntrustedRestart and
// ShowMeshRuntime::brightnessRestartTrust().
void logBrightnessRestartTrust(showmesh::BrightnessRestartTrust trust) {
    switch (trust) {
        case showmesh::BrightnessRestartTrust::kTrustedOrNoRecord:
            return;
        case showmesh::BrightnessRestartTrust::kPrimaryUnreadableBackupRecovered:
            LogErr(VB_PLUGIN,
                   "ShowMesh: the primary brightness record could not be read; only a superseded backup "
                   "was found. Refusing to trust it, this plugin has deliberately settled at zero "
                   "brightness rather than risk restoring brighter than what was actually applied. This "
                   "clears on the next ShowMesh brightness command or an adopted MultiSync full state.\n");
            return;
        case showmesh::BrightnessRestartTrust::kNeitherRecordReadable:
            LogErr(VB_PLUGIN,
                   "ShowMesh: neither the primary nor the backup brightness record could be read. This "
                   "plugin has deliberately settled at zero brightness rather than guess what was last "
                   "applied. This clears on the next ShowMesh brightness command or an adopted MultiSync "
                   "full state.\n");
            return;
    }
}

class ShowMeshFpp10Plugin : public FPPPlugin {
 public:
    ShowMeshFpp10Plugin()
        : FPPPlugin(showmesh::kPluginName),
          sequenceStore_(showmesh::resolveSequenceStateDir()),
          brightnessStore_(showmesh::resolveSequenceStateDir()),
          delivery_(nowMillis),
          runtime_(&definitions_, delivery_.client(), nowMillis, &sequenceStore_, delivery_.client(),
                  &brightnessStore_) {
        logBrightnessRestartTrust(runtime_.brightnessRestartTrust());
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

    // If shutdown() ran, the quiesce thread has already done the work and
    // this just joins it. If it never ran (this object destroyed some
    // other way FPP's contract does not document), quiesce() runs here
    // instead, synchronously, as a defensive fallback: it is idempotent,
    // see its own comment.
    ~ShowMeshFpp10Plugin() override {
        if (quiesceThread_.joinable()) {
            quiesceThread_.join();
        } else {
            quiesce();
        }
    }

    // FPP 10's teardown point, reached only after FPP has already
    // detached this plugin (routes disarmed, nothing calls in again).
    // quiesce() does real, bounded work -- joining the worker thread and
    // fsync'ing two small files -- so it runs on its own thread rather
    // than blocking FPP's main loop, and this returns a predicate FPP
    // polls roughly once a second instead of blocking here until it is
    // done. FPP gives up waiting after 60s regardless, so quiesceDone_ is
    // set from inside the thread itself (never left to a caller who might
    // not poll again) and everything quiesce() does completes in well
    // under a second on any real host.
    std::function<bool()> shutdown() override {
        // Withdrawn synchronously, before the thread and before this
        // returns, because FPP does not wait for the predicate before
        // checking. PluginManager::unloadPlugin takes the predicate and
        // then IMMEDIATELY runs its leftover-command backstop, only
        // polling the predicate afterwards, so anything withdrawn on the
        // quiesce thread is still registered at the moment FPP looks.
        // Deferring these two was observed on a real FPP 10.0 to produce
        // "left 2 command(s) registered at unload ... it should withdraw
        // and delete them in shutdown()" and to have FPP delete the
        // command out from under this object.
        //
        // Unregistering the settings listener here rather than on the
        // thread closes a second gap in the same window: FPP detaches the
        // plugin once this returns, and a settings change arriving before
        // the thread ran would otherwise reach a listener whose runtime_
        // is about to be stopped.
        unregisterSettingsListener(showmesh::kPluginName, showmesh::adapter::kChannelRangesSettingName);
        withdrawCommand();
        quiesceThread_ = std::thread([this] {
            quiesce();
            quiesceDone_.store(true);
        });
        return [this] { return quiesceDone_.load(); };
    }

    void playlistCallback(const Json::Value& playlist, const std::string& action, const std::string& section,
                          int item) override {
        const std::string name = showmesh::adapter::playlistNameOf(playlist);
        const std::string sequenceFilename = showmesh::adapter::sequenceFilenameOf(playlist, section, item);
        const std::string mediaFilename = showmesh::adapter::mediaFilenameOf(playlist, section, item);
        runtime_.observeCallback(name.c_str(), action.c_str(), section.c_str(), item, sequenceFilename.c_str(),
                                 mediaFilename.c_str());
    }

    void modifyChannelData(int, uint8_t* seqData) override {
        runtime_.modifyChannelData(seqData, FPPD_MAX_CHANNELS);
        publishFullStateIfChanged();
    }

    void multiSyncData(const uint8_t* data, int len) override { runtime_.adoptEncodedFullState(data, len); }

 private:
    void quiesce() {
        // Withdrawn before anything else: settings.h's listener registry is
        // a global that outlives this object, and shutdown() runs while the
        // library is still mapped, unlike the dlclose() that may follow it.
        // Idempotent by construction (unregisterSettingsListener() is a
        // no-op once the id is gone, stop() is a no-op once already
        // stopped, and command_ is nulled out at the end), which is what
        // lets the destructor fall back to calling this directly on the
        // rare path where shutdown() never ran.
        unregisterSettingsListener(showmesh::kPluginName, showmesh::adapter::kChannelRangesSettingName);
        // stop() joins the worker before either flush runs, so neither
        // flush races the worker thread's own access to sequence_ or
        // engine_ (flushSequenceState() and flushBrightnessState() both
        // document this precondition).
        runtime_.stop();
        runtime_.flushSequenceState();
        runtime_.flushBrightnessState();
        // Already done synchronously when shutdown() ran; still called
        // here, idempotently, for the destructor path where it did not.
        withdrawCommand();
    }

    // Idempotent: a second call is a no-op once command_ is null. Called
    // from shutdown() synchronously and from quiesce() as the fallback,
    // so whichever path runs first owns the deletion and the other sees
    // nothing to do.
    void withdrawCommand() {
        if (command_ == nullptr) return;
        CommandManager::INSTANCE.removeCommand(command_);
        delete command_;
        command_ = nullptr;
    }

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
    showmesh::ShowMeshRuntime runtime_;
    Command* command_ = nullptr;
    std::uint64_t publishedRevision_ = 0;
    // shutdown()'s background quiesce thread and its readiness flag. See
    // shutdown() and the destructor.
    std::thread quiesceThread_;
    std::atomic<bool> quiesceDone_{false};
};

}  // namespace

// This plugin stops and joins its worker in shutdown(), withdraws its
// settings listener and the command it registered, registers no HTTP
// route, and hands nothing to a drogon event loop, so it is safe to
// unmap. Its outbound client links libcurl and calls curl_global_init()
// but deliberately never calls curl_global_cleanup(): libcurl's own state
// lives in libcurl.so, which fppd holds open regardless of this library,
// and tearing it down here would tear it down underneath fppd.
FPP_PLUGIN_SUPPORTS_UNLOAD()

extern "C" {
FPPPlugin* createPlugin() { return new ShowMeshFpp10Plugin(); }
}
