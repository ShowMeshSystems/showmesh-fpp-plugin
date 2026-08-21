package main

import (
	"bytes"
	"strings"
	"testing"
	"time"
)

func TestStatusWriteLoadRoundTrip(t *testing.T) {
	dir := t.TempDir()
	rec := statusRecord{
		Timestamp: time.Date(2026, 8, 14, 17, 0, 0, 0, time.UTC),
		MacroID:   "m", Class: classRefused, HTTPStatus: 403, Message: "refused",
	}
	if err := writeStatus(dir, rec); err != nil {
		t.Fatal(err)
	}
	loaded, ok, err := loadStatus(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !ok {
		t.Fatal("expected a status record to be present")
	}
	if loaded.MacroID != "m" || loaded.Class != classRefused || loaded.HTTPStatus != 403 {
		t.Errorf("loaded = %+v, want macro m / refused / 403", loaded)
	}
}

func TestStatusOverwritesPreviousRecord(t *testing.T) {
	dir := t.TempDir()
	if err := writeStatus(dir, statusRecord{MacroID: "m", Class: classOK}); err != nil {
		t.Fatal(err)
	}
	if err := writeStatus(dir, statusRecord{MacroID: "m", Class: classUnreachable}); err != nil {
		t.Fatal(err)
	}
	loaded, ok, err := loadStatus(dir)
	if err != nil || !ok {
		t.Fatalf("ok=%v err=%v", ok, err)
	}
	if loaded.Class != classUnreachable {
		t.Errorf("class = %q, want %q — status.json must reflect the LATEST attempt only", loaded.Class, classUnreachable)
	}
}

func TestCmdStatusNoRecordYet(t *testing.T) {
	dir := t.TempDir()
	var stdout, stderr bytes.Buffer
	code := cmdStatus([]string{"--config-dir", dir}, &stdout, &stderr, time.Now)
	if code != exitOK {
		t.Errorf("exit code = %d, want %d (no record yet is not an error)", code, exitOK)
	}
	if !strings.Contains(stdout.String(), "no run has been attempted") {
		t.Errorf("stdout = %q, want it to say no run has been attempted", stdout.String())
	}
}

func TestCmdStatusPrintsRefusedAndUnreachableDifferently(t *testing.T) {
	// This is the acceptance property the whole step is protected around:
	// a 403 and a closed port must produce visibly different local
	// records, readable with no coordinator involved.
	dir := t.TempDir()
	if err := writeStatus(dir, statusRecord{
		Timestamp: time.Now(), MacroID: "m", Class: classRefused, HTTPStatus: 403, Message: "refused: missing scope",
	}); err != nil {
		t.Fatal(err)
	}
	var refusedOut, refusedErr bytes.Buffer
	cmdStatus([]string{"--config-dir", dir}, &refusedOut, &refusedErr, time.Now)

	if err := writeStatus(dir, statusRecord{
		Timestamp: time.Now(), MacroID: "m", Class: classUnreachable, HTTPStatus: 0, Message: "unreachable: connection refused",
	}); err != nil {
		t.Fatal(err)
	}
	var unreachableOut, unreachableErr bytes.Buffer
	cmdStatus([]string{"--config-dir", dir}, &unreachableOut, &unreachableErr, time.Now)

	if refusedOut.String() == unreachableOut.String() {
		t.Fatalf("refused and unreachable status output must differ; both rendered as: %q", refusedOut.String())
	}
	if !strings.Contains(refusedOut.String(), classRefused) {
		t.Errorf("refused output does not name its class: %q", refusedOut.String())
	}
	if !strings.Contains(unreachableOut.String(), classUnreachable) {
		t.Errorf("unreachable output does not name its class: %q", unreachableOut.String())
	}
	if strings.Contains(unreachableOut.String(), "http:") {
		t.Errorf("unreachable output should not print an http status line when there was none: %q", unreachableOut.String())
	}
}
