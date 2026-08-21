package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// withCredentialDir points resolveCredentialDir() at dir for the duration
// of the calling test, via the package-level test-only seam
// (credentialDirOverride — see config.go's doc comment on it). This is
// the ONLY way a test can avoid writing to the real, fixed
// /etc/showmesh-fpp-plugin: there is deliberately no flag or environment
// variable that does this in production.
//
// Also normalizes dir to mode 0700: t.TempDir() creates directories at
// 0755 on this project's own macOS dev environment (umask-dependent, not
// guaranteed anywhere), which is exactly the mismatch
// ensureCredentialDirMode now repairs on every run — every test using
// this helper would otherwise get an unrelated, unrequested repair note
// on stderr, which broke TestCmdRunSkipsConfigFetchWhenCachedRevisionAlreadyCurrent's
// own "stderr must be empty" assertion the first time this ran. Tests
// that specifically want to exercise the repair mechanism (see
// TestCmdRunRepairsCredentialDirMode and its sibling in this file) set up
// their own directory deliberately instead of calling this helper.
func withCredentialDir(t *testing.T, dir string) {
	t.Helper()
	// Only normalize the mode when dir actually exists: a few callers use
	// this helper purely to redirect PATH RESOLUTION (credentialPath(),
	// resolveCredentialDir()) at a directory that is never created for
	// that test — nothing there reads or writes a real file, and a
	// os.Chmod on a nonexistent path would fail those tests outright.
	if _, err := os.Stat(dir); err == nil {
		if err := os.Chmod(dir, 0o700); err != nil {
			t.Fatal(err)
		}
	}
	old := credentialDirOverride
	credentialDirOverride = dir
	t.Cleanup(func() { credentialDirOverride = old })
}

func TestResolveCredentialDirIsFixedUnlessOverriddenForTests(t *testing.T) {
	credentialDirOverride = ""
	if got := resolveCredentialDir(); got != credentialDir {
		t.Errorf("with no test override, resolveCredentialDir() = %q, want the fixed %q", got, credentialDir)
	}
	withCredentialDir(t, "/tmp/whatever-a-test-wants")
	if got := resolveCredentialDir(); got != "/tmp/whatever-a-test-wants" {
		t.Errorf("with a test override set, resolveCredentialDir() = %q, want the override value", got)
	}
}

func TestResolveConfigDirPrecedence(t *testing.T) {
	// Hermetic against whatever the real host environment happens to have
	// set — FPP's own MEDIADIR in particular, since a developer running
	// these tests on a machine that also has an FPP install nearby is not
	// impossible.
	t.Setenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR", "")
	t.Setenv("MEDIADIR", "")

	if got := resolveConfigDir(""); got != defaultStateDir {
		t.Errorf("with no flag, no env, and no $MEDIADIR, got %q, want the pinned default %q", got, defaultStateDir)
	}

	t.Setenv("MEDIADIR", "/home/fpp/media")
	wantUnderMedia := "/home/fpp/media/plugindata/fpp-showmesh"
	if got := resolveConfigDir(""); got != wantUnderMedia {
		t.Errorf("with $MEDIADIR set and nothing higher-priority, got %q, want %q (derived under MEDIADIR)", got, wantUnderMedia)
	}

	t.Setenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR", "/env/dir")
	if got := resolveConfigDir(""); got != "/env/dir" {
		t.Errorf("with both $MEDIADIR and $SHOWMESH_FPP_PLUGIN_CONFIG_DIR set, got %q, want /env/dir (the plugin-specific env var must win over $MEDIADIR)", got)
	}

	if got := resolveConfigDir("/flag/dir"); got != "/flag/dir" {
		t.Errorf("with a flag value, got %q, want /flag/dir (the flag must win over everything else)", got)
	}
}

// TestResolveConfigDirMediaDirUsesAnArbitraryRoot is the property that
// justifies preferring $MEDIADIR over the pinned literal at all: a host
// whose media root was moved off /home/fpp/media entirely still resolves
// correctly, because this program reads what FPP itself hands it rather
// than assuming the default.
func TestResolveConfigDirMediaDirUsesAnArbitraryRoot(t *testing.T) {
	t.Setenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR", "")
	t.Setenv("MEDIADIR", "/mnt/usb-media")
	want := "/mnt/usb-media/plugindata/fpp-showmesh"
	if got := resolveConfigDir(""); got != want {
		t.Errorf("got %q, want %q", got, want)
	}
}

// TestStateDirAndCredentialDirAreIndependent pins the point of the whole
// finding this test file was rewritten for: resolveConfigDir (the state
// directory: config.json, status.json, failures.json, macro-cache.json)
// and resolveCredentialDir (the credential, fixed outside FPP's tree
// entirely) must never resolve to the same root, and setting one must
// never move the other.
func TestStateDirAndCredentialDirAreIndependent(t *testing.T) {
	t.Setenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR", "/some/state/dir")
	credentialDirOverride = ""

	stateDir := resolveConfigDir("")
	credDir := resolveCredentialDir()

	if stateDir == credDir {
		t.Fatalf("state dir and credential dir both resolved to %q; they must be independent roots", stateDir)
	}
	if credDir != credentialDir {
		t.Errorf("setting the STATE dir env var moved the credential dir to %q, want the fixed %q untouched", credDir, credentialDir)
	}
}

func TestLoadCredentialRefusesWrongMode(t *testing.T) {
	dir := t.TempDir()
	withCredentialDir(t, dir)
	path := credentialPath()
	if err := os.WriteFile(path, []byte("secret-token\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	_, err := loadCredential()
	if err == nil {
		t.Fatal("expected an error for a 0644 credential file, got nil")
	}
	// This test's own name is a claim: verify it actually distinguishes
	// mode from every other possible failure by confirming the SAME file,
	// re-chmod'd to 0600, succeeds.
	if err := os.Chmod(path, 0o600); err != nil {
		t.Fatal(err)
	}
	token, err := loadCredential()
	if err != nil {
		t.Fatalf("expected success once mode is 0600, got %v", err)
	}
	if token != "secret-token" {
		t.Errorf("token = %q, want %q (whitespace must be trimmed)", token, "secret-token")
	}
}

func TestLoadCredentialRefusesTooRestrictiveMode(t *testing.T) {
	// The rule is EXACT equality with 0600, not "no more permissive than"
	// — a file this program did not itself write should not be trusted
	// merely because it happens to be private, per config.go's own doc
	// comment. 0400 is more restrictive than 0600 and must still be
	// refused.
	dir := t.TempDir()
	withCredentialDir(t, dir)
	path := credentialPath()
	if err := os.WriteFile(path, []byte("secret-token"), 0o400); err != nil {
		t.Fatal(err)
	}
	if _, err := loadCredential(); err == nil {
		t.Fatal("expected an error for a 0400 credential file (stricter than 0600 is still refused), got nil")
	}
}

func TestLoadCredentialMissingFile(t *testing.T) {
	withCredentialDir(t, t.TempDir())
	if _, err := loadCredential(); err == nil {
		t.Fatal("expected an error for a missing credential file, got nil")
	}
}

func TestLoadCredentialEmptyFile(t *testing.T) {
	dir := t.TempDir()
	withCredentialDir(t, dir)
	if err := os.WriteFile(credentialPath(), []byte("   \n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := loadCredential(); err == nil {
		t.Fatal("expected an error for a whitespace-only credential file, got nil")
	}
}

func TestLoadCoordinatorURLAbsentNullEmptyAreDistinct(t *testing.T) {
	// "Absent, null and explicitly empty are three different things on
	// every field": exercised on this program's own inbound decode of
	// config.json, which is exactly what this test asserts stays true.
	cases := []struct {
		name string
		body string
	}{
		{"missing file", ""},
		{"absent key", `{}`},
		{"null value", `{"coordinatorUrl": null}`},
		{"empty string", `{"coordinatorUrl": ""}`},
		{"not http/https", `{"coordinatorUrl": "ftp://example.com"}`},
		{"no host", `{"coordinatorUrl": "http://"}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			if tc.body != "" {
				if err := os.WriteFile(coordinatorConfigPath(dir), []byte(tc.body), 0o600); err != nil {
					t.Fatal(err)
				}
			}
			if _, err := loadCoordinatorURL(dir); err == nil {
				t.Errorf("case %q: expected an error, got nil", tc.name)
			}
		})
	}
}

func TestLoadCoordinatorURLValid(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(coordinatorConfigPath(dir), []byte(`{"coordinatorUrl": "http://coordinator.local:8080"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	u, err := loadCoordinatorURL(dir)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if u.String() != "http://coordinator.local:8080" {
		t.Errorf("got %q, want http://coordinator.local:8080", u.String())
	}
}

func TestConfigPathsAreUnderStateDir(t *testing.T) {
	dir := "/some/dir"
	for _, p := range []string{
		coordinatorConfigPath(dir),
		statusPath(dir),
		failureBufferPath(dir),
		macroCachePath(dir),
	} {
		if filepath.Dir(p) != dir {
			t.Errorf("path %q is not directly under %q", p, dir)
		}
	}
}

func TestCredentialPathIsUnderCredentialDirNeverStateDir(t *testing.T) {
	withCredentialDir(t, "/etc/showmesh-fpp-plugin-test")
	got := credentialPath()
	want := "/etc/showmesh-fpp-plugin-test/credential"
	if got != want {
		t.Errorf("credentialPath() = %q, want %q", got, want)
	}
}

// setCredentialDirOverrideRaw points credentialDirOverride at dir WITHOUT
// withCredentialDir's own mode normalization — the whole point of these
// tests is to exercise ensureCredentialDirMode against a directory whose
// mode is deliberately wrong, which withCredentialDir would silently fix
// before this program's own code ever saw it.
func setCredentialDirOverrideRaw(t *testing.T, dir string) {
	t.Helper()
	old := credentialDirOverride
	credentialDirOverride = dir
	t.Cleanup(func() { credentialDirOverride = old })
}

func TestEnsureCredentialDirModeAlreadyCorrect(t *testing.T) {
	dir := t.TempDir()
	if err := os.Chmod(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	setCredentialDirOverrideRaw(t, dir)

	check := ensureCredentialDirMode()
	if !check.Checked || !check.OK || check.Repaired {
		t.Errorf("check = %+v, want Checked=true OK=true Repaired=false", check)
	}
	if note := check.Note(); note != "" {
		t.Errorf("Note() = %q, want empty when the directory was already correct", note)
	}
}

// TestEnsureCredentialDirModeRepairsWrongMode is this finding's own
// acceptance property: a directory mode that is not 0700 is fixed IN
// PLACE, not refused. Verified against the real filesystem, not a mock —
// chmod's own success is what this program is trusting, so the test
// checks the directory's mode afterward rather than only trusting the
// returned struct.
func TestEnsureCredentialDirModeRepairsWrongMode(t *testing.T) {
	dir := t.TempDir()
	if err := os.Chmod(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	setCredentialDirOverrideRaw(t, dir)

	check := ensureCredentialDirMode()
	if !check.Checked || !check.OK || !check.Repaired {
		t.Fatalf("check = %+v, want Checked=true OK=true Repaired=true", check)
	}
	if check.FoundMode != 0o755 || check.WantMode != 0o700 {
		t.Errorf("check = %+v, want FoundMode=0755 WantMode=0700", check)
	}
	if check.RepairErr != nil {
		t.Errorf("RepairErr = %v, want nil on a successful repair", check.RepairErr)
	}

	info, err := os.Stat(dir)
	if err != nil {
		t.Fatal(err)
	}
	if got := info.Mode().Perm(); got != 0o700 {
		t.Errorf("directory mode after repair = %04o, want 0700", got)
	}

	note := check.Note()
	if !strings.Contains(note, "repaired") {
		t.Errorf("Note() = %q, want it to say a repair happened", note)
	}
	if !strings.Contains(note, "0755") || !strings.Contains(note, "0700") {
		t.Errorf("Note() = %q, want it to name both the mode found and the mode wanted", note)
	}
}

func TestEnsureCredentialDirModeMissingDirectory(t *testing.T) {
	setCredentialDirOverrideRaw(t, filepath.Join(t.TempDir(), "does-not-exist"))

	check := ensureCredentialDirMode()
	if check.Checked {
		t.Errorf("check = %+v, want Checked=false for a directory that does not exist", check)
	}
	if note := check.Note(); note != "" {
		t.Errorf("Note() = %q, want empty when the directory could not be checked at all", note)
	}
}

// TestCredentialDirCheckNoteForFailedRepair exercises Note()'s failed-
// repair branch directly against a constructed credentialDirCheck: a
// portable, reliable chmod FAILURE (as opposed to a successful chmod on
// a directory this test process owns, which is all t.TempDir() can ever
// produce) isn't reproducible across platforms without root tricks, so
// this is the honest way to prove that branch's own text is correct —
// see TestCmdRunRepairsCredentialDirModeAndProceeds for the success path
// proven end to end instead.
func TestCredentialDirCheckNoteForFailedRepair(t *testing.T) {
	check := credentialDirCheck{
		Checked: true, FoundMode: 0o755, WantMode: 0o700,
		RepairErr: errors.New("permission denied"),
	}
	note := check.Note()
	if !strings.Contains(note, "FAILED") {
		t.Errorf("Note() = %q, want it to say the repair failed", note)
	}
	if !strings.Contains(note, "permission denied") {
		t.Errorf("Note() = %q, want it to carry the underlying repair error", note)
	}
	if !strings.Contains(note, "0755") || !strings.Contains(note, "0700") {
		t.Errorf("Note() = %q, want it to name both the mode found and the mode wanted", note)
	}
	if !strings.Contains(note, "proceeding") {
		t.Errorf("Note() = %q, want it to say the run proceeds anyway", note)
	}
}
