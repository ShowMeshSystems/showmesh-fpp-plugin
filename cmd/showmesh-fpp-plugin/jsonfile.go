package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

// writeJSONFile marshals v and writes it to path, atomically: it writes to
// a temp file in the same directory and renames over the target, so a
// concurrent "status" read (or FPP's own UI reading status.json to render
// a page) never observes a half-written file. Mode 0600: these files can
// carry a macro id and internal detail about this installation's
// coordinator, and nothing about them needs to be world-readable.
func writeJSONFile(path string, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return fmt.Errorf("encoding %s: %w", path, err)
	}
	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, ".tmp-*")
	if err != nil {
		return fmt.Errorf("creating temp file in %s: %w", dir, err)
	}
	tmpPath := tmp.Name()
	// Best-effort cleanup: if anything below fails before the rename, the
	// temp file is removed rather than left behind; the rename itself
	// makes this a no-op on the success path (the file no longer exists
	// under tmpPath).
	defer func() { _ = os.Remove(tmpPath) }()

	if _, err := tmp.Write(data); err != nil {
		_ = tmp.Close()
		return fmt.Errorf("writing temp file for %s: %w", path, err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("closing temp file for %s: %w", path, err)
	}
	if err := os.Chmod(tmpPath, 0o600); err != nil {
		return fmt.Errorf("setting mode on temp file for %s: %w", path, err)
	}
	if err := os.Rename(tmpPath, path); err != nil {
		return fmt.Errorf("renaming temp file into place at %s: %w", path, err)
	}
	return nil
}

// readJSONFile reads and decodes path into v. ok is false (with a nil err)
// when the file does not exist yet, which every caller in this package
// treats as "nothing recorded yet" rather than an error — the very first
// invocation of this plugin on a freshly installed host has no status
// record, no cache, and no failure buffer, and that is a normal state, not
// a fault.
func readJSONFile(path string, v any) (ok bool, err error) {
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return false, nil
		}
		return false, fmt.Errorf("reading %s: %w", path, err)
	}
	if err := json.Unmarshal(data, v); err != nil {
		return false, fmt.Errorf("parsing %s: %w", path, err)
	}
	return true, nil
}
