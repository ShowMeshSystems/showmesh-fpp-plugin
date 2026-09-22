#include "showmesh/coordinator_config.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

#include "showmesh/atomic_write.h"
#include "showmesh/json.h"

namespace showmesh {

const char* const kCredentialDir = "/etc/showmesh-fpp-plugin";

namespace {

constexpr const char* kConfigFilename = "config.json";
constexpr const char* kCredentialFilename = "credential";
// Exact bits, not a maximum. See the header.
constexpr ::mode_t kRequiredCredentialMode = 0600;

std::string trimSpace(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) --end;
    return s.substr(begin, end - begin);
}

bool readWholeFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream contents;
    contents << in.rdbuf();
    *out = contents.str();
    return true;
}

// A deliberately narrow check rather than a URL parser: the scheme must
// be http or https and the authority must be non-empty. Anything the
// coordinator's own routing cares about beyond that is its business.
bool urlHasSchemeAndHost(const std::string& url, std::string* error) {
    std::size_t authority = 0;
    if (url.rfind("http://", 0) == 0) {
        authority = 7;
    } else if (url.rfind("https://", 0) == 0) {
        authority = 8;
    } else {
        *error = "must use http or https";
        return false;
    }
    const std::size_t end = url.find('/', authority);
    const std::string host = url.substr(authority, end == std::string::npos ? std::string::npos : end - authority);
    if (host.empty()) {
        *error = "has no host";
        return false;
    }
    return true;
}

}  // namespace

std::string resolveCredentialDir() { return kCredentialDir; }

std::string joinUrlPath(const std::string& baseUrl, const std::string& path) {
    if (baseUrl.empty()) return path;
    const bool baseEndsWithSlash = baseUrl.back() == '/';
    const bool pathStartsWithSlash = !path.empty() && path.front() == '/';
    if (baseEndsWithSlash && pathStartsWithSlash) return baseUrl.substr(0, baseUrl.size() - 1) + path;
    if (!baseEndsWithSlash && !pathStartsWithSlash) return baseUrl + "/" + path;
    return baseUrl + path;
}

CoordinatorUrlLoad loadCoordinatorBaseUrl(const std::string& stateDir) {
    CoordinatorUrlLoad load;
    const std::string path = joinPath(stateDir, kConfigFilename);

    std::string raw;
    if (!readWholeFile(path, &raw)) {
        load.error = "coordinator config file " + path +
                     " could not be read; this plugin has not been configured with a coordinator URL";
        return load;
    }

    json::ParseResult parsed = json::parse(raw);
    if (!parsed.ok || parsed.value.type() != json::Type::kObject) {
        load.error = "coordinator config file " + path + " is not a JSON object";
        return load;
    }

    for (const json::Value::Member& member : parsed.value.members()) {
        if (member.first != "coordinatorUrl") continue;
        if (member.second.type() != json::Type::kString) {
            load.error = "coordinator config file " + path + " has a non-string coordinatorUrl";
            return load;
        }
        const std::string url = trimSpace(member.second.string());
        if (url.empty()) {
            load.error = "coordinator config file " + path + " has an empty coordinatorUrl";
            return load;
        }
        std::string why;
        if (!urlHasSchemeAndHost(url, &why)) {
            load.error = "coordinator config file " + path + ": coordinatorUrl " + why;
            return load;
        }
        load.ok = true;
        load.baseUrl = url;
        return load;
    }

    load.error = "coordinator config file " + path + " has no coordinatorUrl key";
    return load;
}

CredentialLoad loadCoordinatorCredential(const std::string& credentialDir) {
    CredentialLoad load;
    const std::string path = joinPath(credentialDir, kCredentialFilename);

    struct ::stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        load.error = "credential file " + path +
                     " does not exist or cannot be read; this plugin has not been configured with a coordinator "
                     "credential";
        return load;
    }
    if (!S_ISREG(info.st_mode)) {
        load.error = "credential path " + path + " is not a regular file";
        return load;
    }
    const ::mode_t mode = info.st_mode & 07777;
    if (mode != kRequiredCredentialMode) {
        char found[8] = {0};
        std::snprintf(found, sizeof(found), "%04o", static_cast<unsigned>(mode));
        load.error = "credential file " + path + " has mode " + found +
                     "; refusing to use it until it is exactly 0600 (owner read/write only)";
        return load;
    }

    std::string raw;
    if (!readWholeFile(path, &raw)) {
        load.error = "credential file " + path + " could not be read";
        return load;
    }
    const std::string token = trimSpace(raw);
    if (token.empty()) {
        load.error = "credential file " + path + " is empty";
        return load;
    }
    load.ok = true;
    load.token = token;
    return load;
}

bool writeCoordinatorCredentialAtomically(const std::string& credentialDir, const std::string& token,
                                          std::string* error) {
    if (::mkdir(credentialDir.c_str(), 0700) != 0 && errno != EEXIST) {
        if (error != nullptr) *error = "could not create " + credentialDir + ": " + std::strerror(errno);
        return false;
    }
    // Forced regardless of whether this call created the directory: a
    // directory this program did not create itself is not trusted to
    // already carry the right mode.
    if (::chmod(credentialDir.c_str(), 0700) != 0) {
        if (error != nullptr) *error = "could not set the required mode on " + credentialDir;
        return false;
    }

    const std::string path = joinPath(credentialDir, kCredentialFilename);
    const std::string tmp = path + ".tmp";
    // Created at exactly the required mode from the instant it exists,
    // rather than world-or-group-readable under the process umask until a
    // chmod() lands after the write: a reader racing this open() (a
    // scheduler or an operator's own `ls`) must never be able to observe
    // the token under a more permissive mode than the finished file ever
    // carries. O_TRUNC in case a previous attempt's temp file was left
    // behind; a leftover has never been trusted content either way.
    const int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, kRequiredCredentialMode);
    if (fd < 0) {
        if (error != nullptr) *error = "could not create a temp file in " + credentialDir;
        return false;
    }
    bool writeOk = true;
    std::size_t written = 0;
    while (written < token.size()) {
        const ::ssize_t n = ::write(fd, token.data() + written, token.size() - written);
        if (n <= 0) {
            writeOk = false;
            break;
        }
        written += static_cast<std::size_t>(n);
    }
    if (writeOk) writeOk = ::fsync(fd) == 0;
    ::close(fd);
    if (!writeOk) {
        if (error != nullptr) *error = "could not write the credential temp file";
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (error != nullptr) *error = "could not install the credential file";
        std::remove(tmp.c_str());
        return false;
    }
    const int dirFd = ::open(credentialDir.c_str(), O_RDONLY);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }
    return true;
}

FileCredentialSource::FileCredentialSource(std::string dir) : dir_(std::move(dir)) {}

bool FileCredentialSource::token(std::string* out, std::string* error) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (loaded_) {
        *out = cached_;
        return true;
    }
    CredentialLoad load = loadCoordinatorCredential(dir_);
    if (!load.ok) {
        if (error != nullptr) *error = load.error;
        return false;
    }
    cached_ = load.token;
    loaded_ = true;
    *out = cached_;
    return true;
}

void FileCredentialSource::invalidate() {
    std::lock_guard<std::mutex> guard(mutex_);
    // Overwritten rather than merely cleared, so the previous token does
    // not linger in this object's storage after it stops being used.
    cached_.assign(cached_.size(), '\0');
    cached_.clear();
    loaded_ = false;
}

}  // namespace showmesh
