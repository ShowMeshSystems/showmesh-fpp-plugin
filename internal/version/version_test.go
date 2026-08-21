package version

import (
	"strings"
	"testing"
)

// These tests deliberately avoid asserting the package-level default values
// ("dev", "none", "unknown"): a release build sets Version/Commit/BuildDate
// via -ldflags -X before go test even runs, and a test that hardcodes the
// unversioned defaults would fail on every release build for no good
// reason. What must hold regardless of ldflags is that String() reflects
// whatever the current values are, and that it does not panic or produce
// nonsense when a value is empty.

func TestStringIncludesCurrentValues(t *testing.T) {
	origVersion, origCommit, origBuildDate := Version, Commit, BuildDate
	t.Cleanup(func() {
		Version, Commit, BuildDate = origVersion, origCommit, origBuildDate
	})

	Version = "1.2.3"
	Commit = "abc1234"
	BuildDate = "2026-08-10T00:00:00Z"

	s := String()

	for _, want := range []string{"1.2.3", "abc1234", "2026-08-10T00:00:00Z"} {
		if !strings.Contains(s, want) {
			t.Errorf("String() = %q, want it to contain %q", s, want)
		}
	}
}

func TestStringHandlesEmptyValues(t *testing.T) {
	origVersion, origCommit, origBuildDate := Version, Commit, BuildDate
	t.Cleanup(func() {
		Version, Commit, BuildDate = origVersion, origCommit, origBuildDate
	})

	Version, Commit, BuildDate = "", "", ""

	s := String()

	if s == "" {
		t.Errorf("String() = %q, want a non-empty summary even with empty fields", s)
	}
}

func TestStringReflectsWhateverIsCurrentlySet(t *testing.T) {
	// Whatever the build actually set (ldflags in a release build, the
	// package defaults in a local `go run`/`go test`), String() must
	// surface it faithfully rather than some other hardcoded value.
	s := String()

	for _, want := range []string{Version, Commit, BuildDate} {
		if !strings.Contains(s, want) {
			t.Errorf("String() = %q, want it to contain the current value %q", s, want)
		}
	}
}
