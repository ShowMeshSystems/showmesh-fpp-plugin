package main

import (
	"context"
	"net/http"
	"net/url"
)

// CoordinatorClient is the narrow consumer-side seam this program's
// command logic talks to. It carries exactly the two coordinator calls the
// macro path makes and nothing else: local persistence, cached macro data,
// status rendering, refusal-versus-outage classification, and
// failure-buffer policy stay plugin-owned above this interface.
//
// Buffered prior failures are deliberately not a separate call. They ride
// in the run request so the buffer is cleared only after a 2xx response,
// which is what makes the report transactional.
type CoordinatorClient interface {
	// SubmitMacroRun submits one macro run and returns the outcome already
	// classified. It returns no error: a transport failure is itself one of
	// the classifications.
	SubmitMacroRun(ctx context.Context, macroID string, body createMacroRunRequest) submitResult
	// FetchMacroConfig reads a macro's current definition for the local
	// cache. Its failure is a cache outcome, never a run outcome.
	FetchMacroConfig(ctx context.Context, macroID string) (showMacroConfigResponse, error)
}

// httpCoordinatorClient is the handwritten HTTP implementation, holding
// the per-invocation credential and coordinator URL so neither is threaded
// through the command logic above the seam.
type httpCoordinatorClient struct {
	httpClient     *http.Client
	coordinatorURL *url.URL
	token          string
}

func (c *httpCoordinatorClient) SubmitMacroRun(ctx context.Context, macroID string, body createMacroRunRequest) submitResult {
	return submitMacroRun(ctx, c.httpClient, c.coordinatorURL, c.token, macroID, body)
}

func (c *httpCoordinatorClient) FetchMacroConfig(ctx context.Context, macroID string) (showMacroConfigResponse, error) {
	return fetchMacroConfig(ctx, c.httpClient, c.coordinatorURL, c.token, macroID)
}
