#include "showmesh/brightness_store.h"

#include <fstream>

#include "showmesh/atomic_write.h"
#include "showmesh/brightness_codec.h"
#include "showmesh/sha256.h"

namespace showmesh {

namespace {

constexpr const char* kPrimaryFilename = "brightness-state";
constexpr const char* kBackupFilename = "brightness-state.bak";

// encodeBrightnessState() emits compact, single-line JSON (json.cpp
// escapes any '\n' that appears inside a string value rather than
// emitting a raw newline), so this record is exactly two lines: the
// state itself and the hex SHA-256 of that line, the same shape
// sequence_store.cpp's own record uses.
std::string encodeRecord(const BrightnessState& state) {
    const std::string line = encodeBrightnessState(state);
    return line + "\n" + sha256Hex(line) + "\n";
}

struct ParsedRecord {
    BrightnessState state;
    bool ok = false;
};

bool fileExists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

ParsedRecord parseRecord(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return ParsedRecord{};

    std::string stateLine;
    if (!std::getline(in, stateLine)) return ParsedRecord{};
    std::string hashLine;
    if (!std::getline(in, hashLine)) return ParsedRecord{};
    std::string trailing;
    if (std::getline(in, trailing)) return ParsedRecord{};  // no extra content is a valid record

    if (hashLine != sha256Hex(stateLine)) return ParsedRecord{};

    BrightnessStateDecode decoded = decodeBrightnessState(stateLine);
    if (!decoded.ok) return ParsedRecord{};
    return ParsedRecord{decoded.state, true};
}

}  // namespace

BrightnessFileStore::BrightnessFileStore(std::string dir)
    : primaryPath_(joinPath(dir, kPrimaryFilename)), backupPath_(joinPath(dir, kBackupFilename)) {}

BrightnessStateLoad BrightnessFileStore::load() const {
    // Primary, not "whichever is newer": store() always rotates the prior
    // primary into the backup slot before writing a fresh one, so the
    // primary is always the most recently captured state whenever it is
    // itself checksum-valid. Falling to the backup only when the primary
    // itself fails to parse -- real corruption, since writeFileAtomically
    // never leaves a partially written primary -- rather than comparing
    // the two the way the sequence store's max() does, because there is
    // no total order across two arbitrary brightness records the way
    // there is across two counters.
    const ParsedRecord primary = parseRecord(primaryPath_);
    if (primary.ok) {
        BrightnessStateLoad result;
        result.ok = true;
        result.trustedAsCurrent = true;
        result.state = primary.state;
        return result;
    }
    // The primary failed to parse: real corruption, not a normal rotation
    // race, since writeFileAtomically never leaves a partially written
    // primary. Whatever the primary held superseded the backup, and it
    // cannot be recovered, so the backup's own numbers are not "the last
    // thing this host applied" -- they are simply the last thing before
    // that. trustedAsCurrent=false says so; it is the caller's job (see
    // BrightnessEngine::restoreFromPersisted) not to restore brighter
    // than a value it cannot actually vouch for.
    const ParsedRecord backup = parseRecord(backupPath_);
    if (backup.ok) {
        BrightnessStateLoad result;
        result.ok = true;
        result.trustedAsCurrent = false;
        result.state = backup.state;
        return result;
    }
    BrightnessStateLoad result;
    result.recordExpectedButUnreadable = fileExists(primaryPath_) || fileExists(backupPath_);
    return result;
}

bool BrightnessFileStore::store(const BrightnessState& state) const {
    const bool primaryCurrentlyValid = parseRecord(primaryPath_).ok;
    return writeWithBackupRotation(primaryPath_, backupPath_, primaryCurrentlyValid, encodeRecord(state));
}

}  // namespace showmesh
