package main

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// nightCommands are the lifecycle commands an FPP schedule may invoke.
// Operator recovery commands (end-session, resume-show) are deliberately
// absent: they are not calendar intents and stay with the operator.
var nightCommands = []string{
	"prepare-site",
	"run-readiness",
	"start-preshow",
	"start-night",
	"request-final-show",
	"fade-out-night",
	"power-down-presentation",
}

func isNightCommand(name string) bool {
	for _, c := range nightCommands {
		if c == name {
			return true
		}
	}
	return false
}

// nightCommandResult mirrors NightCommandResult in the coordinator's API.
type nightCommandResult struct {
	Command string `json:"command"`
	Outcome string `json:"outcome"`
	Reason  string `json:"reason"`
}

// nightSessionState is the subset of NightSessionState this program reports.
type nightSessionState struct {
	State string `json:"state"`
}

// nightCommandResponse mirrors the 202 body of POST /api/v1/night/commands/{command}.
type nightCommandResponse struct {
	Command nightCommandResult `json:"command"`
	Session nightSessionState  `json:"session"`
}

// nightSubmitResult is one night command attempt, classified like a macro run.
type nightSubmitResult struct {
	Class        string
	HTTPStatus   int
	Response     *nightCommandResponse
	Problem      *problemDoc
	TransportErr error
}

// nightStatusRecord is the latest night command attempt, kept apart from
// the macro status record so neither overwrites the other.
type nightStatusRecord struct {
	SchemaVersion int       `json:"schemaVersion"`
	Timestamp     time.Time `json:"timestamp"`
	Command       string    `json:"command"`
	Class         string    `json:"class"`
	HTTPStatus    int       `json:"httpStatus"`
	Outcome       string    `json:"outcome,omitempty"`
	SessionState  string    `json:"sessionState,omitempty"`
	Message       string    `json:"message"`
}

const nightStatusSchemaVersion = 1

func writeNightStatus(configDir string, rec nightStatusRecord) error {
	rec.SchemaVersion = nightStatusSchemaVersion
	return writeJSONFile(nightStatusPath(configDir), rec)
}

// nightRequestBody is the empty object, or {"stopFppPlayback":true} when
// stopPlaylists is set. Only prepare-site is ever sent the latter.
func nightRequestBody(stopPlaylists bool) []byte {
	if stopPlaylists {
		return []byte(`{"stopFppPlayback":true}`)
	}
	return []byte("{}")
}

// submitNightCommand POSTs a request body (see nightRequestBody) to
// <coordinatorURL>/api/v1/night/commands/<command> and classifies the outcome.
// A 2xx is trusted only when the body names the command that was sent.
func submitNightCommand(ctx context.Context, httpClient *http.Client, coordinatorURL *url.URL, token, command string, stopPlaylists bool) nightSubmitResult {
	u := *coordinatorURL
	u.Path = strings.TrimRight(u.Path, "/") + "/api/v1/night/commands/" + command

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, u.String(), bytes.NewReader(nightRequestBody(stopPlaylists)))
	if err != nil {
		return nightSubmitResult{Class: classUnreachable, TransportErr: fmt.Errorf("building request: %w", err)}
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("ShowMesh-API-Version", clientAPIVersion)
	req.Header.Set("Authorization", "Bearer "+token)

	resp, err := httpClient.Do(req)
	if err != nil {
		return nightSubmitResult{Class: classUnreachable, TransportErr: err}
	}
	defer func() { _ = resp.Body.Close() }()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, maxRunResponseBytes+1))
	if err != nil {
		return nightSubmitResult{Class: classUnreachable, HTTPStatus: resp.StatusCode, TransportErr: fmt.Errorf("reading response body: %w", err)}
	}
	if int64(len(respBody)) > maxRunResponseBytes {
		return nightSubmitResult{Class: classUnreachable, HTTPStatus: resp.StatusCode, TransportErr: fmt.Errorf("response body exceeded %d byte limit", maxRunResponseBytes)}
	}

	result := nightSubmitResult{Class: classifyHTTPStatus(resp.StatusCode), HTTPStatus: resp.StatusCode}
	if result.Class == classOK {
		var decoded nightCommandResponse
		if err := json.Unmarshal(respBody, &decoded); err != nil {
			return nightSubmitResult{Class: classUnreachable, HTTPStatus: resp.StatusCode, TransportErr: fmt.Errorf("decoding a %d response: %w", resp.StatusCode, err)}
		}
		if decoded.Command.Command != command || decoded.Command.Outcome == "" {
			return nightSubmitResult{Class: classUnreachable, HTTPStatus: resp.StatusCode,
				TransportErr: fmt.Errorf("a %d response did not confirm %s; not treating it as accepted", resp.StatusCode, command)}
		}
		result.Response = &decoded
		return result
	}
	var p problemDoc
	if json.Unmarshal(respBody, &p) == nil && p.Title != "" {
		result.Problem = &p
	}
	return result
}

// cmdNight implements "showmesh-fpp-plugin night <command>".
func cmdNight(args []string, stdout, stderr io.Writer, clock func() time.Time) int {
	fs := flag.NewFlagSet("showmesh-fpp-plugin night", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var configDirFlag string
	var timeout time.Duration
	var stopPlaylists bool
	fs.StringVar(&configDirFlag, "config-dir", "", "override this plugin's state directory; never the credential")
	fs.DurationVar(&timeout, "timeout", defaultRunTimeout, "request timeout for the night command")
	fs.BoolVar(&stopPlaylists, "stop-playlists", false, "prepare-site only: also stop whatever FPP is playing; from inside a playlist this stops that playlist too")
	fs.Usage = func() {
		_, _ = fmt.Fprintln(stderr, "usage: showmesh-fpp-plugin night <command> [flags]")
		_, _ = fmt.Fprintf(stderr, "\nSend one night lifecycle command to the coordinator and record the outcome locally.\nCommands: %s\n", strings.Join(nightCommands, ", "))
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return flagParseExit(err)
	}
	positional := fs.Args()
	if len(positional) > 1 {
		// Flags may also follow the command: night prepare-site --stop-playlists.
		if err := fs.Parse(positional[1:]); err != nil {
			return flagParseExit(err)
		}
		positional = append(positional[:1], fs.Args()...)
	}
	if len(positional) != 1 || !isNightCommand(positional[0]) {
		fs.Usage()
		return exitUsage
	}
	command := positional[0]
	if stopPlaylists && command != "prepare-site" {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin night: --stop-playlists applies only to prepare-site, not %s.\n", command)
		return exitUsage
	}
	configDir := resolveConfigDir(configDirFlag)
	now := clock()

	if note := ensureCredentialDirMode().Note(); note != "" {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin night: %s\n", note)
	}
	token, err := loadCredential()
	if err != nil {
		return reportNightLocalError(stderr, configDir, command, now, err)
	}
	coordinatorURL, err := loadCoordinatorURL(configDir)
	if err != nil {
		return reportNightLocalError(stderr, configDir, command, now, err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	result := submitNightCommand(ctx, newNonRedirectingHTTPClient(timeout), coordinatorURL, token, command, stopPlaylists)
	return reportNightResult(stdout, stderr, configDir, command, now, result)
}

func reportNightLocalError(stderr io.Writer, configDir, command string, now time.Time, cause error) int {
	rec := nightStatusRecord{Timestamp: now, Command: command, Class: classLocalError, Message: cause.Error()}
	if err := writeNightStatus(configDir, rec); err != nil {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin night: also failed to write the local status record: %v\n", err)
	}
	_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin night: %s\n", cause)
	return exitLocalError
}

func reportNightResult(stdout, stderr io.Writer, configDir, command string, now time.Time, result nightSubmitResult) int {
	rec := nightStatusRecord{Timestamp: now, Command: command, Class: result.Class, HTTPStatus: result.HTTPStatus}
	exitCode := exitOK
	switch result.Class {
	case classOK:
		rec.Outcome = result.Response.Command.Outcome
		rec.SessionState = result.Response.Session.State
		reason := result.Response.Command.Reason
		if rec.Outcome == "idempotent_no_op" && reason != "" {
			rec.Message = fmt.Sprintf("The coordinator already had %s in effect. The night is %s.", command, rec.SessionState)
		} else if rec.Outcome == "idempotent_no_op" {
			rec.Message = fmt.Sprintf("The coordinator already had %s in effect, so nothing changed. The night is %s.", command, rec.SessionState)
		} else {
			rec.Message = fmt.Sprintf("The coordinator accepted %s. The night is now %s.", command, rec.SessionState)
		}
		if reason != "" {
			rec.Message += " " + reason
		}
	case classRefused:
		exitCode = exitRefused
		rec.Message = fmt.Sprintf("The coordinator refused this plugin's credential for %s (%s). Check the credential installed for this plugin.", command, nightProblemText(result))
	case classRejected:
		exitCode = exitRejected
		rec.Message = fmt.Sprintf("The coordinator declined %s: %s", command, nightProblemText(result))
	default:
		exitCode = exitUnreachable
		rec.Message = fmt.Sprintf("The coordinator could not be reached for %s (%s). Start the night from the ShowMesh UI or showmeshctl.", command, nightProblemText(result))
	}
	if err := writeNightStatus(configDir, rec); err != nil {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin night: also failed to write the local status record: %v\n", err)
	}
	if exitCode == exitOK {
		_, _ = fmt.Fprintln(stdout, rec.Message)
	} else {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin night: %s\n", rec.Message)
	}
	return exitCode
}

func nightProblemText(result nightSubmitResult) string {
	return problemDetailText(submitResult{Problem: result.Problem, TransportErr: result.TransportErr, HTTPStatus: result.HTTPStatus})
}
