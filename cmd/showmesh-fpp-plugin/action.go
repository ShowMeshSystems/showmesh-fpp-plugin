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

// actionInvocationRequest is the whole body of POST /api/v1/actions/{id}/invocations; the server refuses any other key.
type actionInvocationRequest struct {
	IdempotencyKey string `json:"idempotencyKey"`
}

// actionInvocationResult is the subset of ActionInvocationResult this program reads.
type actionInvocationResult struct {
	ID            string  `json:"id"`
	ActionID      string  `json:"actionId"`
	Revision      int     `json:"revision"`
	Replay        bool    `json:"replay"`
	State         string  `json:"state"`
	Outcome       *string `json:"outcome"`
	OutcomeReason string  `json:"outcomeReason"`
}

// actionInvocationResponse is the 200 body of POST /api/v1/actions/{id}/invocations.
type actionInvocationResponse struct {
	Result actionInvocationResult `json:"result"`
}

// actionResult is one invocation attempt, classified like submitResult.
type actionResult struct {
	Class        string
	HTTPStatus   int
	Invocation   *actionInvocationResult
	Problem      *problemDoc
	TransportErr error
}

func (r actionResult) asSubmitResult() submitResult {
	return submitResult{Class: r.Class, HTTPStatus: r.HTTPStatus, Problem: r.Problem, TransportErr: r.TransportErr}
}

func submitActionInvocation(ctx context.Context, httpClient *http.Client, coordinatorURL *url.URL, token, actionID, key string) actionResult {
	encoded, err := json.Marshal(actionInvocationRequest{IdempotencyKey: key})
	if err != nil {
		return actionResult{Class: classUnreachable, TransportErr: fmt.Errorf("encoding request body: %w", err)}
	}
	u := *coordinatorURL
	// The id goes in raw; url.URL.String() escapes it once.
	u.Path = strings.TrimRight(u.Path, "/") + "/api/v1/actions/" + actionID + "/invocations"

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, u.String(), bytes.NewReader(encoded))
	if err != nil {
		return actionResult{Class: classUnreachable, TransportErr: fmt.Errorf("building request: %w", err)}
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("ShowMesh-API-Version", clientAPIVersion)
	req.Header.Set("Authorization", "Bearer "+token)

	resp, err := httpClient.Do(req)
	if err != nil {
		return actionResult{Class: classUnreachable, TransportErr: err}
	}
	defer func() { _ = resp.Body.Close() }()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, maxRunResponseBytes+1))
	if err != nil {
		return actionResult{Class: classUnreachable, HTTPStatus: resp.StatusCode, TransportErr: fmt.Errorf("reading response body: %w", err)}
	}
	if int64(len(respBody)) > maxRunResponseBytes {
		return actionResult{Class: classUnreachable, HTTPStatus: resp.StatusCode, TransportErr: fmt.Errorf("response body exceeded %d byte limit", maxRunResponseBytes)}
	}

	class := classifyHTTPStatus(resp.StatusCode)
	result := actionResult{Class: class, HTTPStatus: resp.StatusCode}
	if class == classOK {
		var decoded actionInvocationResponse
		if err := json.Unmarshal(respBody, &decoded); err != nil {
			return actionResult{Class: classUnreachable, HTTPStatus: resp.StatusCode, TransportErr: fmt.Errorf("decoding a %d response: %w", resp.StatusCode, err)}
		}
		// A 2xx that does not name this action is not confirmation, same rule as a macro run.
		if decoded.Result.ID == "" || decoded.Result.ActionID != actionID {
			return actionResult{Class: classUnreachable, HTTPStatus: resp.StatusCode,
				TransportErr: fmt.Errorf("a %d response did not confirm an invocation of action %q; refusing to treat it as accepted", resp.StatusCode, actionID)}
		}
		result.Invocation = &decoded.Result
		return result
	}

	var p problemDoc
	if json.Unmarshal(respBody, &p) == nil && p.Title != "" {
		result.Problem = &p
	}
	return result
}

// cmdAction implements "showmesh-fpp-plugin action <actionId>": invoke one
// stored action and record the outcome in status.json, like cmdRun.
func cmdAction(args []string, stdout, stderr io.Writer, clock func() time.Time) int {
	fs := flag.NewFlagSet("showmesh-fpp-plugin action", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var configDirFlag string
	var timeout time.Duration
	fs.StringVar(&configDirFlag, "config-dir", "", "override this plugin's state directory (config.json, status.json); never the credential")
	fs.DurationVar(&timeout, "timeout", defaultRunTimeout, "request timeout for the invocation")
	fs.Usage = func() {
		_, _ = fmt.Fprintln(stderr, "usage: showmesh-fpp-plugin action [flags] [--] <actionId>")
		_, _ = fmt.Fprintln(stderr, "\nInvoke one stored action on this plugin's configured coordinator, and record")
		_, _ = fmt.Fprintln(stderr, "the outcome locally regardless of whether the coordinator could be reached.")
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return flagParseExit(err)
	}
	positional := fs.Args()
	if len(positional) != 1 || positional[0] == "" {
		fs.Usage()
		return exitUsage
	}
	actionID := positional[0]
	configDir := resolveConfigDir(configDirFlag)
	now := clock()

	credentialDirNote := ensureCredentialDirMode().Note()
	if credentialDirNote != "" {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin action: %s\n", credentialDirNote)
	}

	token, err := loadCredential()
	if err != nil {
		return reportActionLocalError(stderr, configDir, actionID, now, credentialDirNote, err)
	}
	coordinatorURL, err := loadCoordinatorURL(configDir)
	if err != nil {
		return reportActionLocalError(stderr, configDir, actionID, now, credentialDirNote, err)
	}
	key, err := newIdempotencyKey()
	if err != nil {
		return reportActionLocalError(stderr, configDir, actionID, now, credentialDirNote, fmt.Errorf("generating an idempotency key: %w", err))
	}

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	result := submitActionInvocation(ctx, newNonRedirectingHTTPClient(timeout), coordinatorURL, token, actionID, key)

	rec := statusRecord{Timestamp: now, ActionID: actionID, Class: result.Class, HTTPStatus: result.HTTPStatus, CredentialDirNote: credentialDirNote}
	code := exitOK
	switch result.Class {
	case classOK:
		inv := result.Invocation
		rec.Message = fmt.Sprintf("action %q accepted as invocation %s (revision %d), %s", actionID, inv.ID, inv.Revision, actionOutcomeText(inv))
		if inv.Replay {
			rec.Message += "; this idempotency key was already used, so the ORIGINAL invocation was returned rather than a new one"
		}
	case classRefused:
		code = exitRefused
		rec.Message = fmt.Sprintf("action %q: the coordinator answered and refused this caller (%s)", actionID, problemDetailText(result.asSubmitResult()))
	case classRejected:
		code = exitRejected
		rec.Message = fmt.Sprintf("action %q: the coordinator answered and declined the request itself (%s); this is NOT a credential problem", actionID, problemDetailText(result.asSubmitResult()))
	default:
		code = exitUnreachable
		rec.Message = fmt.Sprintf("action %q: the coordinator could not be reached, or answered with a server error (%s)", actionID, problemDetailText(result.asSubmitResult()))
	}
	if err := writeStatus(configDir, rec); err != nil {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin action: also failed to write the local status record: %v\n", err)
	}
	if code == exitOK {
		_, _ = fmt.Fprintf(stdout, "%s\n", rec.Message)
	} else {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin action: %s\n", rec.Message)
	}
	return code
}

func actionOutcomeText(inv *actionInvocationResult) string {
	if inv.Outcome == nil {
		return "outcome pending: " + inv.OutcomeReason
	}
	return "outcome " + *inv.Outcome + ": " + inv.OutcomeReason
}

// reportActionLocalError records an attempt that never reached the coordinator.
func reportActionLocalError(stderr io.Writer, configDir, actionID string, now time.Time, credentialDirNote string, cause error) int {
	rec := statusRecord{Timestamp: now, ActionID: actionID, Class: classLocalError, Message: cause.Error(), CredentialDirNote: credentialDirNote}
	if err := writeStatus(configDir, rec); err != nil {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin action: also failed to write the local status record: %v\n", err)
	}
	_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin action: %s\n", cause.Error())
	return exitLocalError
}
