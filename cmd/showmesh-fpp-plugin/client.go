package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
)

// clientAPIVersion is the only API major version this build understands,
// sent on every request as ShowMesh-API-Version.
const clientAPIVersion = "1"

// maxRunResponseBytes bounds how much of the run-submission response body
// this program reads. SHOWMESH HYPOTHESIS, NOT MEASURED, chosen only so a
// misbehaving coordinator or a proxy serving something unrelated at the
// same URL cannot make this program buffer an unbounded body in memory.
// This program's own response shape is one run with at most 32 steps.
const maxRunResponseBytes = 1 << 20 // 1 MiB

// submitResult is the outcome of one POST /api/v1/macros/{id}/runs
// attempt, already classified per section 8.2's four classes. Exactly one
// of Run or Problem is populated, depending on Class: Run only for
// classOK, Problem (best-effort — a response that is not valid
// problem+json still classifies correctly from the status code alone)
// for the three degraded classes.
type submitResult struct {
	Class      string
	HTTPStatus int // 0 when there was no HTTP response at all
	Run        *macroRunSubmitResponse
	Problem    *problemDoc
	// TransportErr is the raw transport-level error for a classUnreachable
	// result that never got an HTTP response at all. nil whenever an HTTP
	// response was received, even an error one.
	TransportErr error
}

// submitMacroRun POSTs body to <coordinatorURL>/api/v1/macros/<macroID>/runs
// with token as a bearer credential, and classifies the outcome. It never
// panics on a malformed or oversized response; a decode failure on an
// otherwise-2xx response is reported as classUnreachable with a
// TransportErr, because a response this program cannot parse is not
// meaningfully different from one that never arrived — this program
// cannot honestly claim the run was accepted if it cannot read what the
// coordinator said about it.
func submitMacroRun(ctx context.Context, httpClient *http.Client, coordinatorURL *url.URL, token, macroID string, body createMacroRunRequest) submitResult {
	encoded, err := json.Marshal(body)
	if err != nil {
		// A local encoding failure of a struct this program itself built
		// is not a coordinator classification at all; callers treat this
		// the same as any other pre-flight local error.
		return submitResult{Class: classUnreachable, TransportErr: fmt.Errorf("encoding request body: %w", err)}
	}

	u := *coordinatorURL
	// u.Path holds the DECODED path; url.URL.String() (via EscapedPath())
	// re-encodes it when serializing. macroID therefore goes in RAW, not
	// pre-escaped with url.PathEscape — escaping it here and letting
	// String() escape the result a second time turned a literal "%" in
	// the first escape into "%25" in the second, corrupting any macro id
	// containing a space or a slash into a 404 that reads as though the
	// id were wrong when it was not. See TestSubmitMacroRunDoesNotDoubleEscapeMacroID.
	u.Path = strings.TrimRight(u.Path, "/") + "/api/v1/macros/" + macroID + "/runs"

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, u.String(), bytes.NewReader(encoded))
	if err != nil {
		return submitResult{Class: classUnreachable, TransportErr: fmt.Errorf("building request: %w", err)}
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("ShowMesh-API-Version", clientAPIVersion)
	req.Header.Set("Authorization", "Bearer "+token)

	resp, err := httpClient.Do(req)
	if err != nil {
		// Every transport-level failure this program can observe — DNS
		// failure, connection refused, TLS failure, a context deadline —
		// is the genuine ADR-004 outage condition RES-015 section 7.2
		// establishes FPP itself cannot detect at all. httpStatus stays 0:
		// a real, distinct value meaning "no response was ever received",
		// never conflated with an actual 5xx.
		return submitResult{Class: classUnreachable, TransportErr: err}
	}
	defer func() { _ = resp.Body.Close() }()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, maxRunResponseBytes+1))
	if err != nil {
		return submitResult{Class: classUnreachable, HTTPStatus: resp.StatusCode, TransportErr: fmt.Errorf("reading response body: %w", err)}
	}
	if int64(len(respBody)) > maxRunResponseBytes {
		return submitResult{Class: classUnreachable, HTTPStatus: resp.StatusCode, TransportErr: fmt.Errorf("response body exceeded %d byte limit", maxRunResponseBytes)}
	}

	class := classifyHTTPStatus(resp.StatusCode)
	result := submitResult{Class: class, HTTPStatus: resp.StatusCode}

	if class == classOK {
		var decoded macroRunSubmitResponse
		if err := json.Unmarshal(respBody, &decoded); err != nil {
			return submitResult{Class: classUnreachable, HTTPStatus: resp.StatusCode, TransportErr: fmt.Errorf("decoding a %d response: %w", resp.StatusCode, err)}
		}
		// A 2xx status code is not itself confirmation: this program's own
		// doc comment above says it "cannot honestly claim the run was
		// accepted if it cannot read what the coordinator said about it",
		// and an empty or malformed body reads perfectly as valid JSON
		// while saying nothing at all. Proved live: a fake coordinator
		// answering "202 {}" decoded with no error, an empty run id, and
		// this program reported class ok, printed "accepted as run  "
		// (blank id), and flushed a pre-seeded refused buffer entry out
		// of existence. Neither an empty run id nor a run answering about
		// a DIFFERENT macro than the one this call named is treated as
		// ok, and neither flushes the buffer — see reportOK's caller,
		// which only flushes on classOK. Classified as classUnreachable,
		// not a new class: the coordinator did not demonstrably serve
		// this request, which is exactly what that class means for every
		// other case reaching it (transport failure, 5xx).
		if decoded.Run.ID == "" {
			return submitResult{Class: classUnreachable, HTTPStatus: resp.StatusCode,
				TransportErr: fmt.Errorf("a %d response carried no run id; refusing to treat an unconfirmed body as an accepted run", resp.StatusCode)}
		}
		if decoded.Run.MacroObjectID != macroID {
			return submitResult{Class: classUnreachable, HTTPStatus: resp.StatusCode,
				TransportErr: fmt.Errorf("a %d response named macro %q, not the macro %q this request submitted; refusing to treat this as an accepted run",
					resp.StatusCode, decoded.Run.MacroObjectID, macroID)}
		}
		result.Run = &decoded
		return result
	}

	// Best-effort problem decode: a body that is not valid problem+json
	// still leaves the class and HTTP status correctly set from the
	// status code alone, so a caller never loses the classification over
	// an unparsable body (a proxy's own HTML error page, for instance).
	var p problemDoc
	if json.Unmarshal(respBody, &p) == nil && p.Title != "" {
		result.Problem = &p
	}
	return result
}

// fetchMacroConfig GETs /api/v1/config/show.macro/{macroID} and decodes it.
// This is section 8.1's actual "last successful authenticated fetch": the
// scheduler credential's show:macro:run scope is exactly what section 5.5
// authorizes this read with, deliberately, so a runner can read what it
// runs. Unlike submitMacroRun this function does NOT classify its outcome
// into section 8.2's four classes — a failed cache refresh is not itself a
// run outcome; run.go's caller treats any non-nil error here as "keep
// whatever cache already exists" and logs it, never as a reason to fail
// the run that already succeeded.
func fetchMacroConfig(ctx context.Context, httpClient *http.Client, coordinatorURL *url.URL, token, macroID string) (showMacroConfigResponse, error) {
	u := *coordinatorURL
	// See submitMacroRun's identical comment: macroID goes in raw, not
	// pre-escaped, so url.URL.String() escapes it exactly once.
	u.Path = strings.TrimRight(u.Path, "/") + "/api/v1/config/show.macro/" + macroID

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	if err != nil {
		return showMacroConfigResponse{}, fmt.Errorf("building request: %w", err)
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("ShowMesh-API-Version", clientAPIVersion)
	req.Header.Set("Authorization", "Bearer "+token)

	resp, err := httpClient.Do(req)
	if err != nil {
		return showMacroConfigResponse{}, fmt.Errorf("requesting macro definition: %w", err)
	}
	defer func() { _ = resp.Body.Close() }()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, maxRunResponseBytes+1))
	if err != nil {
		return showMacroConfigResponse{}, fmt.Errorf("reading macro definition response body: %w", err)
	}
	if int64(len(respBody)) > maxRunResponseBytes {
		return showMacroConfigResponse{}, fmt.Errorf("macro definition response body exceeded %d byte limit", maxRunResponseBytes)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		var p problemDoc
		if json.Unmarshal(respBody, &p) == nil && p.Title != "" {
			return showMacroConfigResponse{}, fmt.Errorf("fetching macro definition: HTTP %d: %s: %s", resp.StatusCode, p.Title, p.Detail)
		}
		return showMacroConfigResponse{}, fmt.Errorf("fetching macro definition: HTTP %d", resp.StatusCode)
	}

	var decoded showMacroConfigResponse
	if err := json.Unmarshal(respBody, &decoded); err != nil {
		return showMacroConfigResponse{}, fmt.Errorf("decoding macro definition response: %w", err)
	}
	return decoded, nil
}

// classifyHTTPStatus is section 8.2's table, applied to a status code that
// DID receive an HTTP response (an actual transport failure is classified
// separately, in submitMacroRun, and never reaches this function).
func classifyHTTPStatus(status int) string {
	switch {
	case status >= 200 && status < 300:
		return classOK
	case status == 401 || status == 403:
		return classRefused
	case status >= 400 && status < 500:
		return classRejected
	default:
		// Everything else — 5xx, and any other status this program does
		// not specifically recognize — is "the coordinator could not
		// serve", the genuine outage condition, per section 8.2's own
		// "unreachable" row ("transport failure, or 5xx").
		return classUnreachable
	}
}
