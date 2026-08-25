#pragma once

#include <cstdint>
#include <string>

// The on-disk half of SequenceState (playlist_identity.h). Host neutral:
// it reaches for POSIX file APIs, never an FPP header, so it builds and is
// unit tested the same way the rest of native/src is.

namespace showmesh {

// Resolves the directory the sequence store lives in. Same precedence and
// same two environment variables as the Go macro helper's state directory
// (cmd/showmesh-fpp-plugin/config.go's resolveConfigDir): a plugin
// setting is not used here because FPP's settings store is reached
// through adapter-only headers this core must not include, and an
// operator who already redirected the helper's state almost certainly
// wants the resident worker's state to follow it rather than silently
// diverge. There is no command-line flag, unlike the helper: this runs
// inside fppd's own process, not as a separately invoked program.
//
// This directory is never /etc/showmesh-fpp-plugin. That path is fixed
// and reserved for the coordinator credential alone (see the Go helper's
// credentialDir); reusing it for non-secret sequence state would put an
// unrelated file under a directory whose whole point is to hold exactly
// one sensitive thing.
std::string resolveSequenceStateDir();

// SequenceFileStore persists a single monotonically increasing value
// (the per-instance event sequence) across process restarts, durably
// enough that a restarted plugin never resumes below the highest value a
// completed store() call ever wrote, the exact property the coordinator's
// monotonicity check depends on.
//
// Two files back every instance: a primary and a backup. store() rotates
// the previous primary into the backup slot (a metadata-only rename, not
// a data copy) before writing the new primary, so a crash mid-write to
// the primary still leaves a complete, independently checksummed backup
// in place. load() trusts only a file whose checksum still matches and
// returns the higher of the two, so neither a truncated file nor
// arbitrary bit corruption can produce a value that regresses past the
// last value a completed store() actually persisted. When neither file
// validates, load() returns 0, the same value a brand-new SequenceState
// already starts at; loadDetailed() below distinguishes that case from a
// genuine first run, where 0 is correct, because the two are not the
// same condition.
class SequenceFileStore {
 public:
    // Attempts to create dir if it does not already exist (mode 0755;
    // this state carries no secret, unlike the Go helper's credential
    // directory, so it is not given that directory's restrictive 0700).
    // The attempt is best effort: a missing parent, or a dir this process
    // still cannot write to for some other reason, is left for store()
    // and load() to report through their own return values, exactly as
    // if this constructor had never tried.
    explicit SequenceFileStore(std::string dir);

    std::uint64_t load() const;

    // The result of load(), plus whether the on-disk state at load time
    // could be told apart from a genuine first run.
    struct LoadResult {
        std::uint64_t value = 0;
        // True when the primary or backup file existed on disk but
        // neither one validated: a value was certainly stored before
        // restart, its height is simply unknown, unlike a genuine first
        // run where neither file exists yet. value is 0 in this case too
        // (there is no safe higher number to report), so a caller that
        // only reads value cannot tell the two apart; a caller that
        // needs to must check this flag instead.
        bool filesPresentButInvalid = false;
    };
    LoadResult loadDetailed() const;

    // Refuses to persist a value lower than what load() would currently
    // return, so the on-disk store enforces the same never-goes-backward
    // rule SequenceState already enforces in memory; a caller racing two
    // stores, or replaying an older value by mistake, cannot silently
    // regress the file. Returns true only once the new value is durable
    // (fsync'd, visible under its final name); returns false, leaving the
    // previously durable value untouched, on any refusal or failure.
    bool store(std::uint64_t value) const;

 private:
    std::string primaryPath_;
    std::string backupPath_;
};

}  // namespace showmesh
