package main

import (
	"encoding/json"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"strings"
)

// credentialDir is the FIXED, non-configurable directory the credential
// lives in. Never a flag, never an environment variable — see doc.go's
// "the credential" section for why this is not merely a preference.
//
// It is deliberately NOT under FPP's own config directory
// (/home/fpp/media/config), and deliberately NOT under $MEDIADIR at all.
// The coordinator verified against the running bench FPP's own PHP source
// that dispatch_get('/configfile/**', 'DownloadConfigFile') resolves a
// URL-supplied filename against FPP's config directory with no allowlist,
// unauthenticated, and that dispatch_post on the same route creates
// subdirectories from a path containing a slash — so anything under
// FPP's config tree is both readable and WRITABLE by anyone who can reach
// the FPP host's HTTP port, which includes overwriting this program's own
// credential or forging status.json, the file whose entire purpose is to
// be an honest local record. /etc/showmesh-fpp-plugin is outside FPP's
// web root, outside the media tree entirely, outside the plugin's own git
// checkout (and therefore outside `git clean -fd`, which FPP 9.x runs as
// an upgrade fallback and would otherwise delete an untracked credential),
// and outside FPP's backup download, whose redaction is an exact
// key-name match list that a file named "credential" is not on.
const credentialDir = "/etc/showmesh-fpp-plugin"

// credentialDirOverride exists ONLY for this package's own tests. It is
// never read from a flag, an environment variable, or any file, and
// nothing in main.go or run.go ever sets it — it can only be assigned by
// a _test.go file in this same package, which is the only thing able to
// see an unexported package-level variable. This is a deliberate choice
// over adding an operator-facing override: the coordinator's own
// instruction was "prefer not to, since a configurable credential path is
// one more way to aim it somewhere world readable" — a Go test seam
// carries none of that risk, because there is no runtime input that can
// ever set it in a shipped binary.
var credentialDirOverride string

// resolveCredentialDir returns credentialDirOverride when a test has set
// it, or the fixed credentialDir otherwise. Never consults a flag or the
// environment.
func resolveCredentialDir() string {
	if credentialDirOverride != "" {
		return credentialDirOverride
	}
	return credentialDir
}

// requiredCredentialDirMode is the mode the credential directory itself
// should carry: owner read/write/execute only, nothing else. Unlike
// requiredCredentialMode (the FILE's mode, which refuses to run on any
// mismatch — see loadCredential), a directory mode mismatch is repaired
// in place rather than treated as a reason to refuse. The coordinator's
// own reasoning, recorded here because it corrects the obvious version:
// if the directory is already group- or world-readable, the token is
// already exposed, and refusing to run does not un-expose it — it only
// also stops the show, which is the wrong direction for a project that
// degrades toward the show continuing everywhere else. A wrong FILE mode
// is a strong signal the file is not the one this program's own
// installer wrote, and there IS a correct action available for a
// directory (fix it in place), which is not true of a file whose
// contents might not even be a real credential.
//
// This repair is affordable specifically because credentialDir is
// /etc/showmesh-fpp-plugin, on the root filesystem. FPP's own media
// directory can live on a USB stick, and on some vfat/exFAT mounts chmod
// reports success while the actual permissions come from mount options
// rather than per-file bits — a case this program's state directory
// (under $MEDIADIR) deliberately does NOT attempt this kind of repair
// for, because a strict or repair-and-trust check there would turn an
// install-time storage choice into a showtime failure. /etc is not that
// kind of filesystem, which is exactly why moving the credential there
// (see credentialDir's own doc comment) also made this repair trustworthy.
const requiredCredentialDirMode = 0o700

// credentialDirCheck is what ensureCredentialDirMode found and did, for
// the caller to fold into the local status record and stderr — a repair
// (successful or not) is a real event and must never be silent, per the
// coordinator's own instruction.
type credentialDirCheck struct {
	// Checked is false when the directory could not even be stat'd (most
	// commonly: it does not exist yet). loadCredential's own error is the
	// operator-facing signal for that case; this struct says nothing more
	// about it.
	Checked bool
	// OK is true when the directory's mode was already correct, or a
	// repair fixed it.
	OK bool
	// Repaired is true only when a chmod was attempted AND a re-stat
	// afterward confirmed the mode was actually fixed.
	Repaired bool
	// FoundMode and WantMode are populated whenever a mismatch was
	// observed, whether or not the repair succeeded.
	FoundMode os.FileMode
	WantMode  os.FileMode
	// RepairErr is set when a chmod was attempted and either failed
	// outright or a re-stat afterward still showed the wrong mode.
	RepairErr error
}

// Note reports what happened as one operator-facing sentence, or empty
// when there is nothing worth recording (already correct, or the
// directory could not be checked at all).
func (c credentialDirCheck) Note() string {
	switch {
	case !c.Checked || c.OK && !c.Repaired:
		return ""
	case c.Repaired:
		return fmt.Sprintf("the credential directory %s had mode %04o (wanted %04o); this program repaired it in place",
			resolveCredentialDir(), c.FoundMode, c.WantMode)
	default:
		return fmt.Sprintf("the credential directory %s has mode %04o (wanted %04o) and this program's attempt to repair it FAILED (%v); proceeding with the run anyway, since refusing would not un-expose a credential that is already exposed",
			resolveCredentialDir(), c.FoundMode, c.WantMode, c.RepairErr)
	}
}

// ensureCredentialDirMode checks the credential directory's own mode and,
// if it is not exactly requiredCredentialDirMode, attempts to repair it
// with chmod and re-stats to confirm. It never returns an error a caller
// is meant to refuse on — see credentialDirCheck's own doc comment and
// requiredCredentialDirMode's reasoning for why a directory mode
// mismatch is fixed in place rather than treated as a reason to stop.
func ensureCredentialDirMode() credentialDirCheck {
	dir := resolveCredentialDir()
	info, err := os.Stat(dir)
	if err != nil {
		return credentialDirCheck{Checked: false}
	}
	mode := info.Mode().Perm()
	if mode == requiredCredentialDirMode {
		return credentialDirCheck{Checked: true, OK: true}
	}

	check := credentialDirCheck{Checked: true, FoundMode: mode, WantMode: requiredCredentialDirMode}
	if err := os.Chmod(dir, requiredCredentialDirMode); err != nil {
		check.RepairErr = fmt.Errorf("chmod %s to %04o: %w", dir, requiredCredentialDirMode, err)
		return check
	}
	info2, err := os.Stat(dir)
	if err != nil {
		check.RepairErr = fmt.Errorf("re-checking %s after chmod: %w", dir, err)
		return check
	}
	if got := info2.Mode().Perm(); got != requiredCredentialDirMode {
		check.RepairErr = fmt.Errorf("chmod %s reported success but its mode is still %04o", dir, got)
		return check
	}
	check.OK = true
	check.Repaired = true
	return check
}

// defaultStateDir is the pinned on-host path for everything EXCEPT the
// credential (config.json, status.json, failures.json, macro-cache.json),
// used only when neither --config-dir, $SHOWMESH_FPP_PLUGIN_CONFIG_DIR,
// nor $MEDIADIR (see resolveConfigDir) says otherwise. Unlike the
// credential, these files carry no secret — a run's outcome, a macro id,
// a coordinator URL, buffered failure records — and FPP's own convention
// for exactly this shape of thing is a plugin's own directory under
// plugindata, which the coordinator confirmed both exists on the bench
// image and is not served by any FPP API route (unlike config).
const defaultStateDir = "/home/fpp/media/plugindata/fpp-showmesh"

// stateDirUnderMediaSuffix is appended to $MEDIADIR to build this
// plugin's state directory when FPP itself supplies that variable.
const stateDirUnderMediaSuffix = "plugindata/fpp-showmesh"

// requiredCredentialMode is the exact permission bits the credential file
// must carry. Exact, not "no more permissive than": a file this program
// did not create itself (a cloned SD card, a manual restore, an operator
// "fixing" permissions by hand) can just as easily be too RESTRICTIVE in a
// way that silently breaks reads later, or have a mode that happens to
// look safe but was never actually set by this program's own install path.
// Refusing on anything other than exactly 0600 makes the check mean "this
// is the file this program's own installer wrote," not merely "this file
// looks reasonably private."
const requiredCredentialMode = 0o600

// resolveConfigDir applies the precedence flag > $SHOWMESH_FPP_PLUGIN_CONFIG_DIR
// > $MEDIADIR-derived > pinned literal default, for the STATE directory
// only (config.json, status.json, failures.json, macro-cache.json). It
// has no bearing on the credential, which resolveCredentialDir resolves
// independently and never by flag or environment — see that function and
// doc.go.
//
// $MEDIADIR sits ahead of the pinned literal specifically because it is
// what FPP itself hands this program, not a guess about where FPP put its
// media root: when FPP forks a plugin's command it passes exactly three
// environment variables — MEDIADIR, FPPDIR, SCRIPTDIR — and nothing else
// (no PATH at all), and MEDIADIR is the authoritative media root on that
// host. Hardcoding the pinned literal ahead of a value FPP is actively
// telling this program would be wrong on any host whose media root was
// ever moved from the default, which is exactly the kind of host-specific
// fact this program has no business assuming when it is not the one
// deciding it.
func resolveConfigDir(flagValue string) string {
	if flagValue != "" {
		return flagValue
	}
	if v := os.Getenv("SHOWMESH_FPP_PLUGIN_CONFIG_DIR"); v != "" {
		return v
	}
	if mediaDir := os.Getenv("MEDIADIR"); mediaDir != "" {
		return filepath.Join(mediaDir, stateDirUnderMediaSuffix)
	}
	return defaultStateDir
}

// credentialPath is under resolveCredentialDir(), never under the state
// directory resolveConfigDir() returns. The two are intentionally
// separate function families (this one takes no argument at all) so a
// future edit cannot accidentally pass a state directory in here by
// following the pattern of the other four path helpers below.
func credentialPath() string { return filepath.Join(resolveCredentialDir(), "credential") }

func coordinatorConfigPath(stateDir string) string { return filepath.Join(stateDir, "config.json") }
func statusPath(stateDir string) string            { return filepath.Join(stateDir, "status.json") }
func failureBufferPath(stateDir string) string     { return filepath.Join(stateDir, "failures.json") }
func macroCachePath(stateDir string) string        { return filepath.Join(stateDir, "macro-cache.json") }

// loadCredential reads the bearer token from credentialPath() and
// enforces the mode-0600 rule. The token is never logged, never included
// in an error message, and never returned alongside a nil error unless the
// mode check passed — a caller cannot accidentally use a token from a file
// this function did not first verify.
func loadCredential() (string, error) {
	path := credentialPath()
	info, err := os.Stat(path)
	if err != nil {
		if os.IsNotExist(err) {
			return "", fmt.Errorf("credential file %s does not exist; this plugin has not been configured with a coordinator credential", path)
		}
		return "", fmt.Errorf("checking credential file %s: %w", path, err)
	}
	if info.IsDir() {
		return "", fmt.Errorf("credential path %s is a directory, not a file", path)
	}
	if mode := info.Mode().Perm(); mode != requiredCredentialMode {
		return "", fmt.Errorf(
			"credential file %s has mode %04o; refusing to run until it is exactly 0600 (owner read/write only, "+
				"nothing else) — a credential file readable by anything other than its owner cannot be trusted on "+
				"this host",
			path, mode)
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return "", fmt.Errorf("reading credential file %s: %w", path, err)
	}
	token := strings.TrimSpace(string(raw))
	if token == "" {
		return "", fmt.Errorf("credential file %s is empty", path)
	}
	return token, nil
}

// pluginConfig is the decoded body of <state-dir>/config.json.
// CoordinatorURL absent, present-and-empty, and present-and-null are three
// distinct decode outcomes: only a present, non-empty, well-formed
// http(s) URL is accepted, so a builder cannot accidentally treat a
// half-written config file as "run against nothing" (which net/http would
// otherwise turn into a confusing relative-URL error deep in the request
// path rather than a clear, early one here).
type pluginConfig struct {
	CoordinatorURL *string `json:"coordinatorUrl"`
}

// loadCoordinatorURL reads and validates <state-dir>/config.json,
// returning the parsed base URL.
func loadCoordinatorURL(stateDir string) (*url.URL, error) {
	path := coordinatorConfigPath(stateDir)
	raw, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, fmt.Errorf("coordinator config file %s does not exist; this plugin has not been configured with a coordinator URL", path)
		}
		return nil, fmt.Errorf("reading coordinator config file %s: %w", path, err)
	}
	var cfg pluginConfig
	if err := json.Unmarshal(raw, &cfg); err != nil {
		return nil, fmt.Errorf("parsing coordinator config file %s: %w", path, err)
	}
	if cfg.CoordinatorURL == nil {
		return nil, fmt.Errorf("coordinator config file %s has no coordinatorUrl key", path)
	}
	if *cfg.CoordinatorURL == "" {
		return nil, fmt.Errorf("coordinator config file %s has an empty coordinatorUrl", path)
	}
	u, err := url.Parse(*cfg.CoordinatorURL)
	if err != nil {
		return nil, fmt.Errorf("coordinator config file %s has an invalid coordinatorUrl %q: %w", path, *cfg.CoordinatorURL, err)
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return nil, fmt.Errorf("coordinator config file %s: coordinatorUrl %q must use http or https", path, *cfg.CoordinatorURL)
	}
	if u.Host == "" {
		return nil, fmt.Errorf("coordinator config file %s: coordinatorUrl %q has no host", path, *cfg.CoordinatorURL)
	}
	return u, nil
}
