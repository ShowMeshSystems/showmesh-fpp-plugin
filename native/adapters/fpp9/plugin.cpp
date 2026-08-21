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
#include "fpp_definition_source.h"
#include "showmesh/runtime.h"

namespace {

showmesh::TimeMillis nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class ShowMeshFpp9Plugin : public FPPPlugin {
 public:
    ShowMeshFpp9Plugin()
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
        if (command_ != nullptr) {
            // removeCommand only unregisters; a Command subclass declared
            // here has its vtable in this library, so the registry must not
            // be left holding it.
            CommandManager::INSTANCE.removeCommand(command_);
            delete command_;
            command_ = nullptr;
        }
    }

    // FPP's callback thread. Copy the bounded evidence and return: no
    // definition fetch, no hash, no persistence, no network, no sleep.
    void playlistCallback(const Json::Value& playlist, const std::string& action, const std::string& section,
                          int item) override {
        const std::string name = showmesh::adapter::playlistNameOf(playlist);
        const std::string sequenceFilename = showmesh::adapter::sequenceFilenameOf(playlist, section, item);
        const std::string mediaFilename = showmesh::adapter::mediaFilenameOf(playlist, section, item);
        runtime_.observeCallback(name.c_str(), action.c_str(), section.c_str(), item, sequenceFilename.c_str(),
                                 mediaFilename.c_str());
    }

    // FPP's output thread, immediately before data goes to the outputs.
    void modifyChannelData(int, uint8_t* seqData) override {
        runtime_.modifyChannelData(seqData, FPPD_MAX_CHANNELS);
        publishFullStateIfChanged();
    }

    // Complete versioned state from another node, never a relative
    // adjustment, so a duplicate or delayed payload cannot apply twice.
    void multiSyncData(const uint8_t* data, int len) override { runtime_.adoptEncodedFullState(data, len); }

 private:
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

extern "C" {
FPPPlugin* createPlugin() { return new ShowMeshFpp9Plugin(); }
}
