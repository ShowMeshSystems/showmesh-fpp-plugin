#pragma once

#include <fstream>
#include <sstream>
#include <string>

#include "settings.h"
#include "showmesh/runtime.h"

// Resolving a playlist's complete definition and this host's persistent
// identity, using FPP's own accessors. Everything here is called from the
// worker thread: reading a file is exactly the work the callback boundary
// exists to keep off FPP's callback thread.

namespace showmesh {
namespace adapter {

class FppDefinitionSource : public PlaylistDefinitionSource {
 public:
    std::string definitionFor(const std::string& playlistName) override {
        if (!playlistNameIsPathSafe(playlistName)) return std::string();
        const std::string path = FPP_DIR_PLAYLIST("/" + playlistName + ".json");
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::string();
        std::ostringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }

    std::string instanceUuid() override {
        const std::string uuid = getSetting("SystemUUID");
        // FPP reports "unknown" before the identity is established. That is
        // an absent UUID, not an identity, and treating it as one would
        // give every un-provisioned host the same entry keys.
        if (uuid.empty() || uuid == "unknown") return std::string();
        return uuid;
    }
};

}  // namespace adapter
}  // namespace showmesh
