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

func nightCoordinator(t *testing.T, handler http.HandlerFunc) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/v1/night/commands/{command}", handler)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

func readNightStatus(t *testing.T, stateDir string) nightStatusRecord {
	t.Helper()
	raw, err := os.ReadFile(nightStatusPath(stateDir))
	if err != nil {
		t.Fatalf("reading night status record: %v", err)
	}
	var rec nightStatusRecord
	if err := json.Unmarshal(raw, &rec); err != nil {
		t.Fatalf("decoding night status record: %v", err)
	}
	return rec
}

func writeNightAccepted(w http.ResponseWriter, command, outcome, state string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusAccepted)
	_ = json.NewEncoder(w).Encode(nightCommandResponse{
		Command: nightCommandResult{Command: command, Outcome: outcome},
		Session: nightSessionState{State: state},
	})
}

func writeProblem(w http.ResponseWriter, status int, title, detail string) {
	w.Header().Set("Content-Type", "application/problem+json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(problemDoc{Title: title, Status: status, Detail: detail})
}

func TestCmdNightStartNightAccepted(t *testing.T) {
	var gotPath, gotAuth, gotBody string
	srv := nightCoordinator(t, func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		gotAuth = r.Header.Get("Authorization")
		var buf bytes.Buffer
		_, _ = buf.ReadFrom(r.Body)
		gotBody = buf.String()
		writeNightAccepted(w, r.PathValue("command"), "applied", "transition-to-show")
	})
	stateDir := setupPlugin(t, srv, "tok")

	var stdout, stderr bytes.Buffer
	code := run([]string{"night", "--config-dir", stateDir, "start-night"}, &stdout, &stderr, fixedClock(time.Unix(1000, 0)))
	if code != exitOK {
		t.Fatalf("exit %d, stderr %q", code, stderr.String())
	}
	if gotPath != "/api/v1/night/commands/start-night" || gotAuth != "Bearer tok" || gotBody != "{}" {
		t.Fatalf("request path %q auth %q body %q", gotPath, gotAuth, gotBody)
	}
	rec := readNightStatus(t, stateDir)
	if rec.Class != classOK || rec.Command != "start-night" || rec.Outcome != "applied" || rec.SessionState != "transition-to-show" {
		t.Fatalf("unexpected record %+v", rec)
	}
	if !strings.Contains(stdout.String(), "accepted start-night") {
		t.Fatalf("stdout %q", stdout.String())
	}
	if _, err := os.Stat(statusPath(stateDir)); !os.IsNotExist(err) {
		t.Fatalf("night command must not write the macro status record, stat err %v", err)
	}
}

func TestCmdNightDuplicateIsReportedAsNoChange(t *testing.T) {
	srv := nightCoordinator(t, func(w http.ResponseWriter, r *http.Request) {
		writeNightAccepted(w, "prepare-site", "idempotent_no_op", "preparing")
	})
	stateDir := setupPlugin(t, srv, "tok")
	var stdout, stderr bytes.Buffer
	if code := run([]string{"night", "--config-dir", stateDir, "prepare-site"}, &stdout, &stderr, time.Now); code != exitOK {
		t.Fatalf("exit %d", code)
	}
	if rec := readNightStatus(t, stateDir); rec.Outcome != "idempotent_no_op" || !strings.Contains(rec.Message, "nothing changed") {
		t.Fatalf("unexpected record %+v", rec)
	}
}

func TestCmdNightRefusalByStateRecordsCoordinatorMessage(t *testing.T) {
	srv := nightCoordinator(t, func(w http.ResponseWriter, r *http.Request) {
		writeProblem(w, http.StatusConflict, "Night not ready", "Readiness has not run for today. Run readiness first.")
	})
	stateDir := setupPlugin(t, srv, "tok")
	var stdout, stderr bytes.Buffer
	if code := run([]string{"night", "--config-dir", stateDir, "start-night"}, &stdout, &stderr, time.Now); code != exitRejected {
		t.Fatalf("exit %d", code)
	}
	rec := readNightStatus(t, stateDir)
	if rec.Class != classRejected || rec.HTTPStatus != 409 || !strings.Contains(rec.Message, "Run readiness first.") {
		t.Fatalf("unexpected record %+v", rec)
	}
}

func TestCmdNightCredentialRefused(t *testing.T) {
	srv := nightCoordinator(t, func(w http.ResponseWriter, r *http.Request) {
		writeProblem(w, http.StatusForbidden, "Forbidden", "")
	})
	stateDir := setupPlugin(t, srv, "tok")
	var stdout, stderr bytes.Buffer
	if code := run([]string{"night", "--config-dir", stateDir, "fade-out-night"}, &stdout, &stderr, time.Now); code != exitRefused {
		t.Fatalf("exit %d", code)
	}
	if rec := readNightStatus(t, stateDir); rec.Class != classRefused {
		t.Fatalf("unexpected record %+v", rec)
	}
}

func TestCmdNightUnreachable(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := ln.Addr().String()
	_ = ln.Close()
	stateDir := setupPluginWithCoordinatorURL(t, "tok", "http://"+addr)
	var stdout, stderr bytes.Buffer
	if code := run([]string{"night", "--config-dir", stateDir, "run-readiness"}, &stdout, &stderr, time.Now); code != exitUnreachable {
		t.Fatalf("exit %d", code)
	}
	if rec := readNightStatus(t, stateDir); rec.Class != classUnreachable || rec.HTTPStatus != 0 {
		t.Fatalf("unexpected record %+v", rec)
	}
}

func TestCmdNightUnconfirmedBodyIsNotAccepted(t *testing.T) {
	srv := nightCoordinator(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusAccepted)
		_, _ = w.Write([]byte("{}"))
	})
	stateDir := setupPlugin(t, srv, "tok")
	var stdout, stderr bytes.Buffer
	if code := run([]string{"night", "--config-dir", stateDir, "start-preshow"}, &stdout, &stderr, time.Now); code != exitUnreachable {
		t.Fatalf("exit %d", code)
	}
}

func TestCmdNightRejectsCommandsOutsideTheSchedule(t *testing.T) {
	for _, args := range [][]string{{"night"}, {"night", "end-session"}, {"night", "resume-show"}, {"night", "start-night", "extra"}, {"night", "start-night", "extra", "--stop-playlists"}} {
		var stdout, stderr bytes.Buffer
		if code := run(args, &stdout, &stderr, time.Now); code != exitUsage {
			t.Fatalf("%v: exit %d", args, code)
		}
	}
}

func nightRequestBodyFor(t *testing.T, args ...string) (body string, code int, stderr string) {
	t.Helper()
	srv := nightCoordinator(t, func(w http.ResponseWriter, r *http.Request) {
		var buf bytes.Buffer
		_, _ = buf.ReadFrom(r.Body)
		body = buf.String()
		writeNightAccepted(w, r.PathValue("command"), "applied", "preparing")
	})
	stateDir := setupPlugin(t, srv, "tok")
	var stdout, errOut bytes.Buffer
	code = run(append([]string{"night", "--config-dir", stateDir}, args...), &stdout, &errOut, time.Now)
	return body, code, errOut.String()
}

func TestCmdNightPrepareSiteBodyIsUnchangedWithoutStopPlaylists(t *testing.T) {
	body, code, stderr := nightRequestBodyFor(t, "prepare-site")
	if code != exitOK || body != "{}" {
		t.Fatalf("exit %d body %q stderr %q, want exit 0 and the body {}", code, body, stderr)
	}
}

func TestCmdNightPrepareSiteStopPlaylistsCarriesTheFlag(t *testing.T) {
	for _, args := range [][]string{{"--stop-playlists", "prepare-site"}, {"prepare-site", "--stop-playlists"}} {
		body, code, stderr := nightRequestBodyFor(t, args...)
		if code != exitOK || body != `{"stopFppPlayback":true}` {
			t.Fatalf("%v: exit %d body %q stderr %q", args, code, body, stderr)
		}
	}
}

func TestCmdNightStopPlaylistsIsRefusedOnEveryOtherCommand(t *testing.T) {
	for _, command := range nightCommands {
		if command == "prepare-site" {
			continue
		}
		for _, args := range [][]string{{"--stop-playlists", command}, {command, "--stop-playlists"}} {
			body, code, stderr := nightRequestBodyFor(t, args...)
			if code != exitUsage || body != "" || !strings.Contains(stderr, "applies only to prepare-site") {
				t.Fatalf("%v: exit %d body %q stderr %q, want a usage refusal and no request", args, code, body, stderr)
			}
		}
	}
}

func TestCmdNightReportsTheStopResultFromTheCoordinator(t *testing.T) {
	srv := nightCoordinator(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusAccepted)
		_ = json.NewEncoder(w).Encode(nightCommandResponse{
			Command: nightCommandResult{Command: "prepare-site", Outcome: "applied", Reason: "Stopped what FPP was playing on player-01."},
			Session: nightSessionState{State: "preparing"},
		})
	})
	stateDir := setupPlugin(t, srv, "tok")
	var stdout, stderr bytes.Buffer
	if code := run([]string{"night", "--config-dir", stateDir, "prepare-site", "--stop-playlists"}, &stdout, &stderr, time.Now); code != exitOK {
		t.Fatalf("exit %d", code)
	}
	if rec := readNightStatus(t, stateDir); !strings.Contains(rec.Message, "Stopped what FPP was playing on player-01.") {
		t.Fatalf("message %q does not carry the stop result", rec.Message)
	}
}
