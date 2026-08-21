package main

import (
	"testing"
	"time"
)

func TestFailureBufferPrunesByCountOldestFirst(t *testing.T) {
	var b failureBuffer
	base := time.Date(2026, 8, 14, 12, 0, 0, 0, time.UTC)
	for i := 0; i < maxBufferedFailures+5; i++ {
		b.append(bufferedFailure{MacroObjectID: "m", Class: classUnreachable, At: base.Add(time.Duration(i) * time.Minute)}, base.Add(time.Duration(i)*time.Minute))
	}
	if len(b.Failures) != maxBufferedFailures {
		t.Fatalf("len(Failures) = %d, want %d", len(b.Failures), maxBufferedFailures)
	}
	if b.Dropped != 5 {
		t.Errorf("Dropped = %d, want 5", b.Dropped)
	}
	// Oldest-first: the surviving entries must be the LAST maxBufferedFailures
	// appended, i.e. the earliest surviving entry is index 5's timestamp.
	wantFirst := base.Add(5 * time.Minute)
	if !b.Failures[0].At.Equal(wantFirst) {
		t.Errorf("oldest surviving entry At = %v, want %v (the 5 oldest should have been dropped)", b.Failures[0].At, wantFirst)
	}
}

func TestFailureBufferPrunesByAge(t *testing.T) {
	var b failureBuffer
	now := time.Date(2026, 8, 14, 12, 0, 0, 0, time.UTC)
	old := now.Add(-maxFailureAge - time.Hour)
	recent := now.Add(-time.Minute)

	b.append(bufferedFailure{MacroObjectID: "m", Class: classRefused, At: old}, old)
	b.append(bufferedFailure{MacroObjectID: "m", Class: classRefused, At: recent}, recent)

	// Neither append call knows "now" beyond its own At value above; prune
	// again with the real current time to exercise the age cutoff as a
	// caller (run.go) actually would, immediately before building a
	// request.
	b.pruneByAge(now)

	if len(b.Failures) != 1 {
		t.Fatalf("len(Failures) = %d, want 1 (the old entry should have aged out)", len(b.Failures))
	}
	if !b.Failures[0].At.Equal(recent) {
		t.Errorf("surviving entry At = %v, want %v", b.Failures[0].At, recent)
	}
	if b.Dropped != 1 {
		t.Errorf("Dropped = %d, want 1 (the age-based drop must be counted, not silent)", b.Dropped)
	}
}

func TestFailureBufferAsPriorFailuresEmptyIsNilNotEmptySlice(t *testing.T) {
	var b failureBuffer
	failures, dropped := b.asPriorFailures()
	if failures != nil {
		t.Errorf("failures = %#v, want nil for an empty buffer (so the wire field is omitted, not an explicit empty array)", failures)
	}
	if dropped != 0 {
		t.Errorf("dropped = %d, want 0", dropped)
	}
}

func TestFailureBufferAsPriorFailuresRoundTrips(t *testing.T) {
	var b failureBuffer
	now := time.Now().UTC()
	b.append(bufferedFailure{MacroObjectID: "macro-a", Class: classRefused, HTTPStatus: 403, At: now}, now)

	failures, dropped := b.asPriorFailures()
	if len(failures) != 1 {
		t.Fatalf("len(failures) = %d, want 1", len(failures))
	}
	if failures[0].MacroObjectID != "macro-a" || failures[0].Class != classRefused || failures[0].HTTPStatus != 403 {
		t.Errorf("failures[0] = %+v, want macro-a/refused/403", failures[0])
	}
	if dropped != 0 {
		t.Errorf("dropped = %d, want 0", dropped)
	}
}

func TestFailureBufferLoadSaveRoundTrip(t *testing.T) {
	dir := t.TempDir()
	now := time.Now().UTC()
	b := failureBuffer{}
	b.append(bufferedFailure{MacroObjectID: "m", Class: classRejected, HTTPStatus: 404, At: now}, now)

	if err := saveFailureBuffer(dir, b); err != nil {
		t.Fatal(err)
	}
	loaded, err := loadFailureBuffer(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(loaded.Failures) != 1 || loaded.Failures[0].MacroObjectID != "m" {
		t.Errorf("loaded buffer = %+v, want one failure for macro m", loaded)
	}
}

// TestAsPriorFailuresExcludesNonWireClasses is the regression guard for
// the coordinator's own request: this program's classLocalError must
// never be able to leak into priorFailures on the wire. No production
// call site ever constructs a bufferedFailure with a non-wire class
// (reportLocalError, the only path that produces classLocalError, never
// touches this buffer at all — run.go), so this test constructs one
// directly, bypassing append entirely, to simulate what a future defect
// in this package would look like: something that got a non-wire class
// into the buffer anyway. asPriorFailures is the actual wire boundary,
// and this asserts it excludes the entry rather than forwarding it.
func TestAsPriorFailuresExcludesNonWireClasses(t *testing.T) {
	now := time.Now().UTC()
	b := failureBuffer{Failures: []bufferedFailure{
		{MacroObjectID: "m", Class: classRefused, At: now},
		{MacroObjectID: "m", Class: classLocalError, At: now}, // must never reach the wire
	}}

	failures, dropped := b.asPriorFailures()

	if len(failures) != 1 {
		t.Fatalf("asPriorFailures returned %d entries, want 1 (only the wire-valid one)", len(failures))
	}
	for _, f := range failures {
		if f.Class == classLocalError {
			t.Fatalf("classLocalError leaked into the wire payload: %+v", failures)
		}
		if !isWireFailureClass(f.Class) {
			t.Fatalf("a non-wire class reached the wire payload: %+v", failures)
		}
	}
	if dropped != 1 {
		t.Errorf("dropped = %d, want 1 (the excluded classLocalError entry must still be counted, not silently discarded)", dropped)
	}
}

func TestIsWireFailureClassClosedSet(t *testing.T) {
	for _, c := range []string{classRefused, classRejected, classUnreachable} {
		if !isWireFailureClass(c) {
			t.Errorf("isWireFailureClass(%q) = false, want true", c)
		}
	}
	for _, c := range []string{classOK, classLocalError, "", "unclassified", "made-up-class"} {
		if isWireFailureClass(c) {
			t.Errorf("isWireFailureClass(%q) = true, want false", c)
		}
	}
}

func TestFailureBufferLoadMissingFileIsEmptyNotError(t *testing.T) {
	dir := t.TempDir()
	b, err := loadFailureBuffer(dir)
	if err != nil {
		t.Fatalf("unexpected error loading a nonexistent buffer file: %v", err)
	}
	if len(b.Failures) != 0 {
		t.Errorf("expected an empty buffer, got %+v", b)
	}
}
