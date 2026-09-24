package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"time"
)

// Exit codes. FPP's own command path discards a script's exit status
// entirely (RES-015 section 7.2), so these exist for a human running this
// program directly — by hand on the bench, or from a shell on the FPP
// host — not as this program's primary reporting channel. The local
// status record (status.go) is that channel, and it is written
// regardless of what this program returns from main.
const (
	exitOK = 0
	// exitUsage is a bad flag or argument — this program never attempted
	// to contact anything.
	exitUsage = 1
	// exitLocalError is a pre-flight failure on this host alone: a
	// missing or wrong-mode credential file, a missing or invalid
	// coordinator config, or a filesystem error writing this program's
	// own records. Distinct from every class in section 8.2, which all
	// describe a coordinator conversation that actually happened.
	exitLocalError = 2
	// exitRefused, exitRejected, and exitUnreachable mirror section 8.2's
	// three degraded classes exactly, so a caller inspecting $? (for
	// whatever that is worth, given the paragraph above) can tell them
	// apart the same way the local status record does.
	exitRefused     = 3
	exitRejected    = 4
	exitUnreachable = 5
)

const (
	outputText = "text"
	outputJSON = "json"
)

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr, time.Now))
}

// run is main's testable core, matching this project's own
// shape: explicit stdout/stderr and an injectable clock rather than
// reaching for the globals directly.
func run(args []string, stdout, stderr io.Writer, clock func() time.Time) int {
	if len(args) == 0 {
		printTopLevelUsage(stderr)
		return exitUsage
	}

	cmd, rest := args[0], args[1:]

	switch cmd {
	case "-h", "-help", "--help", "help":
		printTopLevelUsage(stdout)
		return exitOK
	case "run":
		return cmdRun(rest, stdout, stderr, clock)
	case "night":
		return cmdNight(rest, stdout, stderr, clock)
	case "status":
		return cmdStatus(rest, stdout, stderr, clock)
	case "version":
		return cmdVersion(rest, stdout, stderr, clock)
	default:
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin: unknown command %q\n\n", cmd)
		printTopLevelUsage(stderr)
		return exitUsage
	}
}

func printTopLevelUsage(w io.Writer) {
	_, _ = fmt.Fprint(w, `showmesh-fpp-plugin runs on an FPP host, invoked by FPP's own command
mechanism, to fire a ShowMesh macro and record locally what happened —
readable on this host even when the coordinator cannot be reached.

Usage:
  showmesh-fpp-plugin <command> [flags] [args]

Commands:
  run <macroId>   submit a macro run and record the outcome locally
  night <command> send a night lifecycle command (prepare-site,
                  run-readiness, start-preshow, start-night,
                  request-final-show, fade-out-night,
                  power-down-presentation) and record the outcome
                  locally in night-status.json
  status          print the local record of the most recent run attempt
  version         show this binary's version
  help            show this help

Global flags (per-command; run "showmesh-fpp-plugin <command> --help"):
  --config-dir <dir>   override this plugin's STATE directory (config.json,
                        status.json, failures.json, macro-cache.json —
                        never the credential, see below). Resolved,
                        highest priority first: this flag, then
                        $SHOWMESH_FPP_PLUGIN_CONFIG_DIR, then
                        $MEDIADIR/plugindata/fpp-showmesh when FPP has set
                        $MEDIADIR (it does on every command this plugin
                        registers), then
                        /home/fpp/media/plugindata/fpp-showmesh as a last
                        resort.
  --output text|json   output format (default text)

The credential (a bearer token) lives at a FIXED path,
/etc/showmesh-fpp-plugin/credential, mode 0600 — not under --config-dir,
not under $MEDIADIR, and not configurable by any flag or environment
variable. This is deliberate: FPP's own config directory (where an
earlier version of this plugin kept it) is served unauthenticated over
FPP's own HTTP API with no allowlist, so a secret cannot live anywhere
under it.

Exit codes (a human's convenience only — FPP's own command path discards
a script's exit status; the local status record is the real channel):
  0  success
  1  usage error
  2  local error: this host's own credential file, config file, or
     argument was invalid before any coordinator request was attempted
  3  refused: the coordinator answered, authenticated, and declined this
     caller (401 or 403)
  4  rejected: the coordinator answered about the request itself (an
     unknown macro, a run already in flight, or similar) — not a
     credential problem
  5  unreachable: the coordinator could not be reached, or answered with
     a server error
`)
}

func flagParseExit(err error) int {
	if errors.Is(err, flag.ErrHelp) {
		return exitOK
	}
	return exitUsage
}

func printJSON(w io.Writer, v any) error {
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	return enc.Encode(v)
}
