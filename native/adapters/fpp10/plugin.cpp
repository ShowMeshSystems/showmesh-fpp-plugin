// The FPP 10 adapter. Same responsibilities as the FPP 9 one and the same
// host-neutral runtime underneath, but FPP 10's plugin contract is
// different enough that sharing one translation unit would mean an
// ifdef'd lifecycle: the ABI is versioned and checked at dlopen, the
// libhttpserver registration surface is gone, and teardown happens in a
// shutdown() virtual rather than in the destructor.

#include <chrono>
#include <functional>
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
#include "fpp_definition_source.h"
#include "showmesh/runtime.h"

namespace {

showmesh::TimeMillis nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class ShowMeshFpp10Plugin : public FPPPlugin {
 public:
    ShowMeshFpp10Plugin()
        : FPPPlugin(showmesh::kPluginName), runtime_(&definitions_, nullptr, nowMillis) {
        command_ = new showmesh::adapter::SetBrightnessCeilingCommand(&runtime_);
        CommandManager::INSTANCE.addCommand(command_);
        showmesh::adapter::configureChannelRanges(&*runtime_.brightness(), FPPD_MAX_CHANNELS);
        // start() before registering the settings listener: start() spawns
        // the worker thread and can throw, and a throwing constructor never
        // runs this object's destructor, so a listener registered first
        // would leave the global settings registry holding a callback that
        // captures a freed this.
        runtime_.start();
        registerSettingsListener(showmesh::kPluginName, showmesh::adapter::kChannelRangesSettingName,
                                  [this](const std::string&) {
                                      showmesh::adapter::configureChannelRanges(&*runtime_.brightness(),
                                                                                FPPD_MAX_CHANNELS);
                                  });
    }

    ~ShowMeshFpp10Plugin() override { quiesce(); }

    // FPP 10's teardown point. The worker thread is stopped and joined
    // here, and the registered command is both withdrawn and deleted,
    // because its vtable lives in this library and the library may be
    // unmapped afterwards. Teardown is complete when this returns, so no
    // settling predicate is needed.
    std::function<bool()> shutdown() override {
        quiesce();
        return nullptr;
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
        // Safe to call twice: unregisterSettingsListener() is a no-op if the
        // id is already gone, and quiesce() runs once from shutdown() and
        // again from the destructor FPP invokes after it.
        unregisterSettingsListener(showmesh::kPluginName, showmesh::adapter::kChannelRangesSettingName);
        runtime_.stop();
        if (command_ != nullptr) {
            CommandManager::INSTANCE.removeCommand(command_);
            delete command_;
            command_ = nullptr;
        }
    }

    void publishFullStateIfChanged() {
        const std::uint64_t revision = runtime_.brightness()->revision();
        if (revision == publishedRevision_) return;
        publishedRevision_ = revision;
        std::string payload = runtime_.encodeFullState();
        if (payload.empty()) return;
        PluginManager::INSTANCE.multiSyncData(showmesh::kPluginName,
                                              reinterpret_cast<uint8_t*>(&payload[0]),
                                              static_cast<int>(payload.size()));
    }

    showmesh::adapter::FppDefinitionSource definitions_;
    showmesh::ShowMeshRuntime runtime_;
    Command* command_ = nullptr;
    std::uint64_t publishedRevision_ = 0;
};

}  // namespace

// This plugin stops and joins its worker in shutdown(), withdraws its
// settings listener and the command it registered, registers no HTTP
// route, and hands nothing to a drogon event loop, so it is safe to
// unmap.
FPP_PLUGIN_SUPPORTS_UNLOAD()

extern "C" {
FPPPlugin* createPlugin() { return new ShowMeshFpp10Plugin(); }
}
