package main

import (
	"fmt"
	"strings"
	"time"
)

const macroCacheSchemaVersion = 2

// cachedStep is one step's cached local-policy statement: its id (the
// wire type carries no separate per-step label — only the macro itself
// has one, cached on macroCacheEntry.Label below), its localFallback
// class, and the reason text section 5.4 requires to be non-empty on
// every class. All three come from a GET of the macro's own
// configuration object, never from a run response: a run response's
// MacroRunStep carries stepId and localFallbackClass only, no reason
// (types.go's macroRunStep doc comment), and a class with no reason is
// close to the hardcoded constant section 8.1 forbids, not to the
// definition's own answer.
type cachedStep struct {
	StepID              string `json:"stepId"`
	LocalFallbackClass  string `json:"localFallbackClass"`
	LocalFallbackReason string `json:"localFallbackReason"`
}

// macroCacheEntry is one macro's cached definition: its own label (the
// prose an operator reads, e.g. "Begin set" — distinct from its object
// id) and its steps, captured from the last successful GET of
// /api/v1/config/show.macro/{id} for that macro.
type macroCacheEntry struct {
	Label         string       `json:"label"`
	MacroRevision int          `json:"macroRevision"`
	CachedAt      time.Time    `json:"cachedAt"`
	Steps         []cachedStep `json:"steps"`
}

// macroCache is keyed by macro object id: this plugin may be invoked
// against more than one macro over its lifetime (a different FPP command
// bound to a different macro), so a single-macro cache would silently
// discard what it knew about every macro except the last one run.
type macroCache struct {
	SchemaVersion int                        `json:"schemaVersion"`
	Macros        map[string]macroCacheEntry `json:"macros"`
}

func loadMacroCache(configDir string) (macroCache, error) {
	c := macroCache{Macros: map[string]macroCacheEntry{}}
	ok, err := readJSONFile(macroCachePath(configDir), &c)
	if err != nil {
		return macroCache{Macros: map[string]macroCacheEntry{}}, err
	}
	if !ok || c.Macros == nil {
		c.Macros = map[string]macroCacheEntry{}
	}
	return c, nil
}

// cachedRevisionFor returns the macro revision currently cached for
// macroID, and whether an entry exists at all. Used by run.go to decide
// whether a cache refresh is even worth a request: if the run that just
// executed pinned the same revision already cached, the cached label,
// classes and reasons are still current and re-fetching would cost a
// request for no new information.
func cachedRevisionFor(configDir, macroID string) (revision int, ok bool) {
	c, err := loadMacroCache(configDir)
	if err != nil {
		return 0, false
	}
	entry, ok := c.Macros[macroID]
	if !ok {
		return 0, false
	}
	return entry.MacroRevision, true
}

// updateMacroCache records macroID's label and its steps' labels,
// localFallback classes, and reasons from a GET of the macro's own
// configuration object (section 8.1's "last successful authenticated
// fetch" — a read, not the run response). Only ever called after that GET
// itself succeeded; a failed GET must leave whatever cache already exists
// untouched, which is why this function has no "clear on failure" path at
// all — callers that fail the GET simply never call this.
func updateMacroCache(configDir string, macroID string, cfg showMacroConfigResponse, now time.Time) error {
	c, err := loadMacroCache(configDir)
	if err != nil {
		// A corrupt or unreadable existing cache must not block recording
		// the fresh data this call has in hand — start clean rather than
		// refusing to cache at all.
		c = macroCache{Macros: map[string]macroCacheEntry{}}
	}
	// The wire type carries no separate per-step label (only a step id —
	// api/openapi.yaml's ConfigShowMacroStep has no "label" property), so
	// the step id remains this cache's own per-step identifier; what
	// changes from the run-response-derived design is the MACRO's own
	// label (cfg.Payload.Label, below) and each step's reason text, both
	// of which only this GET's payload carries.
	steps := make([]cachedStep, 0, len(cfg.Payload.Steps))
	for _, s := range cfg.Payload.Steps {
		steps = append(steps, cachedStep{
			StepID:              s.ID,
			LocalFallbackClass:  s.LocalFallback.Class,
			LocalFallbackReason: s.LocalFallback.Reason,
		})
	}
	c.SchemaVersion = macroCacheSchemaVersion
	c.Macros[macroID] = macroCacheEntry{
		Label:         cfg.Payload.Label,
		MacroRevision: cfg.Revision,
		CachedAt:      now,
		Steps:         steps,
	}
	return writeJSONFile(macroCachePath(configDir), c)
}

// localPolicyStatement builds the operator-facing text a refusal reports:
// what the macro's definition said should happen locally, per section 8.1.
// With no cached entry for macroID it says so plainly and does NOT
// substitute a default — an unknown policy stated as unknown, never a
// hardcoded "nothing runs locally" presented as though it were read from
// the definition.
func localPolicyStatement(configDir, macroID string, now time.Time) string {
	c, err := loadMacroCache(configDir)
	if err != nil {
		return fmt.Sprintf("local policy for macro %q is unknown: this plugin's cached macro definitions could not be read", macroID)
	}
	entry, ok := c.Macros[macroID]
	if !ok || len(entry.Steps) == 0 {
		return fmt.Sprintf("local policy for macro %q is unknown: no successful authenticated read of this macro's definition has been cached on this host yet", macroID)
	}

	name := entry.Label
	if name == "" {
		name = macroID
	}

	var b strings.Builder
	age := now.Sub(entry.CachedAt)
	fmt.Fprintf(&b, "local policy for macro %q (%s), from a definition cached %s ago at revision %d:",
		name, macroID, age.Round(time.Second), entry.MacroRevision)
	// entry.Steps is already in the order the definition declared them,
	// which the coordinator preserves on read.
	for _, s := range entry.Steps {
		fmt.Fprintf(&b, "\n  step %q: %s — %s", s.StepID, localFallbackClassPlainText(s.LocalFallbackClass), s.LocalFallbackReason)
	}
	return b.String()
}

// localFallbackClassPlainText renders a localFallback class value as an
// operator-facing sentence fragment. Falls back to the raw value for any
// class this build does not recognize, rather than a raw enum string.
func localFallbackClassPlainText(class string) string {
	switch class {
	case "none":
		return "nothing runs locally if the coordinator is unreachable"
	case "coordinator-required":
		return "this step runs on the coordinator and cannot run locally"
	case "silence":
		return "the deliberate local behaviour is silence, not a handover"
	default:
		return fmt.Sprintf("local behaviour %q (unrecognized by this build)", class)
	}
}
