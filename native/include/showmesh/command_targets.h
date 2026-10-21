#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "showmesh/coordinator_config.h"
#include "showmesh/http_transport.h"

// The macro and action lists FPP's command editor shows as a dropdown.
// Host neutral: the resident worker refreshes them and each major's web
// server serves the rendered bodies from memory.

namespace showmesh {

// The paths each major registers with its own web server.
extern const char* const kCommandTargetMacrosPath;
extern const char* const kCommandTargetActionsPath;
// What FPP's command editor fetches, through FPP's Apache plugin-apis proxy.
extern const char* const kCommandTargetMacrosLanPath;
extern const char* const kCommandTargetActionsLanPath;

constexpr const char* kCommandTargetsFilename = "command-targets.json";

// One element of the coordinator's ConfigObjectsListResponse.objects.
struct ConfigObjectSummary {
    std::string id;
    std::string label;
    std::string show;
};

// Parses a ConfigObjectsListResponse body. Elements without a string id are skipped.
bool parseConfigObjectsList(const std::string& body, std::vector<ConfigObjectSummary>* out);

// True when id is non-empty and only [A-Za-z0-9._:-].
bool isCommandTargetId(const std::string& id);

// Renders {"<id>": "<label> (<show label>)"} ordered by visible text, with
// each '<' in the visible text replaced by U+2039. targets is null when never fetched; noun
// is "macros" or "actions" and names the placeholder entry.
std::string renderCommandTargetOptions(const std::vector<ConfigObjectSummary>* targets,
                                       const std::vector<ConfigObjectSummary>* shows, const std::string& noun);

class CommandTargetLists {
 public:
    // Loads the saved lists from path, if any. Never touches the network.
    explicit CommandTargetLists(std::string path);

    // Fetches all three lists once. A failed fetch keeps that list's last
    // good copy. Returns an empty string, or what went wrong. Never the credential.
    std::string refresh(HttpTransport* transport, CredentialSource* credentials, const std::string& baseUrl,
                        const std::function<bool()>& stopRequested = nullptr);

    std::string macrosBody() const;
    std::string actionsBody() const;

 private:
    using List = std::optional<std::vector<ConfigObjectSummary>>;

    void load();
    void renderLocked();
    std::string serializeLocked() const;

    std::string path_;
    mutable std::mutex mutex_;
    List macros_;
    List actions_;
    List shows_;
    std::string macrosBody_;
    std::string actionsBody_;
};

}  // namespace showmesh
