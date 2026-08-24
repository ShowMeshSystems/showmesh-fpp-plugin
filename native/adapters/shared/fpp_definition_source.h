#pragma once

#include <dirent.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

    // Every playlist definition on the host, so the worker's start-up
    // sweep can publish them all before FPP has played anything. Listing
    // the directory the definitions already come from one at a time; no
    // filesystem watch and no FPP API call.
    std::vector<std::string> playlistNames() override {
        std::vector<std::string> names;
        const std::string dir = FPP_DIR_PLAYLIST("");
        DIR* handle = ::opendir(dir.c_str());
        if (handle == nullptr) return names;
        const std::string suffix = ".json";
        while (const dirent* entry = ::readdir(handle)) {
            const std::string filename = entry->d_name;
            if (filename.size() <= suffix.size()) continue;
            if (filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            const std::string name = filename.substr(0, filename.size() - suffix.size());
            if (!playlistNameIsPathSafe(name)) continue;
            names.push_back(name);
        }
        ::closedir(handle);
        return names;
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
