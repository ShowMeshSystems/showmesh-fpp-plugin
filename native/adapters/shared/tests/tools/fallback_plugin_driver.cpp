// Bench tool, not part of the plugin: runs the plugin's own runtime, pairing
// worker, coordinator client and fallback executor over libcurl, with FPP
// replaced by lines on standard input. For an end to end run against a real
// coordinator and a real node agent.
//
//   fallback_plugin_driver <state-dir> <credential-dir> <coordinator-public-key-base64>
//                          <fpp-instance-uuid> <playlist-definition.json>
//
// <state-dir> holds config.json ({"coordinatorUrl":"..."}) and receives the
// status files and the installed program. Input lines:
//
//   pair                          write a pairing request; the code appears in <state-dir>/pairing-code
//   playing <playlist> <position> [loop]     FPP's `playing` callback for mainPlaylist/<position>
//   query_next <playlist> <position> [loop]  FPP's `query_next` callback
//   stop                          FPP's `stop` callback
//   sleep <milliseconds>
//   status                        print fallback-status.json
//   quit

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "curl_http_transport.h"
#include "fallback_executor.h"
#include "showmesh/atomic_write.h"
#include "showmesh/config_watcher.h"
#include "showmesh/coordinator_client.h"
#include "showmesh/coordinator_config.h"
#include "showmesh/pairing.h"
#include "showmesh/runtime.h"
#include "showmesh/sequence_store.h"

namespace {

showmesh::TimeMillis nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class FileDefinitions : public showmesh::PlaylistDefinitionSource {
 public:
    FileDefinitions(std::string uuid, std::string definition)
        : uuid_(std::move(uuid)), definition_(std::move(definition)) {}
    std::string definitionFor(const std::string&) override { return definition_; }
    std::string instanceUuid() override { return uuid_; }

 private:
    std::string uuid_;
    std::string definition_;
};

void logLine(bool isError, const std::string& line) {
    std::printf("[%lld] %s%s\n", static_cast<long long>(nowMillis()), isError ? "ERROR " : "", line.c_str());
    std::fflush(stdout);
}

class PrintingNotifier : public showmesh::fallback::FallbackStateNotifier {
 public:
    void raise(const std::string& message) override { logLine(false, "NOTICE RAISED: " + message); }
    void clear(const std::string& message) override { logLine(false, "NOTICE CLEARED: " + message); }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s <state-dir> <credential-dir> <coordinator-public-key-base64> "
                             "<fpp-instance-uuid> <playlist-definition.json>\n",
                     argv[0]);
        return 2;
    }
    const std::string stateDir = argv[1];
    const std::string credentialDir = argv[2];
    const std::string uuid = argv[4];
    std::string definition;
    showmesh::fallback::PinnedKeyLoadResult pinnedKey;
    if (!showmesh::readFileWhole(argv[5], &definition) ||
        !showmesh::fallback::detail::base64Decode(argv[3], &pinnedKey.publicKey) || pinnedKey.publicKey.size() != 32) {
        std::fprintf(stderr, "could not read the playlist definition or the coordinator public key\n");
        return 2;
    }
    pinnedKey.status = showmesh::fallback::PinnedKeyLoadStatus::kLoaded;

    showmesh::adapter::CurlHttpTransport transport;
    FileDefinitions definitions(uuid, definition);
    showmesh::FileStatusSink statusSink(stateDir);
    showmesh::FileCredentialSource credentials(credentialDir);
    const showmesh::CoordinatorUrlLoad url = showmesh::loadCoordinatorBaseUrl(stateDir);
    showmesh::CoordinatorClient client(&transport, &credentials, url.ok ? url.baseUrl : std::string(), nowMillis,
                                       &statusSink);
    showmesh::ConfigWatcher configWatcher(stateDir, &client);
    showmesh::PairingWorker pairing(stateDir, credentialDir, &transport, &configWatcher, nowMillis);
    showmesh::SequenceFileStore sequenceStore(stateDir);

    PrintingNotifier notifier;
    showmesh::fallback::FallbackExecutorOptions options;
    options.clock = nowMillis;
    options.fppInstanceUuid = uuid;
    options.stateDir = stateDir;
    options.credentialDir = credentialDir;
    options.installPath = showmesh::joinPath(stateDir, "fallback-program.json");
    options.pinnedKey = pinnedKey;
    options.notifier = &notifier;
    options.log = logLine;
    showmesh::fallback::FallbackExecutor executor(options, &transport);

    showmesh::ShowMeshRuntime runtime(&definitions, &client, nowMillis, &sequenceStore, &client, nullptr,
                                      showmesh::kDefaultSafeCeilingPercent, &executor, &pairing, &configWatcher);
    runtime.start();
    logLine(false, "driver started, boot id " + executor.bootId());

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string command;
        in >> command;
        if (command == "quit") break;
        if (command == "pair") {
            std::ofstream(showmesh::joinPath(stateDir, showmesh::kPairingRequestFilename)) << "{}";
        } else if (command == "playing" || command == "query_next") {
            std::string playlist;
            int position = 0;
            int loop = -1;
            in >> playlist >> position >> loop;
            logLine(false, "FPP callback: " + line);
            runtime.observeCallback(playlist.c_str(), command.c_str(), "mainPlaylist", position, "", "",
                                    loop >= 0 ? std::optional<int>(loop) : std::nullopt);
        } else if (command == "stop") {
            logLine(false, "FPP callback: stop");
            runtime.observeCallback("", "stop", "", 0, "", "");
        } else if (command == "sleep") {
            int millis = 0;
            in >> millis;
            std::this_thread::sleep_for(std::chrono::milliseconds(millis));
        } else if (command == "status") {
            std::string status;
            showmesh::readFileWhole(showmesh::joinPath(stateDir, showmesh::fallback::kFallbackStatusFilename), &status);
            logLine(false, "STATUS " + status);
        }
    }
    runtime.stop();
    return 0;
}
