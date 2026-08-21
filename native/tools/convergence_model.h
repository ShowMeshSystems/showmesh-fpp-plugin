#pragma once

// A randomized model of what the two adapters actually do with
// BrightnessEngine, used to measure MultiSync convergence rather than
// assert it from a handful of hand-picked states. It is deliberately not
// part of `make -C native test`: it is a diagnostic harness for defect 1
// of the second adversarial pass, run manually and reported in the
// session's evidence, not a CI gate.
//
// Model: at each command round, every node issues a local setCeiling
// command within the same simulated instant (a near-simultaneous burst,
// not one random node at a time), applies output frames on its own
// schedule (skewing its private lastAppliedCeiling/lastAppliedGain
// independently of every other node), and broadcasts its full state to
// every other node whenever its own revision() changes, exactly as the
// adapters' publishFullStateIfChanged would. Delivery is a discrete-event
// queue: a message can be duplicated, delivered out of order, and delayed
// by a random amount, and receiving one can itself trigger a further
// broadcast (an adoption bumps revision()), which is how a real gossip
// mesh converges, and also how a bug in the ordering key turns into an
// unbounded rebroadcast storm.
//
// The burst is what makes this model able to observe a genuine conflict:
// one acting node per command, spaced hundreds of milliseconds apart, is
// resolved by the timestamp tier alone almost every time, so the hash
// tiebreak is exercised only when two broadcasts happen to carry
// byte-identical content and can never observe a real disagreement. Every
// node committing its own independent choice at the same simulated
// millisecond is what a real MultiSync burst (e.g. several controllers
// all reacting to one show-wide command) looks like, and is the case that
// actually stresses the ordering key's tie-break tiers.

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "showmesh/brightness.h"

namespace showmesh {
namespace convergence_model {

struct SimConfig {
    int nodeCount = 3;
    bool distinctInstanceIds = false;
    // Number of command rounds. Every round, every node issues one command
    // at the same simulated millisecond, so the total number of setCeiling
    // calls in a trial is commandCount * nodeCount.
    int commandCount = 8;
    // Every node applies an output frame at roughly this interval for the
    // whole simulated window (plus a tail past the last command), the way
    // a real adapter's output thread runs continuously at the show's
    // frame rate: lastApplied keeps drifting for as long as messages
    // might still be in flight, which is what lets a comparison that
    // never settles show up as a genuine non-convergence instead of a
    // bounded, self-limiting wobble.
    TimeMillis frameIntervalMillis = 33;
    TimeMillis tailMillis = 3000;
    // Safety cap: a trial that has not quiesced after this many delivered
    // packets is counted as a disagreement rather than looped forever,
    // matching how a real runaway would be observed (never settles).
    std::uint64_t maxPackets = 40000;
};

struct TrialResult {
    bool converged = false;
    std::uint64_t packetsDelivered = 0;
    int framesApplied = 0;
};

// event kinds processed in simulated-time order from a std::multimap so
// equal-time events run in the order they were scheduled.
struct Event {
    enum class Kind { kCommand, kFrame, kDeliver } kind;
    int actingNode = 0;
    int targetNode = 0;   // kDeliver only
    int senderNode = 0;   // kDeliver only
    int targetPercent = 0;
    std::int64_t fadeSeconds = 0;
    BrightnessState payload;  // kDeliver only
};

// Adapts to whichever adoptState signature the linked BrightnessEngine
// exposes. AdoptFn's job is exactly one call: adopt(engine, state, now).
template <typename AdoptFn>
TrialResult runTrial(const SimConfig& cfg, std::uint64_t seed, AdoptFn adopt) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> percentDist(0, 100);
    std::uniform_int_distribution<int> fadeDist(0, 300);
    std::uniform_int_distribution<int> delayDist(0, 4000);
    std::uniform_int_distribution<int> dupDist(0, 4);  // 20% duplication
    std::uniform_int_distribution<int> jitterDist(0, 900);

    constexpr TimeMillis kBase = 2'000'000'000'000;  // well inside the plausible band

    std::vector<BrightnessEngine> nodes(static_cast<std::size_t>(cfg.nodeCount));
    for (int i = 0; i < cfg.nodeCount; ++i) {
        if (cfg.distinctInstanceIds) {
            nodes[static_cast<std::size_t>(i)].setInstanceId("node-" + std::to_string(i));
        }
    }

    std::multimap<TimeMillis, Event> queue;
    TimeMillis t = kBase;
    for (int i = 0; i < cfg.commandCount; ++i) {
        t += 500 + jitterDist(rng);
        // Every node commands at this same simulated millisecond: a
        // near-simultaneous burst, not one random node at a time, so real
        // conflicting content lands at an equal stateChangedAtMillis
        // instead of always being resolved by the timestamp tier before
        // the hash tiebreak is ever reached.
        for (int node = 0; node < cfg.nodeCount; ++node) {
            Event e;
            e.kind = Event::Kind::kCommand;
            e.actingNode = node;
            e.targetPercent = percentDist(rng);
            e.fadeSeconds = fadeDist(rng);
            queue.emplace(t, e);
        }
    }
    const TimeMillis frameHorizon = t + cfg.tailMillis;
    std::uniform_int_distribution<int> frameJitterDist(0, 10);
    for (int i = 0; i < cfg.nodeCount; ++i) {
        // A small per-node phase offset means no two nodes apply frames
        // at exactly the same simulated millisecond, which is what real
        // independent output threads look like.
        TimeMillis ft = kBase + (i * cfg.frameIntervalMillis) / (cfg.nodeCount + 1);
        while (ft <= frameHorizon) {
            Event e;
            e.kind = Event::Kind::kFrame;
            e.actingNode = i;
            queue.emplace(ft, e);
            ft += cfg.frameIntervalMillis + frameJitterDist(rng);
        }
    }

    TrialResult result;
    std::vector<std::uint8_t> frame(4, 255);

    while (!queue.empty()) {
        if (result.packetsDelivered > cfg.maxPackets) {
            result.converged = false;
            return result;
        }
        auto it = queue.begin();
        const TimeMillis now = it->first;
        Event e = it->second;
        queue.erase(it);

        if (e.kind == Event::Kind::kCommand) {
            BrightnessEngine& n = nodes[static_cast<std::size_t>(e.actingNode)];
            const std::uint64_t before = n.revision();
            n.setCeiling(e.targetPercent, e.fadeSeconds, now);
            if (n.revision() != before) {
                BrightnessState published = n.captureState(now);
                for (int j = 0; j < cfg.nodeCount; ++j) {
                    if (j == e.actingNode) continue;
                    Event d;
                    d.kind = Event::Kind::kDeliver;
                    d.targetNode = j;
                    d.senderNode = e.actingNode;
                    d.payload = published;
                    queue.emplace(now + delayDist(rng), d);
                    if (dupDist(rng) == 0) {
                        queue.emplace(now + delayDist(rng) + 1, d);
                    }
                }
            }
        } else if (e.kind == Event::Kind::kFrame) {
            nodes[static_cast<std::size_t>(e.actingNode)].applyToFrame(frame.data(), frame.size(), now);
            ++result.framesApplied;
        } else {
            ++result.packetsDelivered;
            BrightnessEngine& n = nodes[static_cast<std::size_t>(e.targetNode)];
            const std::uint64_t before = n.revision();
            adopt(n, e.payload, now);
            if (n.revision() != before) {
                BrightnessState published = n.captureState(now);
                for (int j = 0; j < cfg.nodeCount; ++j) {
                    if (j == e.targetNode) continue;
                    Event d;
                    d.kind = Event::Kind::kDeliver;
                    d.targetNode = j;
                    d.senderNode = e.targetNode;
                    d.payload = published;
                    queue.emplace(now + delayDist(rng), d);
                }
            }
        }
    }

    // Agreement means every node's shared, published state (never its
    // private lastApplied fields, which are never synchronized by
    // design) matches node 0's.
    const BrightnessState reference = nodes[0].captureState(kBase);
    result.converged = true;
    for (int i = 1; i < cfg.nodeCount; ++i) {
        const BrightnessState other = nodes[static_cast<std::size_t>(i)].captureState(kBase);
        if (reference.ceilingStart != other.ceilingStart || reference.ceilingTarget != other.ceilingTarget ||
            reference.ceilingFadeStartMillis != other.ceilingFadeStartMillis ||
            reference.ceilingFadeEndMillis != other.ceilingFadeEndMillis ||
            reference.gainStart != other.gainStart || reference.gainTarget != other.gainTarget ||
            reference.gainFadeStartMillis != other.gainFadeStartMillis ||
            reference.gainFadeEndMillis != other.gainFadeEndMillis) {
            result.converged = false;
            break;
        }
    }
    return result;
}

}  // namespace convergence_model
}  // namespace showmesh
