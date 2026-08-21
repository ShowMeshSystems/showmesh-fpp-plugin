package main

import (
	"bytes"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
)

// setupPluginWithCoordinatorURL points this test's credential directory
// (via withCredentialDir, config.go's test-only seam) at a fresh temp dir
// holding a valid mode-0600 credential, and writes a SEPARATE fresh state
// directory's config.json to name coordinatorURL. Returns the state
// directory — the value cmdRun's --config-dir flag expects — since the
// credential directory is deliberately not expressible through any flag
// or environment variable in production; see config.go's credentialDir
// and credentialDirOverride doc comments for why the two are independent
// roots a test must set up separately.
func setupPluginWithCoordinatorURL(t *testing.T, token, coordinatorURL string) string {
	t.Helper()
	withCredentialDir(t, t.TempDir())
	if err := os.WriteFile(credentialPath(), []byte(token), 0o600); err != nil {
		t.Fatal(err)
	}
	stateDir := t.TempDir()
	cfg := `{"coordinatorUrl": "` + coordinatorURL + `"}`
	if err := os.WriteFile(coordinatorConfigPath(stateDir), []byte(cfg), 0o600); err != nil {
		t.Fatal(err)
	}
	return stateDir
}

// setupPlugin is setupPluginWithCoordinatorURL's own common case: a fake
// coordinator built with httptest.NewServer, whose URL comes from the
// server rather than being assembled by hand.
func setupPlugin(t *testing.T, srv *httptest.Server, token string) string {
	t.Helper()
	return setupPluginWithCoordinatorURL(t, token, srv.URL)
}

func fixedClock(t time.Time) func() time.Time { return func() time.Time { return t } }

// runResponse builds the fixed-shape POST /api/v1/macros/{id}/runs 2xx
// body this file's fake coordinators return.
func runResponse(id, macroObjectID string, revision int) macroRunSubmitResponse {
	return macroRunSubmitResponse{Run: macroRun{ID: id, MacroObjectID: macroObjectID, MacroRevision: revision}}
}

// configResponse builds the fixed-shape GET /api/v1/config/show.macro/{id}
// 2xx body this file's fake coordinators return.
func configResponse(revision int, label string, steps ...configShowMacroStep) showMacroConfigResponse {
	return showMacroConfigResponse{Revision: revision, Payload: configShowMacro{Label: label, Steps: steps}}
}

// testCoordinator is a fake coordinator serving the two endpoints
// cmdRun's success path can call: POST .../runs (runHandler) and GET
// .../config/show.macro/{id} (configHandler). Either may be nil, in which
// case that route 404s — a nil configHandler lets a test assert the
// config endpoint was never hit (the conditional-fetch tests rely on
// this: an unexpected GET fails the request instead of silently
// succeeding against the wrong handler).
func testCoordinator(t *testing.T, runHandler, configHandler http.HandlerFunc) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	if runHandler != nil {
		mux.HandleFunc("POST /api/v1/macros/{id}/runs", runHandler)
	}
	if configHandler != nil {
		mux.HandleFunc("GET /api/v1/config/show.macro/{id}", configHandler)
	}
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

func jsonHandler(status int, body any) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(status)
		_ = json.NewEncoder(w).Encode(body)
	}
}

func TestCmdRunRefusesOnWrongCredentialMode(t *testing.T) {
	withCredentialDir(t, t.TempDir())
	if err := os.WriteFile(credentialPath(), []byte("tok"), 0o644); err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	if err := os.WriteFile(coordinatorConfigPath(dir), []byte(`{"coordinatorUrl":"http://example.invalid"}`), 0o600); err != nil {
		t.Fatal(err)
	}

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitLocalError {
		t.Errorf("exit code = %d, want %d", code, exitLocalError)
	}
	rec, ok, err := loadStatus(dir)
	if err != nil || !ok {
		t.Fatalf("expected a status record to be written even for a local error: ok=%v err=%v", ok, err)
	}
	if rec.Class != classLocalError {
		t.Errorf("class = %q, want %q", rec.Class, classLocalError)
	}
	// The credential itself must never leak into the record or stderr.
	if strings.Contains(stderr.String(), "tok") || strings.Contains(rec.Message, "tok") {
		t.Error("the credential value leaked into an error message")
	}
}

// TestCmdRunRepairsCredentialDirModeAndProceeds is the coordinator's own
// correction proven end to end: a credential DIRECTORY mode mismatch is
// repaired in place, recorded on stderr and in the status record, and
// the run proceeds and succeeds regardless — never refused, because
// refusing would not un-expose a credential that a wrong directory mode
// already exposed.
func TestCmdRunRepairsCredentialDirModeAndProceeds(t *testing.T) {
	credDir := t.TempDir()
	if err := os.Chmod(credDir, 0o755); err != nil {
		t.Fatal(err)
	}
	setCredentialDirOverrideRaw(t, credDir)
	if err := os.WriteFile(credentialPath(), []byte("tok"), 0o600); err != nil {
		t.Fatal(err)
	}

	srv := testCoordinator(t,
		jsonHandler(http.StatusAccepted, runResponse("run-1", "my-macro", 1)),
		jsonHandler(http.StatusOK, configResponse(1, "Begin Set")),
	)
	stateDir := t.TempDir()
	if err := os.WriteFile(coordinatorConfigPath(stateDir), []byte(`{"coordinatorUrl": "`+srv.URL+`"}`), 0o600); err != nil {
		t.Fatal(err)
	}

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", stateDir, "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitOK {
		t.Fatalf("exit code = %d, want %d (a credential DIRECTORY mode mismatch must never block the run), stderr=%s", code, exitOK, stderr.String())
	}
	if !strings.Contains(stderr.String(), "repaired") {
		t.Errorf("stderr = %q, want a note that the credential directory was repaired", stderr.String())
	}

	rec, ok, err := loadStatus(stateDir)
	if err != nil || !ok {
		t.Fatalf("ok=%v err=%v", ok, err)
	}
	if rec.CredentialDirNote == "" {
		t.Error("status record's CredentialDirNote is empty, want the repair recorded there too, not only on stderr")
	}
	if rec.Class != classOK {
		t.Errorf("class = %q, want %q — the directory repair must not change the run's own outcome", rec.Class, classOK)
	}

	info, err := os.Stat(credDir)
	if err != nil {
		t.Fatal(err)
	}
	if got := info.Mode().Perm(); got != 0o700 {
		t.Errorf("credential directory mode after the run = %04o, want 0700 (repaired)", got)
	}
}

func TestCmdRunMissingMacroIDIsUsageError(t *testing.T) {
	dir := t.TempDir()
	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir}, &stdout, &stderr, time.Now)
	if code != exitUsage {
		t.Errorf("exit code = %d, want %d", code, exitUsage)
	}
}

// TestCmdRunCorruptFailureBufferIsCountedAndLogged is the regression
// guard for the finding that loadFailureBuffer returning an error used
// to reset to a fresh, empty buffer silently: Dropped stayed 0, nothing
// was printed, and every previously buffered refusal was destroyed with
// the outbound request itself reporting nothing was ever dropped. This
// program cannot recover how many entries an unreadable file held, so it
// counts the corruption event itself as at least 1 dropped record and
// says so on stderr, rather than reporting 0.
func TestCmdRunCorruptFailureBufferIsCountedAndLogged(t *testing.T) {
	var gotRunBody createMacroRunRequest
	srv := testCoordinator(t,
		func(w http.ResponseWriter, r *http.Request) {
			_ = json.NewDecoder(r.Body).Decode(&gotRunBody)
			w.WriteHeader(http.StatusAccepted)
			_ = json.NewEncoder(w).Encode(runResponse("run-1", "my-macro", 1))
		},
		jsonHandler(http.StatusOK, configResponse(1, "L")),
	)
	dir := setupPlugin(t, srv, "tok")
	if err := os.WriteFile(failureBufferPath(dir), []byte("{not valid json"), 0o600); err != nil {
		t.Fatal(err)
	}

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitOK {
		t.Fatalf("exit code = %d, want %d, stderr=%s", code, exitOK, stderr.String())
	}

	if stderr.Len() == 0 {
		t.Error("expected a stderr note that the failure buffer was corrupt and something was lost, got none")
	}
	if gotRunBody.PriorFailuresDropped < 1 {
		t.Errorf("priorFailuresDropped sent to the coordinator = %d, want at least 1 (the corruption event itself, not silently reported as 0)", gotRunBody.PriorFailuresDropped)
	}
}

func TestCmdRunOKCachesMacroFromConfigFetchAndFlushesBuffer(t *testing.T) {
	var gotRunBody createMacroRunRequest
	var configFetches int
	srv := testCoordinator(t,
		func(w http.ResponseWriter, r *http.Request) {
			_ = json.NewDecoder(r.Body).Decode(&gotRunBody)
			w.WriteHeader(http.StatusAccepted)
			_ = json.NewEncoder(w).Encode(runResponse("run-1", "my-macro", 2))
		},
		func(w http.ResponseWriter, r *http.Request) {
			configFetches++
			if got := r.Header.Get("Authorization"); got != "Bearer tok" {
				t.Errorf("config fetch Authorization header = %q, want Bearer tok", got)
			}
			jsonHandler(http.StatusOK, configResponse(2, "Begin Set",
				configShowMacroStep{ID: "s1", LocalFallback: configShowMacroLocalFallback{
					Class: "coordinator-required", Reason: "runs on the coordinator",
				}},
			))(w, r)
		},
	)

	dir := setupPlugin(t, srv, "tok")
	// Pre-seed a buffered failure to prove it gets sent AND flushed on
	// this success.
	if err := saveFailureBuffer(dir, failureBuffer{Failures: []bufferedFailure{
		{MacroObjectID: "my-macro", Class: classUnreachable, At: time.Now().Add(-time.Hour)},
	}}); err != nil {
		t.Fatal(err)
	}

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "my-macro"}, &stdout, &stderr, fixedClock(time.Now()))
	if code != exitOK {
		t.Fatalf("exit code = %d, want %d, stderr=%s", code, exitOK, stderr.String())
	}

	if len(gotRunBody.PriorFailures) != 1 {
		t.Errorf("request carried %d priorFailures, want 1 (the pre-seeded buffer entry)", len(gotRunBody.PriorFailures))
	}
	if gotRunBody.Trigger != "plugin" {
		t.Errorf("trigger = %q, want plugin", gotRunBody.Trigger)
	}
	if configFetches != 1 {
		t.Errorf("config fetches = %d, want exactly 1 (nothing was cached before this run)", configFetches)
	}

	buf, err := loadFailureBuffer(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(buf.Failures) != 0 {
		t.Errorf("buffer after a successful run has %d entries, want 0 (flush on 2xx)", len(buf.Failures))
	}

	c, err := loadMacroCache(dir)
	if err != nil {
		t.Fatal(err)
	}
	entry, ok := c.Macros["my-macro"]
	if !ok {
		t.Fatal("expected my-macro to be cached after a successful run")
	}
	if entry.Label != "Begin Set" {
		t.Errorf("cached Label = %q, want %q (from the config fetch, not the run response)", entry.Label, "Begin Set")
	}
	if entry.MacroRevision != 2 || len(entry.Steps) != 1 || entry.Steps[0].LocalFallbackClass != "coordinator-required" {
		t.Errorf("cached entry = %+v, want revision 2 with one coordinator-required step", entry)
	}
	if entry.Steps[0].LocalFallbackReason != "runs on the coordinator" {
		t.Errorf("cached step reason = %q, want %q (the run response carries no reason at all)", entry.Steps[0].LocalFallbackReason, "runs on the coordinator")
	}

	rec, ok, err := loadStatus(dir)
	if err != nil || !ok {
		t.Fatalf("ok=%v err=%v", ok, err)
	}
	if rec.Class != classOK || rec.RunID != "run-1" {
		t.Errorf("status = %+v, want class ok, runId run-1", rec)
	}
}

// TestCmdRunSkipsConfigFetchWhenCachedRevisionAlreadyCurrent pins the
// conditional-fetch rule: if the cache already holds the same revision
// the run itself pinned, a config fetch would cost a request for no new
// information, so it must not happen at all. A nil configHandler on the
// server makes an unexpected GET fail loudly rather than silently 404ing
// and being ignored.
func TestCmdRunSkipsConfigFetchWhenCachedRevisionAlreadyCurrent(t *testing.T) {
	srv := testCoordinator(t,
		jsonHandler(http.StatusAccepted, runResponse("run-1", "my-macro", 5)),
		nil, // must never be hit
	)
	dir := setupPlugin(t, srv, "tok")
	if err := updateMacroCache(dir, "my-macro", configResponse(5, "Begin Set"), time.Now().Add(-time.Hour)); err != nil {
		t.Fatal(err)
	}

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitOK {
		t.Fatalf("exit code = %d, want %d, stderr=%s", code, exitOK, stderr.String())
	}
	// nil configHandler means any GET to that route 404s; if this program
	// had fetched anyway it would have logged the resulting failure to
	// stderr (refreshMacroCacheIfStale never fails the run itself), so an
	// empty stderr is this test's actual assertion that no fetch happened.
	if stderr.Len() != 0 {
		t.Errorf("stderr = %q, want empty (a config fetch must not have been attempted)", stderr.String())
	}
}

// TestCmdRunRefetchesConfigWhenRevisionMoved is the other half of the
// conditional-fetch rule: a cached revision that no longer matches what
// the run just pinned means the macro was edited, and the cache must
// refresh rather than silently serving stale label/reason text on a
// future refusal.
func TestCmdRunRefetchesConfigWhenRevisionMoved(t *testing.T) {
	var configFetches int
	srv := testCoordinator(t,
		jsonHandler(http.StatusAccepted, runResponse("run-1", "my-macro", 6)),
		func(w http.ResponseWriter, r *http.Request) {
			configFetches++
			jsonHandler(http.StatusOK, configResponse(6, "Begin Set v2",
				configShowMacroStep{ID: "s1", LocalFallback: configShowMacroLocalFallback{Class: "none", Reason: "r"}},
			))(w, r)
		},
	)
	dir := setupPlugin(t, srv, "tok")
	if err := updateMacroCache(dir, "my-macro", configResponse(5, "Begin Set v1"), time.Now().Add(-time.Hour)); err != nil {
		t.Fatal(err)
	}

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitOK {
		t.Fatalf("exit code = %d, want %d, stderr=%s", code, exitOK, stderr.String())
	}
	if configFetches != 1 {
		t.Errorf("config fetches = %d, want 1 (the cached revision 5 no longer matches this run's pinned revision 6)", configFetches)
	}
	c, err := loadMacroCache(dir)
	if err != nil {
		t.Fatal(err)
	}
	if c.Macros["my-macro"].Label != "Begin Set v2" {
		t.Errorf("cached label = %q, want the REFRESHED value %q", c.Macros["my-macro"].Label, "Begin Set v2")
	}
}

// TestCmdRunOKKeepsOldCacheWhenConfigFetchFails proves the coordinator's
// own instruction: a failed cache refresh degrades the cache and is
// recorded as such, but never fails the run (which already succeeded)
// and never clears whatever was cached before.
func TestCmdRunOKKeepsOldCacheWhenConfigFetchFails(t *testing.T) {
	srv := testCoordinator(t,
		jsonHandler(http.StatusAccepted, runResponse("run-1", "my-macro", 6)),
		jsonHandler(http.StatusInternalServerError, map[string]any{"type": "t", "title": "internal error", "status": 500}),
	)
	dir := setupPlugin(t, srv, "tok")
	if err := updateMacroCache(dir, "my-macro", configResponse(5, "Old Label",
		configShowMacroStep{ID: "s1", LocalFallback: configShowMacroLocalFallback{Class: "silence", Reason: "old reason"}},
	), time.Now().Add(-time.Hour)); err != nil {
		t.Fatal(err)
	}

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitOK {
		t.Fatalf("exit code = %d, want %d (a cache-refresh failure must not fail an already-successful run), stderr=%s", code, exitOK, stderr.String())
	}
	if stderr.Len() == 0 {
		t.Error("expected a stderr note that the cache refresh failed, got none — a degraded cache must be recorded as such")
	}
	c, err := loadMacroCache(dir)
	if err != nil {
		t.Fatal(err)
	}
	entry := c.Macros["my-macro"]
	if entry.Label != "Old Label" || entry.MacroRevision != 5 {
		t.Errorf("cached entry = %+v, want the OLD entry preserved untouched (revision 5, Old Label)", entry)
	}
}

func TestCmdRunRefusedStatesUnknownPolicyWithNoCache(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/problem+json")
		w.WriteHeader(http.StatusForbidden)
		_, _ = w.Write([]byte(`{"type":"t","title":"forbidden","status":403,"detail":"missing scope show:macro:run"}`))
	}))
	defer srv.Close()

	dir := setupPlugin(t, srv, "tok-without-scope")
	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitRefused {
		t.Fatalf("exit code = %d, want %d", code, exitRefused)
	}

	rec, ok, err := loadStatus(dir)
	if err != nil || !ok {
		t.Fatalf("ok=%v err=%v", ok, err)
	}
	if rec.Class != classRefused || rec.HTTPStatus != 403 {
		t.Errorf("status = %+v, want class refused, http 403", rec)
	}
	// The exact sentence, not a substring search for "unknown" alone: see
	// cache_test.go's wantNoCacheStatement doc comment for why a
	// substring check on "unknown" (or on any of the three enum tokens)
	// silently passes a mutation that hardcodes the none-fallback's own
	// plain-text sentence into the no-cache branch.
	wantPolicy := wantNoCacheStatement("my-macro")
	if !strings.Contains(rec.Message, wantPolicy) {
		t.Errorf("message %q should contain the exact no-cache statement %q", rec.Message, wantPolicy)
	}

	buf, err := loadFailureBuffer(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(buf.Failures) != 1 || buf.Failures[0].Class != classRefused {
		t.Errorf("buffer = %+v, want one refused entry", buf.Failures)
	}
}

func TestCmdRunRefusedStatesCachedPolicy(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/problem+json")
		w.WriteHeader(http.StatusUnauthorized)
		_, _ = w.Write([]byte(`{"type":"t","title":"unauthorized","status":401,"detail":"no credential"}`))
	}))
	defer srv.Close()

	dir := setupPlugin(t, srv, "expired-tok")
	// Seed a cache as if an earlier successful run's config fetch had
	// populated it.
	if err := updateMacroCache(dir, "my-macro", configResponse(7, "Blackout Sequence",
		configShowMacroStep{ID: "blackout", LocalFallback: configShowMacroLocalFallback{
			Class: "silence", Reason: "deliberately no handover",
		}},
	), time.Now().Add(-time.Hour)); err != nil {
		t.Fatal(err)
	}

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitRefused {
		t.Fatalf("exit code = %d, want %d", code, exitRefused)
	}
	rec, _, err := loadStatus(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(rec.Message, "revision 7") || !strings.Contains(rec.Message, "silence") {
		t.Errorf("message %q should state the cached policy (revision 7, silence step)", rec.Message)
	}
	if !strings.Contains(rec.Message, "Blackout Sequence") {
		t.Errorf("message %q should name the macro's own cached LABEL, not only its id", rec.Message)
	}
	if !strings.Contains(rec.Message, "deliberately no handover") {
		t.Errorf("message %q should carry the cached step's own REASON text", rec.Message)
	}
}

func TestCmdRunRejectedIsNotReportedAsCredentialProblem(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/problem+json")
		w.WriteHeader(http.StatusNotFound)
		_, _ = w.Write([]byte(`{"type":"t","title":"resource not found","status":404,"detail":"unknown macro id"}`))
	}))
	defer srv.Close()

	dir := setupPlugin(t, srv, "tok")
	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "typo-macro"}, &stdout, &stderr, time.Now)
	if code != exitRejected {
		t.Fatalf("exit code = %d, want %d", code, exitRejected)
	}
	rec, _, err := loadStatus(dir)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Class != classRejected {
		t.Errorf("class = %q, want %q", rec.Class, classRejected)
	}
	if !strings.Contains(rec.Message, "NOT a credential problem") {
		t.Errorf("message %q must explicitly say this is not a credential problem, so a 404 does not send the operator to rotate a token", rec.Message)
	}
}

// TestCmdRunNonOKRetainsPriorBufferEntries pins the flush rule the
// specification calls out as the one a builder is most likely to get
// wrong: the buffer flushes on 2xx only. A 404 (rejected) must retain
// whatever was already buffered AND append its own new failure, never
// clear the buffer just because a request happened to be sent with a
// non-empty priorFailures field.
func TestCmdRunNonOKRetainsPriorBufferEntries(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/problem+json")
		w.WriteHeader(http.StatusNotFound)
		_, _ = w.Write([]byte(`{"type":"t","title":"resource not found","status":404,"detail":"unknown macro id"}`))
	}))
	defer srv.Close()

	dir := setupPlugin(t, srv, "tok")
	preexisting := bufferedFailure{MacroObjectID: "my-macro", Class: classUnreachable, At: time.Now().Add(-time.Hour)}
	if err := saveFailureBuffer(dir, failureBuffer{Failures: []bufferedFailure{preexisting}}); err != nil {
		t.Fatal(err)
	}

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitRejected {
		t.Fatalf("exit code = %d, want %d", code, exitRejected)
	}

	buf, err := loadFailureBuffer(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(buf.Failures) != 2 {
		t.Fatalf("buffer has %d entries after a non-2xx response, want 2 (the pre-existing one retained, plus this attempt's own)", len(buf.Failures))
	}
	if buf.Failures[0].Class != classUnreachable {
		t.Errorf("first buffered entry class = %q, want %q (the pre-existing entry, order preserved)", buf.Failures[0].Class, classUnreachable)
	}
	if buf.Failures[1].Class != classRejected {
		t.Errorf("second buffered entry class = %q, want %q (this attempt's own outcome)", buf.Failures[1].Class, classRejected)
	}
}

func TestCmdRunUnreachableOnClosedPort(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := l.Addr().String()
	if err := l.Close(); err != nil {
		t.Fatal(err)
	}

	dir := setupPluginWithCoordinatorURL(t, "tok", "http://"+addr)

	var stdout, stderr bytes.Buffer
	code := cmdRun([]string{"--config-dir", dir, "--timeout", "2s", "my-macro"}, &stdout, &stderr, time.Now)
	if code != exitUnreachable {
		t.Fatalf("exit code = %d, want %d", code, exitUnreachable)
	}
	rec, ok, err := loadStatus(dir)
	if err != nil || !ok {
		t.Fatalf("ok=%v err=%v", ok, err)
	}
	if rec.Class != classUnreachable || rec.HTTPStatus != 0 {
		t.Errorf("status = %+v, want class unreachable, http 0", rec)
	}
}

// TestARefusedAndAnUnreachableRecordAreVisiblyDifferent is this step's own
// pinned acceptance property, exercised end to end through cmdRun rather
// than only at the status.go layer: a 403 and a closed port must produce
// visibly different local records, with neither depending on any
// subsequent successful call.
func TestARefusedAndAnUnreachableRecordAreVisiblyDifferent(t *testing.T) {
	refusingSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/problem+json")
		w.WriteHeader(http.StatusForbidden)
		_, _ = w.Write([]byte(`{"type":"t","title":"forbidden","status":403,"detail":"missing scope"}`))
	}))
	defer refusingSrv.Close()
	refusedDir := setupPlugin(t, refusingSrv, "tok")
	var out1, err1 bytes.Buffer
	cmdRun([]string{"--config-dir", refusedDir, "my-macro"}, &out1, &err1, time.Now)
	refusedRec, _, err := loadStatus(refusedDir)
	if err != nil {
		t.Fatal(err)
	}

	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := l.Addr().String()
	_ = l.Close()
	unreachableDir := setupPluginWithCoordinatorURL(t, "tok", "http://"+addr)
	var out2, err2 bytes.Buffer
	cmdRun([]string{"--config-dir", unreachableDir, "--timeout", "2s", "my-macro"}, &out2, &err2, time.Now)
	unreachableRec, _, err := loadStatus(unreachableDir)
	if err != nil {
		t.Fatal(err)
	}

	if refusedRec.Class == unreachableRec.Class {
		t.Fatalf("both records classified as %q; a 403 and a closed port must be distinguishable", refusedRec.Class)
	}
	if refusedRec.HTTPStatus == unreachableRec.HTTPStatus {
		t.Errorf("both records carry httpStatus %d; expected 403 vs 0", refusedRec.HTTPStatus)
	}
}
