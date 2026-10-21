package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

func actionOKBody(id, actionID string) map[string]any {
	return map[string]any{"serverTime": "2026-01-01T00:00:00Z", "result": map[string]any{
		"id": id, "actionId": actionID, "revision": 3, "replay": false, "state": "resolved",
		"outcome": "confirmed", "outcomeReason": "device acknowledged",
	}}
}

// actionRecorder serves POST /api/v1/actions/{id}/invocations and keeps every raw body it saw.
type actionRecorder struct {
	mu     sync.Mutex
	bodies []string
	paths  []string
}

func (a *actionRecorder) handler(status int, body any) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		raw, _ := io.ReadAll(r.Body)
		a.mu.Lock()
		a.bodies = append(a.bodies, string(raw))
		a.paths = append(a.paths, r.URL.Path)
		a.mu.Unlock()
		w.WriteHeader(status)
		_ = json.NewEncoder(w).Encode(body)
	}
}

func actionServer(t *testing.T, h http.HandlerFunc) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/v1/actions/{id}/invocations", h)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

func TestCmdActionOK(t *testing.T) {
	rec := &actionRecorder{}
	srv := actionServer(t, rec.handler(http.StatusOK, actionOKBody("inv-1", "blackout")))
	dir := setupPlugin(t, srv, "tok")

	var stdout, stderr bytes.Buffer
	code := cmdAction([]string{"--config-dir", dir, "--", "blackout"}, &stdout, &stderr, time.Now)
	if code != exitOK {
		t.Fatalf("exit = %d, want %d; stderr: %s", code, exitOK, stderr.String())
	}
	if len(rec.paths) != 1 || rec.paths[0] != "/api/v1/actions/blackout/invocations" {
		t.Errorf("paths = %v", rec.paths)
	}
	st, ok, err := loadStatus(dir)
	if err != nil || !ok {
		t.Fatalf("ok=%v err=%v", ok, err)
	}
	if st.Class != classOK || st.ActionID != "blackout" || st.MacroID != "" || st.HTTPStatus != 200 {
		t.Errorf("status = %+v", st)
	}
	if !strings.Contains(st.Message, "inv-1") || !strings.Contains(st.Message, "confirmed") {
		t.Errorf("message = %q", st.Message)
	}
}

func TestCmdActionRefusalAndNotFound(t *testing.T) {
	cases := []struct {
		status    int
		wantExit  int
		wantClass string
	}{
		{http.StatusForbidden, exitRefused, classRefused},
		{http.StatusNotFound, exitRejected, classRejected},
	}
	for _, tc := range cases {
		rec := &actionRecorder{}
		srv := actionServer(t, rec.handler(tc.status, map[string]any{"title": "No", "detail": "not allowed"}))
		dir := setupPlugin(t, srv, "tok")

		var stdout, stderr bytes.Buffer
		code := cmdAction([]string{"--config-dir", dir, "blackout"}, &stdout, &stderr, time.Now)
		if code != tc.wantExit {
			t.Errorf("%d: exit = %d, want %d", tc.status, code, tc.wantExit)
		}
		st, _, _ := loadStatus(dir)
		if st.Class != tc.wantClass || st.HTTPStatus != tc.status || !strings.Contains(st.Message, "not allowed") {
			t.Errorf("%d: status = %+v", tc.status, st)
		}
		if tc.status == http.StatusNotFound && !strings.Contains(st.Message, "NOT a credential problem") {
			t.Errorf("404 message = %q", st.Message)
		}
	}
}

func TestCmdActionUnreachable(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := l.Addr().String()
	_ = l.Close()
	dir := setupPluginWithCoordinatorURL(t, "tok", "http://"+addr)

	var stdout, stderr bytes.Buffer
	code := cmdAction([]string{"--config-dir", dir, "--timeout", "2s", "blackout"}, &stdout, &stderr, time.Now)
	if code != exitUnreachable {
		t.Fatalf("exit = %d, want %d", code, exitUnreachable)
	}
	st, _, _ := loadStatus(dir)
	if st.Class != classUnreachable || st.HTTPStatus != 0 {
		t.Errorf("status = %+v", st)
	}
}

func TestCmdActionMissingIDIsUsageError(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := cmdAction([]string{"--config-dir", t.TempDir()}, &stdout, &stderr, time.Now); code != exitUsage {
		t.Errorf("exit = %d, want %d", code, exitUsage)
	}
}

func TestCmdActionBodyIsOnlyAFreshIdempotencyKey(t *testing.T) {
	rec := &actionRecorder{}
	srv := actionServer(t, rec.handler(http.StatusOK, actionOKBody("inv-1", "blackout")))
	dir := setupPlugin(t, srv, "tok")

	for i := 0; i < 2; i++ {
		var stdout, stderr bytes.Buffer
		if code := cmdAction([]string{"--config-dir", dir, "blackout"}, &stdout, &stderr, time.Now); code != exitOK {
			t.Fatalf("exit = %d; stderr: %s", code, stderr.String())
		}
	}
	keys := map[string]bool{}
	for _, raw := range rec.bodies {
		var m map[string]any
		if err := json.Unmarshal([]byte(raw), &m); err != nil {
			t.Fatal(err)
		}
		if len(m) != 1 {
			t.Errorf("body has keys beyond idempotencyKey: %s", raw)
		}
		k, _ := m["idempotencyKey"].(string)
		if k == "" || keys[k] {
			t.Errorf("idempotencyKey %q is empty or reused", k)
		}
		keys[k] = true
	}
	if len(keys) != 2 {
		t.Errorf("saw %d distinct keys, want 2", len(keys))
	}
}

func TestStatusPrintsActionID(t *testing.T) {
	dir := t.TempDir()
	if err := writeStatus(dir, statusRecord{Timestamp: time.Now(), ActionID: "blackout", Class: classOK, Message: "m"}); err != nil {
		t.Fatal(err)
	}
	var stdout, stderr bytes.Buffer
	if code := cmdStatus([]string{"--config-dir", dir}, &stdout, &stderr, time.Now); code != exitOK {
		t.Fatalf("exit = %d", code)
	}
	if !strings.Contains(stdout.String(), "action:  blackout") {
		t.Errorf("output = %q", stdout.String())
	}
}
