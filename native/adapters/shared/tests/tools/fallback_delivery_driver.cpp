// Bench tool, not part of the plugin: hands one node a signed fallback program
// and sends it one signed activation, with the same code the plugin ships
// (resolver, request builder, signer, section 5.8 handling, libcurl).
//
//   fallback_delivery_driver <signed-program.json> <coordinator-public-key-base64>
//                            <executor-key-file> <entry-key> [node-id]
//
// The executor key file holds the 64 hex character seed, as the plugin stores
// it. Every target of the entry is addressed unless node-id names one.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "curl_http_transport.h"
#include "fallback_activation_resolver.h"
#include "fallback_executor_key.h"
#include "fallback_node_delivery.h"
#include "showmesh/atomic_write.h"
#include "showmesh/pairing.h"

int main(int argc, char** argv) {
    using namespace showmesh::fallback;
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <signed-program.json> <coordinator-public-key-base64> <executor-key-file> "
                             "<entry-key> [node-id]\n",
                     argv[0]);
        return 2;
    }
    std::string program;
    std::string seedHex;
    std::vector<uint8_t> coordinatorKey;
    std::vector<uint8_t> seed;
    ExecutorKey key;
    if (!showmesh::readFileWhole(argv[1], &program) || !detail::base64Decode(argv[2], &coordinatorKey) ||
        !showmesh::readFileWhole(argv[3], &seedHex)) {
        std::fprintf(stderr, "could not read the program, the coordinator key, or the executor key file\n");
        return 2;
    }
    while (!seedHex.empty() && (seedHex.back() == '\n' || seedHex.back() == '\r')) seedHex.pop_back();
    if (!hexDecode(seedHex, &seed) || !executorKeyFromSeed(seed, &key)) {
        std::fprintf(stderr, "the executor key file does not hold a 64 hex character seed\n");
        return 2;
    }
    const std::string onlyNode = argc > 5 ? argv[5] : "";

    const ActivationResolution resolution =
        ResolveActivationFromDocument(argv[4], program, coordinatorKey, std::chrono::system_clock::now());
    std::printf("resolve %s %s\n", ActivationResolveKindName(resolution.kind), resolution.reason.c_str());
    if (resolution.kind != ActivationResolveKind::kMatch) return 1;
    const ActivationMatch& match = *resolution.match;
    std::printf("executor key %s, program %s it\n", key.publicKeyBase64.c_str(),
                match.executorPublicKey() == key.publicKeyBase64 ? "carries" : "DOES NOT carry");

    showmesh::adapter::CurlHttpTransport transport;
    bool allAuthorized = true;
    for (const ActivationTarget& target : match.targets()) {
        if (!onlyNode.empty() && target.nodeId != onlyNode) continue;
        if (!target.address.has_value()) {
            std::printf("node %s no-address\n", target.nodeId.c_str());
            allAuthorized = false;
            continue;
        }
        const NodeAnswer handed = HandProgramToNode(&transport, *target.address, match.fppInstanceUuid(), program);
        std::printf("node %s program status=%d outcome=%s reason=%s\n", target.nodeId.c_str(), handed.statusCode,
                    handed.outcome.c_str(), handed.reason.c_str());

        uint8_t random[16];
        std::string signature;
        if (!showmesh::readRandomBytes(random, sizeof(random))) return 2;
        const std::string executionId = formatExecutionId(random);
        const ActivationRequestBuild build = BuildActivationRequest(match, target, executionId);
        if (!build.ok || !signWithExecutorKey(key, build.canonical, &signature)) {
            std::printf("node %s not sent: %s\n", target.nodeId.c_str(), build.refusal.c_str());
            allAuthorized = false;
            continue;
        }
        const NodeDeliveryResult result = DeliverActivation(
            &transport, *target.address, ActivationRequestBody(build.canonical, signature), match.fppInstanceUuid(),
            program, [](int millis) {
                std::this_thread::sleep_for(std::chrono::milliseconds(millis));
                return true;
            });
        std::printf("node %s activation executionId=%s outcome=%s attempts=%d reason=%s\n", target.nodeId.c_str(),
                    executionId.c_str(), result.outcome.c_str(), result.attempts, result.reason.c_str());
        allAuthorized = allAuthorized && result.activated();
    }
    return allAuthorized ? 0 : 1;
}
