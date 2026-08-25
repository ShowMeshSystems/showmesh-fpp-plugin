#include "showmesh/sequence_store.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "showmesh/atomic_write.h"
#include "showmesh/sha256.h"

namespace showmesh {

namespace {

constexpr const char* kEnvConfigDirOverride = "SHOWMESH_FPP_PLUGIN_CONFIG_DIR";
constexpr const char* kEnvMediaDir = "MEDIADIR";
constexpr const char* kStateDirSuffix = "plugindata/fpp-showmesh";
constexpr const char* kDefaultStateDir = "/home/fpp/media/plugindata/fpp-showmesh";
constexpr const char* kPrimaryFilename = "sequence-state";
constexpr const char* kBackupFilename = "sequence-state.bak";

// value\n followed by the hex SHA-256 of that first line, and nothing
// else. Reusing the hash already shipped for playlist identity rather
// than adding a second, weaker checksum: any single flipped byte, any
// truncation, and any extra trailing bytes from a previous longer value
// change the hash, so parseRecord() below detects all three as "not a
// valid record" rather than as a plausible, silently wrong number.
std::string encodeRecord(std::uint64_t value) {
    const std::string line = std::to_string(value);
    return line + "\n" + sha256Hex(line) + "\n";
}

struct ParsedRecord {
    std::uint64_t value = 0;
    bool ok = false;
    // Whether the file could be opened at all, independent of whether its
    // contents validated. Lets a caller tell "this file does not exist"
    // apart from "this file exists but is corrupt".
    bool present = false;
};

ParsedRecord parseRecord(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return ParsedRecord{};
    ParsedRecord result;
    result.present = true;

    std::string valueLine;
    if (!std::getline(in, valueLine)) return result;
    std::string hashLine;
    if (!std::getline(in, hashLine)) return result;
    std::string trailing;
    if (std::getline(in, trailing)) return result;  // no extra content is a valid record

    if (valueLine.empty() || valueLine.find_first_not_of("0123456789") != std::string::npos) {
        return result;
    }
    if (hashLine != sha256Hex(valueLine)) return result;

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(valueLine.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') return result;
    result.value = static_cast<std::uint64_t>(parsed);
    result.ok = true;
    return result;
}

}  // namespace

std::string resolveSequenceStateDir() {
    const char* override = std::getenv(kEnvConfigDirOverride);
    if (override != nullptr && *override != '\0') return override;

    const char* mediaDir = std::getenv(kEnvMediaDir);
    if (mediaDir != nullptr && *mediaDir != '\0') return joinPath(mediaDir, kStateDirSuffix);

    return kDefaultStateDir;
}

SequenceFileStore::SequenceFileStore(std::string dir)
    : primaryPath_(joinPath(dir, kPrimaryFilename)), backupPath_(joinPath(dir, kBackupFilename)) {
    // Best effort, once, here rather than on every store(): nothing else
    // in this repository provisions this directory (see
    // resolveSequenceStateDir()'s doc comment above), so a plugin that
    // starts before an operator or installer has created it must not
    // silently persist nothing forever. A failure here (including EEXIST)
    // is not reported: store() below already reports a directory it still
    // cannot write to, and that is the return value callers already have
    // to check.
    ::mkdir(dir.c_str(), 0755);
}

std::uint64_t SequenceFileStore::load() const { return loadDetailed().value; }

SequenceFileStore::LoadResult SequenceFileStore::loadDetailed() const {
    const ParsedRecord primary = parseRecord(primaryPath_);
    const ParsedRecord backup = parseRecord(backupPath_);
    LoadResult result;
    if (primary.ok && primary.value > result.value) result.value = primary.value;
    if (backup.ok && backup.value > result.value) result.value = backup.value;
    const bool anyPresent = primary.present || backup.present;
    const bool anyValid = primary.ok || backup.ok;
    result.filesPresentButInvalid = anyPresent && !anyValid;
    return result;
}

bool SequenceFileStore::store(std::uint64_t value) const {
    // Enforces the same never-goes-backward rule SequenceState enforces
    // in memory, at the on-disk boundary too.
    if (value < load()) return false;

    // Rotates the current primary into the backup slot (skipped when it
    // does not currently hold a valid record, so an already-invalid
    // primary never clobbers a still-valid backup) before durably writing
    // the new value. See writeWithBackupRotation in atomic_write.h, shared
    // with BrightnessFileStore.
    const bool primaryCurrentlyValid = parseRecord(primaryPath_).ok;
    return writeWithBackupRotation(primaryPath_, backupPath_, primaryCurrentlyValid, encodeRecord(value));
}

}  // namespace showmesh
