package main

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

func fixtureMacroConfig(revision int, label string, steps ...configShowMacroStep) showMacroConfigResponse {
	return showMacroConfigResponse{
		Revision: revision,
		Payload:  configShowMacro{Label: label, Steps: steps},
	}
}

// wantNoCacheStatement reconstructs the EXACT text localPolicyStatement's
// no-cache branch must render for macroID. Used for an exact-equality
// assertion rather than a substring search, per the reviewer finding this
// file was rewritten to answer: a mutation that hardcodes the
// none-fallback's own plain-text sentence ("nothing runs locally if the
// coordinator is unreachable" — localFallbackClassPlainText's own output
// for "none") into the no-cache branch contains NEITHER the word
// "unknown" NOR any of the three enum tokens the old guard searched for,
// so a substring check for either one passes on that exact mutation. Only
// an exact match on the real sentence — or, equivalently, asserting the
// absence of any per-step line, which this file's tests also do — catches
// it. See TestLocalPolicyStatementNoCache's own doc comment for the
// mutation this was run against to confirm that.
func wantNoCacheStatement(macroID string) string {
	return fmt.Sprintf("local policy for macro %q is unknown: no successful authenticated read of this macro's definition has been cached on this host yet", macroID)
}

// TestLocalPolicyStatementNoCache asserts EXACT equality against the
// real no-cache sentence (wantNoCacheStatement), not a substring search.
// The previous version of this test searched for the three enum tokens
// ("coordinator-required", "\"none\"", "silence") and separately for the
// word "unknown", and BOTH checks silently passed when the no-cache
// branch was replaced with the hardcoded default text section 8.1
// forbids substituting ("nothing runs locally if the coordinator is
// unreachable"): that sentence contains none of the enum tokens (it is
// prose, not an enum value) and does not contain the word "unknown"
// either. Verified by hand: temporarily replacing this function's
// `return fmt.Sprintf(...)` in cache.go with
// `return "nothing runs locally if the coordinator is unreachable"` and
// rerunning this test turns it from passing to failing — confirmed, then
// reverted.
func TestLocalPolicyStatementNoCache(t *testing.T) {
	dir := t.TempDir()
	got := localPolicyStatement(dir, "unknown-macro", time.Now())
	want := wantNoCacheStatement("unknown-macro")
	if got != want {
		t.Errorf("localPolicyStatement with no cache =\n\t%q\nwant exactly\n\t%q", got, want)
	}
	// Belt-and-braces structural check, independent of the exact wording
	// above: a genuinely cached statement always renders at least one
	// per-step line (see the step loop below, "\n  step %q: ..."),
	// because a cache hit requires entry.Steps to be non-empty. The
	// no-cache branch must never render one, regardless of what prose it
	// uses.
	if strings.Contains(got, "\n  step ") {
		t.Errorf("no-cache statement contains a per-step line, which must never happen with nothing cached: %q", got)
	}
}

func TestUpdateMacroCacheThenLocalPolicyStatement(t *testing.T) {
	dir := t.TempDir()
	now := time.Date(2026, 8, 14, 17, 0, 0, 0, time.UTC)
	cfg := fixtureMacroConfig(4, "Begin Set",
		configShowMacroStep{ID: "projectors", LocalFallback: configShowMacroLocalFallback{
			Class: "coordinator-required", Reason: "projector control runs on the coordinator",
		}},
		configShowMacroStep{ID: "start", LocalFallback: configShowMacroLocalFallback{
			Class: "none", Reason: "no local delivery path exists for this step",
		}},
	)
	if err := updateMacroCache(dir, "projectors-on", cfg, now); err != nil {
		t.Fatal(err)
	}

	later := now.Add(90 * time.Minute)
	got := localPolicyStatement(dir, "projectors-on", later)

	if !strings.Contains(got, "Begin Set") {
		t.Errorf("statement does not name the macro's own LABEL (the prose an operator reads, not just its id): %q", got)
	}
	if !strings.Contains(got, "projectors-on") {
		t.Errorf("statement does not name the macro id: %q", got)
	}
	if !strings.Contains(got, "revision 4") {
		t.Errorf("statement does not name the cached revision: %q", got)
	}
	if !strings.Contains(got, `"projectors"`) || !strings.Contains(got, "cannot run locally") {
		t.Errorf("statement does not describe the coordinator-required step: %q", got)
	}
	if !strings.Contains(got, "projector control runs on the coordinator") {
		t.Errorf("statement does not carry the DEFINITION'S OWN reason text for the coordinator-required step: %q", got)
	}
	if !strings.Contains(got, `"start"`) || !strings.Contains(got, "nothing runs locally") {
		t.Errorf("statement does not describe the none-fallback step: %q", got)
	}
	if !strings.Contains(got, "no local delivery path exists for this step") {
		t.Errorf("statement does not carry the definition's own reason text for the none-fallback step: %q", got)
	}
}

func TestLocalPolicyStatementFallsBackToMacroIDWhenLabelIsEmpty(t *testing.T) {
	// A definition with an empty label (allowed on the wire — ConfigShowMacro
	// requires the key but not a non-empty value) must not render as a
	// literal empty string where the macro's name should read.
	dir := t.TempDir()
	now := time.Now()
	cfg := fixtureMacroConfig(1, "", configShowMacroStep{ID: "s1", LocalFallback: configShowMacroLocalFallback{Class: "silence", Reason: "r"}})
	if err := updateMacroCache(dir, "macro-x", cfg, now); err != nil {
		t.Fatal(err)
	}
	got := localPolicyStatement(dir, "macro-x", now)
	if !strings.Contains(got, "macro-x") {
		t.Errorf("statement should fall back to the macro id when label is empty: %q", got)
	}
}

func TestLocalPolicyStatementDoesNotConfuseDifferentMacros(t *testing.T) {
	dir := t.TempDir()
	now := time.Now()
	cfg := fixtureMacroConfig(1, "Macro A", configShowMacroStep{ID: "s1", LocalFallback: configShowMacroLocalFallback{Class: "silence", Reason: "r"}})
	if err := updateMacroCache(dir, "macro-a", cfg, now); err != nil {
		t.Fatal(err)
	}

	got := localPolicyStatement(dir, "macro-b", now)
	want := wantNoCacheStatement("macro-b")
	if got != want {
		t.Errorf("statement for uncached macro-b =\n\t%q\nwant exactly\n\t%q (must not leak macro-a's cached policy)", got, want)
	}
}

func TestCachedRevisionFor(t *testing.T) {
	dir := t.TempDir()
	if _, ok := cachedRevisionFor(dir, "macro-a"); ok {
		t.Fatal("expected no cached revision before anything is cached")
	}
	cfg := fixtureMacroConfig(9, "Macro A")
	if err := updateMacroCache(dir, "macro-a", cfg, time.Now()); err != nil {
		t.Fatal(err)
	}
	rev, ok := cachedRevisionFor(dir, "macro-a")
	if !ok || rev != 9 {
		t.Errorf("cachedRevisionFor = (%d, %v), want (9, true)", rev, ok)
	}
	if _, ok := cachedRevisionFor(dir, "macro-b"); ok {
		t.Error("expected no cached revision for a different, never-cached macro id")
	}
}
