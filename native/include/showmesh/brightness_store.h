#pragma once

#include <string>

#include "showmesh/brightness.h"

// The on-disk half of BrightnessEngine's full state. Host neutral, in the
// same sense as sequence_store.h: POSIX file APIs only, never an FPP
// header, so it builds and is unit tested the same way the rest of
// native/src is.

namespace showmesh {

struct BrightnessStateLoad {
    bool ok = false;
    BrightnessState state;
    // True only when state came from the primary record, the most
    // recent one this store ever wrote. False when it was recovered
    // from the backup after the primary failed to parse: the backup is
    // by construction the state the primary superseded, so its numbers
    // can be brighter than whatever this host actually applied after it
    // was written. A caller must not treat trustedAsCurrent=false as
    // "the last thing this host applied" -- see
    // BrightnessEngine::restoreFromPersisted.
    bool trustedAsCurrent = false;
    // True when a primary or backup file exists on disk even though
    // load() still could not produce a valid record (ok=false).
    // Distinguishes "nothing has ever been written here", where the
    // engine's own built-in defaults are correct, from "something was
    // written and none of it can be trusted", where the last applied
    // value is unknown.
    bool recordExpectedButUnreadable = false;
};

// BrightnessFileStore persists BrightnessEngine's captureState() output
// across process restarts, using exactly the durability shape
// SequenceFileStore uses (see sequence_store.h): a primary and a backup
// file, the previous primary rotated into the backup slot before a new
// primary is written, a per-file checksum, load() trusting only a
// checksum-valid, decodable record. It lives in the same directory as the
// sequence store (resolveSequenceStateDir(), sequence_store.h), under
// different filenames, so an operator who redirects one redirects both.
//
// The safety rule intentionally differs from the sequence store's. The
// sequence store's load() refuses to let a restarted process resume below
// a value it already durably issued: "the higher value always wins" is
// safe there because the sequence is a single monotonic counter and
// nothing else about it matters. Brightness has no such total order
// across a restart -- what must never happen is a restart applying a
// BRIGHTER value than what this host had actually applied to its
// outputs, and "brighter" is not simply "the other record" the way a
// smaller sequence number is. This store makes no attempt to arbitrate
// that itself: store() always durably records whatever the engine's live
// state actually is, the same way encodeFullState() always describes it
// to a MultiSync peer without judging it. The "never brighter" guarantee
// is entirely BrightnessEngine::restoreFromPersisted's job (brightness.h):
// it already settles on the darker of the recorded target and the
// recorded last-applied value whenever the persisted fade timing cannot
// be trusted. This store's obligation is to hand restoreFromPersisted a
// payload it can trust byte-for-byte, or to say plainly that it cannot --
// via trustedAsCurrent and recordExpectedButUnreadable on
// BrightnessStateLoad -- rather than silently handing over the backup's
// superseded numbers as if they were current.
class BrightnessFileStore {
 public:
    // dir is used as-is; it is not created here, matching
    // SequenceFileStore's contract.
    explicit BrightnessFileStore(std::string dir);

    // Returns ok=false when neither file holds a checksum-valid,
    // decodable record. On the very first run that means recordExpected-
    // ButUnreadable is also false: nothing was ever written, which for an
    // engine that has not been constructed yet is indistinguishable from
    // restoring its own built-in defaults. On every later run where a
    // file exists but nothing in it can be trusted,
    // recordExpectedButUnreadable is true instead: some state was once
    // durable here, it is now unrecoverable, and a caller must not infer
    // the engine's bright built-in defaults are safe to use in its place.
    // A caller also cannot fully trust an ok=true result on its own: see
    // trustedAsCurrent.
    BrightnessStateLoad load() const;

    // Always writes: a fresh capture always describes what the engine's
    // live state is right now, so there is no lower bound to refuse
    // regressing past the way SequenceFileStore::store has one. Returns
    // true once durable (fsync'd, visible under its final name).
    bool store(const BrightnessState& state) const;

 private:
    std::string primaryPath_;
    std::string backupPath_;
};

}  // namespace showmesh
