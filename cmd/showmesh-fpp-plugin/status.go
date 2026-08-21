package main

import (
	"flag"
	"fmt"
	"io"
	"time"
)

// statusSchemaVersion guards status.json's own shape so a future field
// addition can tell an old record apart from a corrupt one. 2: added
// CredentialDirNote.
const statusSchemaVersion = 2

// Attempt classes. classOK through classUnreachable are section 8.2's four
// classes, exactly. classLocalError is this program's own addition for an
// attempt that never reached the coordinator at all — a missing or
// wrong-mode credential file, a missing or invalid coordinator config, or
// a bad macro id argument. It is kept visibly distinct from the four wire
// classes (and is never sent to the coordinator; MacroPriorFailureRequest's
// own class enum only ever carries refused/rejected/unreachable) so the
// FPP host's own record never reports a local misconfiguration as though
// the coordinator had said anything about it at all.
const (
	classOK          = "ok"
	classRefused     = "refused"
	classRejected    = "rejected"
	classUnreachable = "unreachable"
	classLocalError  = "local_error"
)

// statusRecord is the single, latest local status record this program
// writes on every attempt (section 8.3 path 1). It is overwritten each
// time, deliberately: this is "what happened most recently," read
// directly off the host with no coordinator required, which is the
// obligation this file exists to discharge. A history of attempts is a
// separate concern this record does not take on — see the failure buffer
// for what makes it TO the coordinator.
type statusRecord struct {
	SchemaVersion int       `json:"schemaVersion"`
	Timestamp     time.Time `json:"timestamp"`
	MacroID       string    `json:"macroId"`
	Class         string    `json:"class"`
	// HTTPStatus is 0 when there was no HTTP response at all (a transport
	// failure, or a local error that never made a request) — a real,
	// distinct value, not an omission standing in for "unknown".
	HTTPStatus int    `json:"httpStatus"`
	RunID      string `json:"runId,omitempty"`
	// Message is the operator-facing summary: what class this attempt
	// fell into and why, in the operator's own terms, never a repo path,
	// doc reference, ADR number, or research-record id. See
	// copy_guard_test.go, which checks this package's own source for
	// exactly that.
	Message string `json:"message"`
	// CredentialDirNote is set only when THIS attempt's startup check of
	// the credential directory's own mode (config.go's
	// ensureCredentialDirMode) found something worth recording: a repair,
	// or a failed repair attempt. Empty on every attempt where the
	// directory was already correct, or could not be checked at all —
	// see credentialDirCheck.Note. This is independent of Class: a
	// repair (or a failed one) is recorded alongside whatever this
	// attempt's own outcome was, never in place of it.
	CredentialDirNote string `json:"credentialDirNote,omitempty"`
}

func writeStatus(configDir string, rec statusRecord) error {
	rec.SchemaVersion = statusSchemaVersion
	return writeJSONFile(statusPath(configDir), rec)
}

func loadStatus(configDir string) (statusRecord, bool, error) {
	var rec statusRecord
	ok, err := readJSONFile(statusPath(configDir), &rec)
	return rec, ok, err
}

// cmdStatus implements "showmesh-fpp-plugin status": prints the local
// status record, reading nothing but this host's own filesystem. This is
// the command an FPP UI page (or an operator with a terminal and no
// working coordinator) uses to see what the last run attempt actually
// did, per section 8.3 path 1 — it makes no network call and needs no
// credential.
func cmdStatus(args []string, stdout, stderr io.Writer, _ func() time.Time) int {
	fs := flag.NewFlagSet("showmesh-fpp-plugin status", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var configDirFlag, output string
	fs.StringVar(&configDirFlag, "config-dir", "", "override this plugin's config directory")
	fs.StringVar(&output, "output", outputText, "output format: text|json")
	fs.Usage = func() {
		_, _ = fmt.Fprintln(stderr, "usage: showmesh-fpp-plugin status [flags]")
		_, _ = fmt.Fprintln(stderr, "\nPrint the local record of this plugin's most recent macro run attempt.")
		_, _ = fmt.Fprintln(stderr, "Reads only this host's own filesystem; makes no network call.")
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return flagParseExit(err)
	}
	if output != outputText && output != outputJSON {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin status: invalid --output value %q: must be text or json\n", output)
		return exitUsage
	}

	configDir := resolveConfigDir(configDirFlag)
	rec, ok, err := loadStatus(configDir)
	if err != nil {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin status: %v\n", err)
		return exitLocalError
	}
	if !ok {
		if output == outputJSON {
			_, _ = fmt.Fprintln(stdout, `{"recorded":false}`)
		} else {
			_, _ = fmt.Fprintln(stdout, "no run has been attempted yet; no status record exists")
		}
		return exitOK
	}

	if output == outputJSON {
		if err := printJSON(stdout, rec); err != nil {
			_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin status: %v\n", err)
			return exitLocalError
		}
		return exitOK
	}

	_, _ = fmt.Fprintf(stdout, "time:    %s\n", rec.Timestamp.Format(time.RFC3339))
	_, _ = fmt.Fprintf(stdout, "macro:   %s\n", rec.MacroID)
	_, _ = fmt.Fprintf(stdout, "class:   %s\n", rec.Class)
	if rec.HTTPStatus != 0 {
		_, _ = fmt.Fprintf(stdout, "http:    %d\n", rec.HTTPStatus)
	}
	if rec.RunID != "" {
		_, _ = fmt.Fprintf(stdout, "run id:  %s\n", rec.RunID)
	}
	_, _ = fmt.Fprintf(stdout, "detail:  %s\n", rec.Message)
	if rec.CredentialDirNote != "" {
		_, _ = fmt.Fprintf(stdout, "note:    %s\n", rec.CredentialDirNote)
	}
	return exitOK
}
