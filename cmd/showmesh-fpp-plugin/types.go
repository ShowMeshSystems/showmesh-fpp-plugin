package main

import "time"

// The types below are this program's own, independent decode of the subset
// of api/openapi.yaml this program actually speaks: POST
// /api/v1/macros/{id}/runs and the RFC 9457 problem document any non-2xx
// response carries. Deliberately not shared with the coordinator — see
// doc.go and importgraph_test.go for why:
// a JSON tag rename on the server must break this build, not silently
// rename the field on both sides of a shared type at once.
//
// None of these decoders set json.Decoder.DisallowUnknownFields: the
// contract is additive-only within a major version, and a strict decoder
// would make this program break the moment the coordinator it talks to
// adds a field.

// createMacroRunRequest is the body of POST /api/v1/macros/{id}/runs.
type createMacroRunRequest struct {
	IdempotencyKey string `json:"idempotencyKey"`
	// Trigger is always "plugin" for this program: it is the FPP plugin's
	// own value in the trigger enum (api, plugin, cli, ui).
	Trigger string `json:"trigger"`
	// PriorFailures and PriorFailuresDropped are omitted entirely (not
	// sent as an explicit empty array / zero) when this program's failure
	// buffer is empty, which is the same thing an empty buffer means on
	// the wire — there is nothing degraded to report.
	PriorFailures        []macroPriorFailureRequest `json:"priorFailures,omitempty"`
	PriorFailuresDropped int                        `json:"priorFailuresDropped,omitempty"`
}

// macroPriorFailureRequest is one element of createMacroRunRequest.priorFailures.
// class is restricted to the three degraded outcomes; "ok" is never
// buffered because there is nothing degraded to report about it.
type macroPriorFailureRequest struct {
	MacroObjectID string    `json:"macroObjectId"`
	Class         string    `json:"class"` // refused | rejected | unreachable
	HTTPStatus    int       `json:"httpStatus"`
	At            time.Time `json:"at"`
}

// macroRun is the subset of MacroRun this program reads from a submit
// response: enough to record what was accepted and to decide whether the
// macro cache needs refreshing (cache.go's cachedRevisionFor compares
// against MacroRevision). It deliberately does NOT decode the run's own
// "steps" array: an earlier version of this program cached the refusal
// statement from that array (stepId + localFallbackClass), but MacroRunStep
// carries no per-step reason and MacroRun carries no macro-level label —
// both of which the definition's own show.macro configuration object DOES
// carry, and section 8.1 wants the definition's own answer, not a class
// with no explanation attached. See showMacroConfigResponse below and
// run.go's cache-refresh logic.
type macroRun struct {
	ID            string `json:"id"`
	MacroObjectID string `json:"macroObjectId"`
	MacroRevision int    `json:"macroRevision"`
}

// macroRunSubmitResponse is the success (2xx) body of
// POST /api/v1/macros/{id}/runs.
type macroRunSubmitResponse struct {
	ServerTime time.Time `json:"serverTime"`
	Run        macroRun  `json:"run"`
	Replay     bool      `json:"replay"`
}

// configShowMacroLocalFallback mirrors api/openapi.yaml's
// ConfigShowMacroLocalFallback: one step's required localFallback object.
type configShowMacroLocalFallback struct {
	Class  string `json:"class"`
	Reason string `json:"reason"`
}

// configShowMacroStep mirrors ConfigShowMacroStep. This program reads only
// id and localFallback; action/onFailure/onUnconfirmed are decoded by
// nobody here and silently ignored, which additive-only promises is safe.
type configShowMacroStep struct {
	ID            string                       `json:"id"`
	LocalFallback configShowMacroLocalFallback `json:"localFallback"`
}

// configShowMacro mirrors ConfigShowMacro, the "show.macro" configuration
// kind's decoded payload. Label is the prose an operator reads (distinct
// from the macro's own object id); this program reads nothing else here.
type configShowMacro struct {
	Label string                `json:"label"`
	Steps []configShowMacroStep `json:"steps"`
}

// showMacroConfigResponse mirrors ShowMacroConfigResponse, the body of
// GET /api/v1/config/show.macro/{id}. Revision is the macro's CURRENT
// revision as of this read — not necessarily the same value as a run's own
// pinned MacroRevision, since the two are read at different times, though
// in the common case (this program fetches only right after its own run
// submission) they usually agree.
type showMacroConfigResponse struct {
	ServerTime time.Time       `json:"serverTime"`
	ID         string          `json:"id"`
	Revision   int             `json:"revision"`
	Payload    configShowMacro `json:"payload"`
}

// problemDoc is an RFC 9457 application/problem+json document. Title and
// detail carry the operator-relevant text; this program never guesses at
// meaning from the HTTP status code alone.
type problemDoc struct {
	Type   string `json:"type"`
	Title  string `json:"title"`
	Status int    `json:"status"`
	Detail string `json:"detail"`
}
