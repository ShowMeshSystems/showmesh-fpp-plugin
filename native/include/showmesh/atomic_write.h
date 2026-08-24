#pragma once

#include <string>

// Durable whole-file replacement, shared by the sequence store and the
// local coordinator-status record. Host neutral: POSIX only, no FPP
// header.

namespace showmesh {

// Writes contents to path atomically: a complete write to a sibling temp
// file, fsync'd, then rename()'d over path. POSIX rename() replaces the
// destination as a single filesystem operation, so a reader never
// observes a partially written path: it sees either the previous
// complete contents or the new complete contents. The containing
// directory is then fsync'd best effort so the rename entry itself
// survives a crash; that best-effort fsync is not required for success,
// because the rename already landed in the page cache and a reader
// within this same boot sees it regardless.
bool writeFileAtomically(const std::string& path, const std::string& contents);

// Joins a directory and a file name with exactly one separator.
std::string joinPath(const std::string& dir, const std::string& name);

}  // namespace showmesh
