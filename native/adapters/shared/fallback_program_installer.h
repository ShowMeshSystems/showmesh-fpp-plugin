#pragma once

// Installs a verified ADR-048 fallback program so it survives an fppd
// restart: it must be the last known good program on disk, never a
// truncated one, if a write fails partway.
//
// Reuses showmesh::writeFileAtomically (native/include/showmesh/atomic_
// write.h), the identical temp-file-plus-fsync-plus-rename primitive
// BrightnessFileStore and SequenceFileStore already use, rather than a
// second atomic-write implementation. That function writes a sibling
// "<path>.tmp" file, fsyncs it, and rename()s it over path; rename() on
// POSIX either fully replaces the destination or fails, so the previous
// file at path is never observed half-written. If the open, the write,
// or the fsync fails (a full disk, a permissions problem, the directory
// not existing), writeFileAtomically returns false before ever calling
// rename(), so the previous program at path is untouched: this function
// turns that false into a stated refusal, never a silent no-op and never
// a directory this function created on its own.
//
// This file does not verify. It does not need to trust a caller's word
// that VerifyFallbackProgram already ran: VerifiedFallbackProgram's own
// private constructor makes that structural, not a convention this file
// has to take on faith.

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

#include "fallback_program_verifier.h"
#include "showmesh/atomic_write.h"

namespace showmesh {
namespace fallback {

// ADR-048 section 1's reported fields for one installed program: package
// id, revision, verification result, and installed time. Age is derived
// from installedAt at report time, not stored, the identical "derive at
// read time, never at write time" rule schemaV25's own doc comment
// states for the coordinator side of this record.
struct InstalledFallbackProgramReport {
    std::string packageId;
    std::string revision;
    std::string verificationResult;  // "accepted" once installed; a refusal never reaches this struct
    std::chrono::system_clock::time_point installedAt;
};

struct InstallResult {
    bool ok = false;
    std::string refusalReason;  // populated only when !ok
    InstalledFallbackProgramReport report;
};

// The FPP plugin-data directory FPP creates for this plugin
// (fpp-showmesh/pluginInfo.json's repoName is "fpp-showmesh"), the
// identical location every other installed FPP plugin's own persisted
// data already lives under, rather than a path under this plugin's
// source or config tree.
inline const char* DefaultFallbackProgramPath() {
    return "/home/fpp/media/plugindata/fpp-showmesh/fallback-program.json";
}

// Atomically installs verified's document as the last known good
// fallback program at path. verified can only exist because
// VerifyFallbackProgram's signature check accepted it (see that type's
// own doc comment), so this function never needs its own opinion about
// whether the content is trustworthy. Never creates path's directory: a
// missing directory, or one that exists but is not writable, is a
// stated refusal, not a silently created tree and not a silent no-op.
inline InstallResult InstallFallbackProgram(const VerifiedFallbackProgram& verified, const std::string& path) {
    InstallResult result;

    if (!showmesh::writeFileAtomically(path, verified.rawDocument())) {
        result.refusalReason = "fallback: could not durably install fallback program at " + path +
                                " (directory missing, not writable, or write failed; the previous program there, "
                                "if any, is unchanged)";
        return result;
    }

    result.ok = true;
    result.report.packageId = verified.packageId();
    result.report.revision = verified.revision();
    result.report.verificationResult = "accepted";
    result.report.installedAt = std::chrono::system_clock::now();
    return result;
}

// Reads back the last known good fallback program from path, for restart
// survival: a fresh process calls this after a restart to recover
// exactly the document InstallFallbackProgram most recently wrote, then
// re-verifies it with VerifyFallbackProgram before trusting it. Disk
// content is never treated as pre-verified.
struct ReadInstalledResult {
    bool ok = false;
    std::string error;
    std::string rawDocument;
};

inline ReadInstalledResult ReadInstalledFallbackProgram(const std::string& path) {
    ReadInstalledResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "fallback: could not open installed fallback program at " + path;
        return result;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        result.error = "fallback: could not read installed fallback program at " + path;
        return result;
    }
    result.ok = true;
    result.rawDocument = buffer.str();
    return result;
}

}  // namespace fallback
}  // namespace showmesh
