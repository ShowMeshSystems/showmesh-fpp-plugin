#include <atomic>
#include <thread>
#include <vector>

#include "check.h"
#include "showmesh/callback_handoff.h"

using showmesh::CallbackEvidence;
using showmesh::CallbackHandoff;
using showmesh::PlaylistAction;

// The producer here stands in for FPP's callback thread and the consumer
// for the resident worker. What must hold is that nothing is torn or lost
// without being counted: every observation is either taken or accounted
// for in the coalesced total, and the queue never grows past its capacity.
TEST(TheHandoffIsSafeAcrossTheCallbackAndWorkerThreads) {
    constexpr int kOffers = 20000;
    CallbackHandoff handoff(4);
    std::atomic<int> taken{0};
    std::atomic<long long> coalescedTotal{0};
    std::atomic<bool> producerDone{false};

    std::thread worker([&] {
        CallbackEvidence e;
        std::uint32_t coalesced = 0;
        while (!producerDone.load() || handoff.pending() > 0) {
            if (handoff.take(&e, &coalesced)) {
                ++taken;
                coalescedTotal += coalesced;
                // A taken observation is always internally consistent: the
                // position and the observation time were written by one
                // offer, never spliced from two.
                CHECK_EQ(static_cast<long long>(e.observedAtMillis), static_cast<long long>(1000 + e.position));
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (int position = 0; position < kOffers; ++position) {
        CallbackEvidence e;
        e.setPlaylistName("Main Show");
        e.setSection("mainPlaylist");
        e.position = position;
        e.action = PlaylistAction::kPlaying;
        e.observedAtMillis = 1000 + position;
        handoff.offer(e);
    }
    producerDone.store(true);
    worker.join();

    coalescedTotal += handoff.coalescedPending();
    CHECK_EQ(handoff.offeredTotal(), static_cast<std::uint64_t>(kOffers));
    CHECK_EQ(handoff.pending(), static_cast<std::size_t>(0));
    // Nothing vanished silently: everything offered was either delivered
    // or counted as a gap.
    CHECK_EQ(static_cast<long long>(taken.load()) + coalescedTotal.load(), static_cast<long long>(kOffers));
}
