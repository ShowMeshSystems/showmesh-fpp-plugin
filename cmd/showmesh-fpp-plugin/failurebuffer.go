package main

import "time"

const failureBufferSchemaVersion = 1

// maxBufferedFailures and maxFailureAge bound the failure buffer in count
// and in age (section 8.3 path 2). SHOWMESH HYPOTHESIS, NOT MEASURED — no
// bench has sized how often a real schedule fires this plugin against a
// refusing or unreachable coordinator, so these are chosen only to make
// the buffer file's own size and the age of what it reports bounded rather
// than unbounded, matching this project's own retention lesson: an
// unbounded write on a failure path evicts the evidence it exists to
// preserve, just at the opposite end (a buffer that grows forever on a
// host with a stuck credential is the same shape of problem the
// coordinator's own event-history bound exists to prevent).
const (
	maxBufferedFailures = 50
	maxFailureAge       = 30 * 24 * time.Hour
)

// bufferedFailure is one degraded outcome this plugin recorded locally and
// has not yet reported to the coordinator. class is restricted to the
// three degraded classes; classOK is never buffered.
type bufferedFailure struct {
	MacroObjectID string    `json:"macroObjectId"`
	Class         string    `json:"class"`
	HTTPStatus    int       `json:"httpStatus"`
	At            time.Time `json:"at"`
}

// failureBuffer is section 8.3 path 2's on-disk state: everything degraded
// that has happened since the last successful, authenticated run
// submission, plus a count of how many entries have been dropped to stay
// within the bounds above. Dropped is never silently reset to zero except
// by a successful flush — a truncated buffer records the fact that it was
// truncated (a silently truncated
// failure history is a smaller version of the same lie an unbounded write
// would be).
type failureBuffer struct {
	SchemaVersion int               `json:"schemaVersion"`
	Failures      []bufferedFailure `json:"failures"`
	Dropped       int               `json:"dropped"`
}

func loadFailureBuffer(configDir string) (failureBuffer, error) {
	var b failureBuffer
	_, err := readJSONFile(failureBufferPath(configDir), &b)
	if err != nil {
		return failureBuffer{}, err
	}
	b.SchemaVersion = failureBufferSchemaVersion
	return b, nil
}

func saveFailureBuffer(configDir string, b failureBuffer) error {
	b.SchemaVersion = failureBufferSchemaVersion
	return writeJSONFile(failureBufferPath(configDir), b)
}

// append adds f to the buffer, then prunes by age and by count, oldest
// first, incrementing Dropped for every entry it removes either way. The
// buffer is kept ordered oldest-first (append order), which is what makes
// "drop oldest" a slice-front trim rather than a sort.
func (b *failureBuffer) append(f bufferedFailure, now time.Time) {
	b.Failures = append(b.Failures, f)
	b.pruneByAge(now)
	b.pruneByCount()
}

func (b *failureBuffer) pruneByAge(now time.Time) {
	kept := b.Failures[:0:0]
	for _, f := range b.Failures {
		if now.Sub(f.At) > maxFailureAge {
			b.Dropped++
			continue
		}
		kept = append(kept, f)
	}
	b.Failures = kept
}

func (b *failureBuffer) pruneByCount() {
	if len(b.Failures) <= maxBufferedFailures {
		return
	}
	excess := len(b.Failures) - maxBufferedFailures
	b.Dropped += excess
	b.Failures = b.Failures[excess:]
}

// isWireFailureClass reports whether class is one of the three values
// api/openapi.yaml's MacroPriorFailureRequest.class enum actually accepts
// (refused | rejected | unreachable). classLocalError deliberately is not
// among them: it names an attempt that never reached the coordinator at
// all, which is not a fact the coordinator's own priorFailures handling
// has any use for, and — per the coordinator-side defect this guard
// mirrors, where a caller-supplied class string became an unbounded
// coalescing key — sending an arbitrary string here would let a future
// local-only class this program adds later become exactly that same
// coalescing key again, on the wire, for a coordinator this program does
// not own.
func isWireFailureClass(class string) bool {
	switch class {
	case classRefused, classRejected, classUnreachable:
		return true
	}
	return false
}

// asPriorFailures converts the buffer's current contents to the wire
// shape carried in a run submission's priorFailures field. Every entry in
// b.Failures is expected to already carry a wire-valid class — the only
// production call site that ever appends one, reportDegraded in run.go,
// is only ever invoked with result.Class set to classRefused, classRejected,
// or classUnreachable (reportLocalError, the classLocalError path, never
// touches this buffer at all) — but this function is the actual wire
// boundary, so it re-checks rather than trusting that invariant to hold
// forever. Any entry that somehow carries a non-wire class is excluded
// and folded into the dropped count instead of forwarded, exactly as if
// it had been pruned for age or count: see
// TestAsPriorFailuresExcludesNonWireClasses, which fails if this check is
// ever removed.
func (b failureBuffer) asPriorFailures() ([]macroPriorFailureRequest, int) {
	dropped := b.Dropped
	var out []macroPriorFailureRequest
	for _, f := range b.Failures {
		if !isWireFailureClass(f.Class) {
			dropped++
			continue
		}
		// Field-by-field on purpose, not a type conversion: bufferedFailure
		// is this host's on-disk record and macroPriorFailureRequest is the
		// wire shape, and they happen to match today. A conversion would
		// couple them, so changing one would silently change the other.
		//nolint:staticcheck // S1016: the two types are deliberately independent
		out = append(out, macroPriorFailureRequest{
			MacroObjectID: f.MacroObjectID,
			Class:         f.Class,
			HTTPStatus:    f.HTTPStatus,
			At:            f.At,
		})
	}
	if len(out) == 0 && dropped == 0 {
		return nil, 0
	}
	return out, dropped
}
