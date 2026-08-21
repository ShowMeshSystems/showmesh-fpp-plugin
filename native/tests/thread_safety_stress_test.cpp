#include "showmesh/runtime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "check.h"

using namespace showmesh;

namespace {

TimeMillis nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class NullDefinitions : public PlaylistDefinitionSource {
 public:
    std::string definitionFor(const std::string&) override { return std::string(); }
    std::string instanceUuid() override { return std::string("stress-node"); }
};

class NullSink : public ObservationSink {
 public:
    bool publish(const PlaylistEntryObservation&) override { return true; }
    bool publishUnavailable(const PlaylistEntryObservation&) override { return true; }
};

}  // namespace

// finding 1: BrightnessEngine had no synchronization across the three
// fppd threads that reach it. This drives the same three roles at once,
// the way the FPP 9/10 adapters do: an output thread scaling frames and
// reading composed state, a command thread starting fades, and a
// MultiSync thread adopting a peer's full state. Every value the output
// thread observes must stay in the valid 0-100 range; a torn fadeTo()
// write (target_ stored before endMillis_) would read the new target
// against the old, already-expired window and jump outside it for one
// frame. This is also the regression target for ThreadSanitizer.
TEST(ThreeThreadsHittingTheRuntimeLikeTheAdaptersDoProduceNoTornFrame) {
    NullDefinitions definitions;
    NullSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, nowMillis);
    runtime.start();

    std::atomic<bool> stop{false};
    std::atomic<bool> sawOutOfRange{false};

    std::thread outputThread([&] {
        std::vector<std::uint8_t> frame(64, 255);
        while (!stop.load()) {
            runtime.modifyChannelData(frame.data(), frame.size());
            const TimeMillis now = nowMillis();
            const int percent = runtime.brightness()->effectivePercentAt(now);
            if (percent < 0 || percent > 100) sawOutOfRange.store(true);
            (void)runtime.encodeFullState();
        }
    });

    std::thread commandThread([&] {
        int target = 0;
        while (!stop.load()) {
            target = (target + 13) % 101;
            runtime.applyBrightnessCommand(std::to_string(target), "0");
            std::this_thread::yield();
        }
    });

    // Stands in for the MultiSync thread: a second, independent runtime
    // plays the role of a peer node whose full state gets adopted, the
    // way multiSyncData() feeds a received payload into adoptState().
    std::thread multiSyncThread([&] {
        ShowMeshRuntime peer(&definitions, &sink, nowMillis);
        int target = 100;
        while (!stop.load()) {
            target = (target + 7) % 101;
            peer.applyBrightnessCommand(std::to_string(target), "1");
            const std::string payload = peer.encodeFullState();
            if (!payload.empty()) {
                runtime.adoptEncodedFullState(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                              static_cast<int>(payload.size()));
            }
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    outputThread.join();
    commandThread.join();
    multiSyncThread.join();
    runtime.stop();

    CHECK(!sawOutOfRange.load());
}

// finding 10: EngineAccessor's own comment says the lock releases only
// when the accessor temporary is destroyed at the end of the calling
// expression, but ranges() and instanceId() returned references into
// engine_ that outlived that temporary. A verifier reproduced a race
// through instanceId() under ThreadSanitizer. Returning both by value
// (this test's regression target) copies the data out while the lock is
// still held, before the accessor's destructor runs.
TEST(ConcurrentIdentityAndRangeReadsDoNotRaceEngineAccessorMutations) {
    NullDefinitions definitions;
    NullSink sink;
    ShowMeshRuntime runtime(&definitions, &sink, nowMillis);
    runtime.start();

    std::atomic<bool> stop{false};

    std::thread writerThread([&] {
        int i = 0;
        while (!stop.load()) {
            runtime.brightness()->setInstanceId("node-" + std::to_string(i % 8));
            RangeConfig config;
            config.apply.push_back(ChannelRange{1, static_cast<std::uint32_t>(16 + (i % 8))});
            runtime.brightness()->configureRanges(config, 64);
            ++i;
            std::this_thread::yield();
        }
    });

    std::thread readerThread([&] {
        while (!stop.load()) {
            const std::string& id = runtime.brightness()->instanceId();
            const RangeConfig& config = runtime.brightness()->ranges();
            // Actually touch the returned data after EngineAccessor's
            // lock has released, not merely bind a name to it: a
            // reference-returning accessor is only provably racy once
            // its result is read, not merely held.
            volatile std::size_t idLen = id.size();
            volatile std::size_t rangeCount = config.apply.size();
            (void)idLen;
            (void)rangeCount;
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    writerThread.join();
    readerThread.join();
    runtime.stop();
}
