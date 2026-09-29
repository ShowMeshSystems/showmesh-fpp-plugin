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

// Reads path whole into *contents. Returns false, leaving *contents
// untouched, when path does not exist or cannot be opened; a missing
// record is the normal first-run case, not an error a caller need report.
bool readFileWhole(const std::string& path, std::string* contents);

// Joins a directory and a file name with exactly one separator.
std::string joinPath(const std::string& dir, const std::string& name);

// Rotates any existing, valid primary into the backup slot (a metadata-
// only rename: no data is copied) and then durably writes newContents to
// the primary via writeFileAtomically. Shared by every primary/backup
// store in this repository that validates its own record with a per-file
// checksum (SequenceFileStore, BrightnessFileStore): rotation is skipped
// when primaryIsCurrentlyValid is false, so a primary that is already
// invalid can never clobber a still-valid backup with garbage. The
// caller determines validity itself, in whatever format its own records
// use; this function knows nothing about record formats.
bool writeWithBackupRotation(const std::string& primaryPath, const std::string& backupPath,
                              bool primaryIsCurrentlyValid, const std::string& newContents);

}  // namespace showmesh
