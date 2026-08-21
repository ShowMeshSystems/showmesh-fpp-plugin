// Standalone diagnostic: runs the randomized MultiSync convergence model
// (convergence_model.h) across every {node count, instance-id mode}
// configuration and prints the disagreement rate. Not part of `make -C
// native test`; compiled and run manually, once against the pre-fix
// brightness.cpp/.h and once against the fix, to produce the before/after
// numbers reported for defect 1.
//
// Build (from native/):
//   c++ -std=c++17 -O2 -Iinclude -Itools tools/convergence_harness.cpp \
//       src/brightness.cpp src/json.cpp src/sha256.cpp -o /tmp/convergence_harness
//
// The pre-fix build additionally needs
//   -DLEGACY_ONE_ARG_ADOPT_STATE
// which selects the one-argument adoptState(state) call instead of
// adoptState(state, now); the pre-fix header does not declare the
// two-argument form.

#include <cstdio>
#include <random>

#include "convergence_model.h"

int main() {
    using showmesh::BrightnessEngine;
    using showmesh::BrightnessState;
    using showmesh::TimeMillis;
    using showmesh::convergence_model::SimConfig;
    using showmesh::convergence_model::TrialResult;
    using showmesh::convergence_model::runTrial;

#ifdef LEGACY_ONE_ARG_ADOPT_STATE
    auto adopt = [](BrightnessEngine& e, const BrightnessState& s, TimeMillis) { return e.adoptState(s); };
#else
    auto adopt = [](BrightnessEngine& e, const BrightnessState& s, TimeMillis now) { return e.adoptState(s, now); };
#endif

    const int nodeCounts[] = {2, 3, 4, 5};
    const bool idModes[] = {false, true};
    const std::uint64_t trialsPerConfig = 10000;

    std::mt19937_64 seedSource(0xC0FFEEu);

    std::printf("%-6s %-16s %10s %14s %10s\n", "nodes", "instanceIds", "trials", "disagreements", "rate");
    std::uint64_t totalTrials = 0;
    std::uint64_t totalDisagreements = 0;

    for (int nodeCount : nodeCounts) {
        for (bool distinct : idModes) {
            SimConfig cfg;
            cfg.nodeCount = nodeCount;
            cfg.distinctInstanceIds = distinct;

            std::uint64_t disagreements = 0;
            std::uint64_t maxPacketsSeen = 0;
            for (std::uint64_t trial = 0; trial < trialsPerConfig; ++trial) {
                const std::uint64_t seed = seedSource();
                TrialResult r = runTrial(cfg, seed, adopt);
                if (r.packetsDelivered > maxPacketsSeen) maxPacketsSeen = r.packetsDelivered;
                if (!r.converged) ++disagreements;
            }
            totalTrials += trialsPerConfig;
            totalDisagreements += disagreements;
            std::printf("%-6d %-16s %10llu %14llu %9.4f%% (max packets in one trial: %llu)\n", nodeCount,
                        distinct ? "distinct" : "empty", static_cast<unsigned long long>(trialsPerConfig),
                        static_cast<unsigned long long>(disagreements),
                        100.0 * static_cast<double>(disagreements) / static_cast<double>(trialsPerConfig),
                        static_cast<unsigned long long>(maxPacketsSeen));
        }
    }

    std::printf("TOTAL: %llu trials, %llu disagreements, %.6f%%\n", static_cast<unsigned long long>(totalTrials),
                static_cast<unsigned long long>(totalDisagreements),
                100.0 * static_cast<double>(totalDisagreements) / static_cast<double>(totalTrials));
    return 0;
}
