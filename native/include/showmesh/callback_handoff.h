#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "showmesh/fading_value.h"
#include "showmesh/playlist_identity.h"

namespace showmesh {

// Field bounds for one callback observation. The evidence struct is fixed
// size and allocation-free on purpose: it is filled on FPP's own callback
// thread, which belongs to a running show. Anything unbounded there is a
// show risk, not a memory risk.
constexpr std::size_t kMaxPlaylistNameLength = 255;
constexpr std::size_t kMaxSectionLength = 63;
constexpr std::size_t kMaxFilenameLength = 255;

// CallbackEvidence is exactly what the callback copies and returns with.
// Resolving the playlist definition, hashing it, assigning a sequence,
// persisting, and sending all happen later, on the worker thread.
struct CallbackEvidence {
    char playlistName[kMaxPlaylistNameLength + 1] = {0};
    char section[kMaxSectionLength + 1] = {0};
    char sequenceFilename[kMaxFilenameLength + 1] = {0};
    char mediaFilename[kMaxFilenameLength + 1] = {0};
    int position = 0;
    PlaylistAction action = PlaylistAction::kUnknown;
    TimeMillis observedAtMillis = 0;

    // True when the copy lost bytes off the end of the source. playlistName
    // and section determine entry identity: two different values that
    // share their first 255 or 63 bytes must never read as the same entry.
    bool playlistNameTruncated = false;
    bool sectionTruncated = false;
    bool sequenceFilenameTruncated = false;
    bool mediaFilenameTruncated = false;

    // Truncates rather than allocating. A name longer than the bound is a
    // deformed input, and losing its tail is better than doing unbounded
    // work on the callback thread. Returns true when the source had more
    // bytes than fit, so the caller can refuse to treat the copy as
    // complete.
    static bool copyField(char* dest, std::size_t capacityWithNul, const char* source) {
        if (source == nullptr) {
            dest[0] = '\0';
            return false;
        }
        std::size_t i = 0;
        for (; i + 1 < capacityWithNul && source[i] != '\0'; ++i) {
            dest[i] = source[i];
        }
        dest[i] = '\0';
        return source[i] != '\0';
    }

    void setPlaylistName(const char* s) { playlistNameTruncated = copyField(playlistName, sizeof(playlistName), s); }
    void setSection(const char* s) { sectionTruncated = copyField(section, sizeof(section), s); }
    void setSequenceFilename(const char* s) {
        sequenceFilenameTruncated = copyField(sequenceFilename, sizeof(sequenceFilename), s);
    }
    void setMediaFilename(const char* s) {
        mediaFilenameTruncated = copyField(mediaFilename, sizeof(mediaFilename), s);
    }

    // True when a field that determines entry identity was truncated.
    // sequenceFilename and mediaFilename are corroborating evidence, not
    // identity, so their truncation does not gate this.
    bool identityFieldTruncated() const { return playlistNameTruncated || sectionTruncated; }

    // Two observations describe the same playlist entry when the playlist,
    // section, and position all match. The action deliberately does not
    // participate: a repeated `playing` with a new position is item
    // advancement, and a repeated `playing` at the same position is not.
    bool sameEntryAs(const CallbackEvidence& other) const {
        return std::strcmp(playlistName, other.playlistName) == 0 && std::strcmp(section, other.section) == 0 &&
               position == other.position;
    }
};

// CallbackHandoff is the bounded queue between FPP's callback thread and
// the resident worker. It never blocks on I/O, never allocates while
// holding the lock, and never grows: when it is full the oldest pending
// observation is dropped so the newest complete state survives, and the
// drop is counted rather than hidden.
//
// The count is the gap evidence the coordinator needs. Current-state
// convergence is the invariant; replaying every transition is not, and a
// coalesced delivery must never read as a complete event history.
class CallbackHandoff {
 public:
    explicit CallbackHandoff(std::size_t capacity = 8) : capacity_(capacity == 0 ? 1 : capacity) {
        slots_.resize(capacity_);
    }

    // Called on FPP's callback thread. Bounded work only.
    void offer(const CallbackEvidence& evidence) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (size_ == capacity_) {
            head_ = (head_ + 1) % capacity_;
            --size_;
            ++coalesced_;
        }
        const std::size_t tail = (head_ + size_) % capacity_;
        slots_[tail] = evidence;
        ++size_;
        ++offered_;
    }

    // Called on the worker thread. coalescedSincePreviousTake reports how
    // many observations were dropped to make room since the last take, and
    // is cleared by this call: a caller that fails to publish must carry
    // that count forward itself, since the gap it describes has not been
    // acknowledged by anyone yet.
    bool take(CallbackEvidence* out, std::uint32_t* coalescedSincePreviousTake) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (size_ == 0) {
            if (coalescedSincePreviousTake != nullptr) *coalescedSincePreviousTake = 0;
            return false;
        }
        *out = slots_[head_];
        head_ = (head_ + 1) % capacity_;
        --size_;
        if (coalescedSincePreviousTake != nullptr) {
            *coalescedSincePreviousTake = coalesced_;
        }
        coalesced_ = 0;
        return true;
    }

    std::size_t capacity() const { return capacity_; }

    std::size_t pending() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return size_;
    }

    std::uint32_t coalescedPending() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return coalesced_;
    }

    std::uint64_t offeredTotal() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return offered_;
    }

 private:
    mutable std::mutex mutex_;
    std::vector<CallbackEvidence> slots_;
    std::size_t capacity_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint32_t coalesced_ = 0;
    std::uint64_t offered_ = 0;
};

}  // namespace showmesh
