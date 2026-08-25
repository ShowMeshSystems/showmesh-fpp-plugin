#include "showmesh/atomic_write.h"

#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

namespace showmesh {

namespace {

std::string directoryOf(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// Best-effort fsync of an already-closed file or a directory, by path. A
// failure here does not undo a rename that already landed; it only means
// the durability of that rename against a concurrent crash is weaker
// than intended.
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

}  // namespace

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

bool writeFileAtomically(const std::string& path, const std::string& contents) {
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

bool writeWithBackupRotation(const std::string& primaryPath, const std::string& backupPath,
                              bool primaryIsCurrentlyValid, const std::string& newContents) {
    // A rename failure here (for example, a permissions problem) is not
    // fatal: the old primary is simply left in place to be overwritten by
    // the write below, the same outcome as never having attempted
    // rotation at all.
    if (primaryIsCurrentlyValid) {
        std::rename(primaryPath.c_str(), backupPath.c_str());
    }
    return writeFileAtomically(primaryPath, newContents);
}

}  // namespace showmesh
