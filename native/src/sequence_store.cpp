#include "showmesh/sequence_store.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

#include "showmesh/sha256.h"

namespace showmesh {

namespace {

constexpr const char* kEnvConfigDirOverride = "SHOWMESH_FPP_PLUGIN_CONFIG_DIR";
constexpr const char* kEnvMediaDir = "MEDIADIR";
constexpr const char* kStateDirSuffix = "plugindata/fpp-showmesh";
constexpr const char* kDefaultStateDir = "/home/fpp/media/plugindata/fpp-showmesh";
constexpr const char* kPrimaryFilename = "sequence-state";
constexpr const char* kBackupFilename = "sequence-state.bak";

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string directoryOf(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// Best-effort fsync of an already-closed file or a directory, by path.
// A failure here does not undo a rename that already landed; it only
// means the durability of that rename against a concurrent crash is
// weaker than intended, which is why writeAtomic() only requires the
// file fsync (before the rename) and the rename itself to succeed, and
// treats the directory fsync as best effort.
void fsyncPathBestEffort(const std::string& path, bool isDirectory) {
    const int fd = ::open(path.c_str(), isDirectory ? O_RDONLY : O_WRONLY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

bool fsyncFileRequired(const std::string& path) {
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

// Writes contents to path atomically: a complete write to a sibling temp
// file, fsync'd, then rename()'d over path. POSIX rename() replaces the
// destination as a single filesystem operation, so a reader never
// observes a partially written path: it sees either the previous
// complete contents or the new complete contents. The containing
// directory is then fsync'd best-effort so the rename entry itself
// survives a crash; that best-effort fsync is not required for
// writeAtomic() to report success, because the rename already landed in
// the page cache and a reader within this same boot sees it regardless.
bool writeAtomic(const std::string& path, const std::string& contents) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out) return false;
    }
    if (!fsyncFileRequired(tmp)) return false;
    if (std::rename(tmp.c_str(), path.c_str()) != 0) return false;
    fsyncPathBestEffort(directoryOf(path), true);
    return true;
}

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
};

ParsedRecord parseRecord(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return ParsedRecord{};

    std::string valueLine;
    if (!std::getline(in, valueLine)) return ParsedRecord{};
    std::string hashLine;
    if (!std::getline(in, hashLine)) return ParsedRecord{};
    std::string trailing;
    if (std::getline(in, trailing)) return ParsedRecord{};  // no extra content is a valid record

    if (valueLine.empty() || valueLine.find_first_not_of("0123456789") != std::string::npos) {
        return ParsedRecord{};
    }
    if (hashLine != sha256Hex(valueLine)) return ParsedRecord{};

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(valueLine.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') return ParsedRecord{};
    return ParsedRecord{static_cast<std::uint64_t>(parsed), true};
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
    : primaryPath_(joinPath(dir, kPrimaryFilename)), backupPath_(joinPath(dir, kBackupFilename)) {}

std::uint64_t SequenceFileStore::load() const {
    const ParsedRecord primary = parseRecord(primaryPath_);
    const ParsedRecord backup = parseRecord(backupPath_);
    std::uint64_t best = 0;
    if (primary.ok && primary.value > best) best = primary.value;
    if (backup.ok && backup.value > best) best = backup.value;
    return best;
}

bool SequenceFileStore::store(std::uint64_t value) const {
    // Enforces the same never-goes-backward rule SequenceState enforces
    // in memory, at the on-disk boundary too.
    if (value < load()) return false;

    // Rotate the current primary into the backup slot before it is
    // overwritten, so a crash during the write below still leaves a
    // valid, independently checksummed fallback. rename() is a metadata
    // operation on the same filesystem: no data is copied, so this costs
    // no extra flash write beyond the primary write itself. A rename
    // failure (for example, a permissions problem) is not fatal here: the
    // old primary is simply left in place to be overwritten by the write
    // below, which is the same outcome as never having attempted
    // rotation at all.
    const ParsedRecord existingPrimary = parseRecord(primaryPath_);
    if (existingPrimary.ok) {
        std::rename(primaryPath_.c_str(), backupPath_.c_str());
    }

    return writeAtomic(primaryPath_, encodeRecord(value));
}

}  // namespace showmesh
