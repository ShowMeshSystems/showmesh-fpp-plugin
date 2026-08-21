package main

import (
	"bytes"
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// fakeCoordinatorClient records what the command logic asked for and
// answers with whatever the test staged, so the run path is exercisable
// with no HTTP server, no credential file, and no coordinator URL.
type fakeCoordinatorClient struct {
	submitResults []submitResult
	submitted     []createMacroRunRequest
	submittedIDs  []string

	config      showMacroConfigResponse
	configErr   error
	configCalls []string
}

func (f *fakeCoordinatorClient) SubmitMacroRun(_ context.Context, macroID string, body createMacroRunRequest) submitResult {
	f.submitted = append(f.submitted, body)
	f.submittedIDs = append(f.submittedIDs, macroID)
	if len(f.submitResults) == 0 {
		panic("fakeCoordinatorClient: no staged submit result")
	}
	r := f.submitResults[0]
	if len(f.submitResults) > 1 {
		f.submitResults = f.submitResults[1:]
	}
	return r
}

func (f *fakeCoordinatorClient) FetchMacroConfig(_ context.Context, macroID string) (showMacroConfigResponse, error) {
	f.configCalls = append(f.configCalls, macroID)
	return f.config, f.configErr
}

func okResult(runID, macroID string, revision int) submitResult {
	return submitResult{
		Class:      classOK,
		HTTPStatus: 202,
		Run:        &macroRunSubmitResponse{Run: macroRun{ID: runID, MacroObjectID: macroID, MacroRevision: revision}},
	}
}

// runMacroWithFake drives the command logic below the client seam against
// a fresh state directory.
func runMacroWithFake(t *testing.T, client CoordinatorClient, macroID string, now time.Time) (dir string, code int, stdout, stderr string) {
	t.Helper()
	dir = t.TempDir()
	var out, errOut bytes.Buffer
	code = runMacro(context.Background(), client, &out, &errOut, dir, macroID, now, "")
	return dir, code, out.String(), errOut.String()
}

func TestRunMacroSendsBufferedPriorFailuresInTheRunRequest(t *testing.T) {
	now := time.Date(2026, 8, 21, 22, 0, 0, 0, time.UTC)
	dir := t.TempDir()
	seeded := failureBuffer{Failures: []bufferedFailure{
		{MacroObjectID: "my-macro", Class: classRefused, HTTPStatus: 403, At: now.Add(-time.Hour)},
		{MacroObjectID: "my-macro", Class: classUnreachable, HTTPStatus: 0, At: now.Add(-time.Minute)},
	}, Dropped: 3}
	if err := saveFailureBuffer(dir, seeded); err != nil {
		t.Fatal(err)
	}

	fake := &fakeCoordinatorClient{submitResults: []submitResult{okResult("run-1", "my-macro", 4)}}
	var out, errOut bytes.Buffer
	if code := runMacro(context.Background(), fake, &out, &errOut, dir, "my-macro", now, ""); code != exitOK {
		t.Fatalf("exit code = %d, want %d (stderr: %s)", code, exitOK, errOut.String())
	}

	if len(fake.submitted) != 1 {
		t.Fatalf("submit calls = %d, want 1", len(fake.submitted))
	}
	body := fake.submitted[0]
	if len(body.PriorFailures) != 2 {
		t.Fatalf("priorFailures = %d, want 2", len(body.PriorFailures))
	}
	if body.PriorFailuresDropped != 3 {
		t.Errorf("priorFailuresDropped = %d, want 3", body.PriorFailuresDropped)
	}
	if body.Trigger != "plugin" {
		t.Errorf("trigger = %q, want %q", body.Trigger, "plugin")
	}
	if body.IdempotencyKey == "" {
		t.Error("idempotencyKey is empty; every submission mints its own")
	}
}

func TestRunMacroFlushesTheBufferOnlyOnA2xx(t *testing.T) {
	now := time.Date(2026, 8, 21, 22, 0, 0, 0, time.UTC)
	seeded := failureBuffer{Failures: []bufferedFailure{
		{MacroObjectID: "my-macro", Class: classRefused, HTTPStatus: 403, At: now.Add(-time.Hour)},
	}, Dropped: 2}

	cases := []struct {
		name        string
		result      submitResult
		wantCode    int
		wantEntries int
		wantDropped int
	}{
		{"ok flushes", okResult("run-1", "my-macro", 1), exitOK, 0, 0},
		{"refused retains and appends", submitResult{Class: classRefused, HTTPStatus: 403}, exitRefused, 2, 2},
		{"rejected retains and appends", submitResult{Class: classRejected, HTTPStatus: 404}, exitRejected, 2, 2},
		{"unreachable retains and appends", submitResult{Class: classUnreachable, TransportErr: errors.New("connection refused")}, exitUnreachable, 2, 2},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			if err := saveFailureBuffer(dir, seeded); err != nil {
				t.Fatal(err)
			}
			fake := &fakeCoordinatorClient{submitResults: []submitResult{tc.result}}
			var out, errOut bytes.Buffer
			if code := runMacro(context.Background(), fake, &out, &errOut, dir, "my-macro", now, ""); code != tc.wantCode {
				t.Fatalf("exit code = %d, want %d", code, tc.wantCode)
			}
			after, err := loadFailureBuffer(dir)
			if err != nil {
				t.Fatal(err)
			}
			if len(after.Failures) != tc.wantEntries {
				t.Errorf("buffered failures = %d, want %d", len(after.Failures), tc.wantEntries)
			}
			if after.Dropped != tc.wantDropped {
				t.Errorf("dropped = %d, want %d", after.Dropped, tc.wantDropped)
			}
		})
	}
}

// A 2xx whose body could not be believed is classified unreachable by the
// client, and the buffer must survive it: the coordinator holds no record
// of failures this program could not confirm were delivered.
func TestRunMacroDoesNotFlushOnAnUnconfirmed2xx(t *testing.T) {
	now := time.Date(2026, 8, 21, 22, 0, 0, 0, time.UTC)
	dir := t.TempDir()
	if err := saveFailureBuffer(dir, failureBuffer{Failures: []bufferedFailure{
		{MacroObjectID: "my-macro", Class: classRefused, HTTPStatus: 403, At: now},
	}}); err != nil {
		t.Fatal(err)
	}

	fake := &fakeCoordinatorClient{submitResults: []submitResult{{
		Class:        classUnreachable,
		HTTPStatus:   202,
		TransportErr: errors.New("a 202 response carried no run id"),
	}}}
	var out, errOut bytes.Buffer
	if code := runMacro(context.Background(), fake, &out, &errOut, dir, "my-macro", now, ""); code != exitUnreachable {
		t.Fatalf("exit code = %d, want %d", code, exitUnreachable)
	}
	after, err := loadFailureBuffer(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(after.Failures) != 2 {
		t.Errorf("buffered failures = %d, want the seeded entry retained plus this attempt appended", len(after.Failures))
	}
	if len(fake.configCalls) != 0 {
		t.Errorf("macro definition was fetched for a non-ok outcome: %v", fake.configCalls)
	}
}

func TestRunMacroFetchesTheDefinitionOnlyWhenTheCachedRevisionIsStale(t *testing.T) {
	now := time.Date(2026, 8, 21, 22, 0, 0, 0, time.UTC)

	t.Run("nothing cached fetches", func(t *testing.T) {
		fake := &fakeCoordinatorClient{
			submitResults: []submitResult{okResult("run-1", "my-macro", 7)},
			config:        showMacroConfigResponse{Revision: 7, Payload: configShowMacro{Label: "Ten PM dim"}},
		}
		dir, code, _, errOut := runMacroWithFake(t, fake, "my-macro", now)
		if code != exitOK {
			t.Fatalf("exit code = %d, want %d (stderr: %s)", code, exitOK, errOut)
		}
		if len(fake.configCalls) != 1 {
			t.Fatalf("config fetches = %d, want 1", len(fake.configCalls))
		}
		if rev, ok := cachedRevisionFor(dir, "my-macro"); !ok || rev != 7 {
			t.Errorf("cached revision = %d (ok=%v), want 7", rev, ok)
		}
	})

	t.Run("current revision does not fetch", func(t *testing.T) {
		dir := t.TempDir()
		if err := updateMacroCache(dir, "my-macro", showMacroConfigResponse{Revision: 7, Payload: configShowMacro{Label: "Ten PM dim"}}, now); err != nil {
			t.Fatal(err)
		}
		fake := &fakeCoordinatorClient{submitResults: []submitResult{okResult("run-1", "my-macro", 7)}}
		var out, errOut bytes.Buffer
		if code := runMacro(context.Background(), fake, &out, &errOut, dir, "my-macro", now, ""); code != exitOK {
			t.Fatalf("exit code = %d, want %d", code, exitOK)
		}
		if len(fake.configCalls) != 0 {
			t.Errorf("config fetches = %d, want 0 for an already-current cache", len(fake.configCalls))
		}
	})
}

// A cache-refresh failure degrades the cache, never the run that already
// succeeded.
func TestRunMacroKeepsTheRunSuccessWhenTheDefinitionFetchFails(t *testing.T) {
	now := time.Date(2026, 8, 21, 22, 0, 0, 0, time.UTC)
	fake := &fakeCoordinatorClient{
		submitResults: []submitResult{okResult("run-1", "my-macro", 2)},
		configErr:     errors.New("HTTP 500"),
	}
	dir, code, stdout, _ := runMacroWithFake(t, fake, "my-macro", now)
	if code != exitOK {
		t.Fatalf("exit code = %d, want %d", code, exitOK)
	}
	if !bytes.Contains([]byte(stdout), []byte("run-1")) {
		t.Errorf("stdout = %q, want it to report the accepted run", stdout)
	}
	rec, ok, err := loadStatus(dir)
	if err != nil || !ok {
		t.Fatalf("expected a status record: ok=%v err=%v", ok, err)
	}
	if rec.Class != classOK {
		t.Errorf("class = %q, want %q", rec.Class, classOK)
	}
}

// The HTTP implementation must carry the credential as a bearer header and
// the macro id in the path, and must never place either in a query
// parameter.
func TestHTTPCoordinatorClientSendsTheCredentialAsABearerHeader(t *testing.T) {
	var gotAuth, gotPath, gotQuery string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotAuth, gotPath, gotQuery = r.Header.Get("Authorization"), r.URL.Path, r.URL.RawQuery
		w.WriteHeader(http.StatusAccepted)
		_, _ = w.Write([]byte(`{"run":{"id":"run-1","macroObjectId":"my-macro","macroRevision":1}}`))
	}))
	t.Cleanup(srv.Close)

	client := &httpCoordinatorClient{httpClient: srv.Client(), coordinatorURL: mustParseURL(t, srv.URL), token: "secret-token"}
	result := client.SubmitMacroRun(context.Background(), "my-macro", createMacroRunRequest{Trigger: "plugin"})

	if result.Class != classOK {
		t.Fatalf("class = %q, want %q", result.Class, classOK)
	}
	if gotAuth != "Bearer secret-token" {
		t.Errorf("Authorization = %q, want a bearer credential", gotAuth)
	}
	if gotPath != "/api/v1/macros/my-macro/runs" {
		t.Errorf("path = %q", gotPath)
	}
	if gotQuery != "" {
		t.Errorf("query string = %q, want empty; a credential or macro id must never ride in a query parameter", gotQuery)
	}
}
