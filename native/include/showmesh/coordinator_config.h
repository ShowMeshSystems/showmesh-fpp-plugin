#pragma once

#include <mutex>
#include <string>

// Where the resident worker gets the coordinator base URL and the bearer
// credential. Both resolve exactly the way the Go macro helper resolves
// them (cmd/showmesh-fpp-plugin/config.go), because an operator who
// configured one and not the other has configured half a plugin.
//
// The base URL comes from <state-dir>/config.json, the same file and the
// same key the helper reads, with the state directory resolved by
// resolveSequenceStateDir()'s precedence.
//
// The credential comes from the FIXED directory /etc/showmesh-fpp-plugin,
// never from a flag, an environment variable, or an FPP setting. That
// path is outside FPP's web root, outside the media tree, and outside the
// plugin's own git checkout, which matters because FPP's upgrade fallback
// runs `git clean -fd` over that checkout.

namespace showmesh {

// The fixed, non-configurable credential directory. The dir argument on
// the loaders below exists for this repository's own tests; nothing in an
// adapter ever passes anything else.
extern const char* const kCredentialDir;

std::string resolveCredentialDir();

struct CoordinatorUrlLoad {
    bool ok = false;
    std::string baseUrl;
    // Operator-facing reason the URL is unusable. Carries the path it
    // read, never a credential.
    std::string error;
};

// Reads and validates <stateDir>/config.json's coordinatorUrl. Absent,
// present-and-empty, and malformed are three distinct failures, each
// reported with the path, so a half-written config file reads as a
// configuration error rather than as "run against nothing".
CoordinatorUrlLoad loadCoordinatorBaseUrl(const std::string& stateDir);

struct CredentialLoad {
    bool ok = false;
    // Populated only when ok. Never logged, never written anywhere.
    std::string token;
    // Never contains the token, not even a prefix of it.
    std::string error;
};

// Enforces the same exact-0600 rule the Go helper enforces: exact, not
// "no more permissive than", because a mode this program's own installer
// did not write is a reason to distrust the file rather than a
// permission to read it.
CredentialLoad loadCoordinatorCredential(const std::string& credentialDir);

// CredentialSource hands the client a token without holding one in a
// long-lived member the rest of the runtime can reach. It re-reads the
// file after invalidate(), so an operator who fixes a wrong or expired
// credential does not have to restart fppd to get it picked up.
class CredentialSource {
 public:
    virtual ~CredentialSource() = default;
    // Returns false and fills error when no usable credential resolves.
    virtual bool token(std::string* out, std::string* error) = 0;
    virtual void invalidate() = 0;
};

class FileCredentialSource : public CredentialSource {
 public:
    explicit FileCredentialSource(std::string dir);
    FileCredentialSource() : FileCredentialSource(resolveCredentialDir()) {}

    bool token(std::string* out, std::string* error) override;
    void invalidate() override;

 private:
    std::string dir_;
    mutable std::mutex mutex_;
    std::string cached_;
    bool loaded_ = false;
};

// Joins a base URL and an absolute path with exactly one separator.
std::string joinUrlPath(const std::string& baseUrl, const std::string& path);

}  // namespace showmesh
