//go:build ignore

// One-off generator: produces signed ADR-048 fallback program fixtures
// for the FPP plugin's verifier tests, using the coordinator repository's
// own Program/CanonicalBytes/SignedProgram code and crypto/ed25519
// directly (not internal/coordinator/signingkey: these are throwaway
// test keypairs, never a real coordinator signing key).
//
// This file imports github.com/showmeshsystems/showmesh, which is the
// coordinator repository's module, not this one's: the ignore build tag
// keeps `go build ./...`/`go vet ./...` in this repository from trying to
// compile it. Run it from inside a checkout of ShowMeshSystems/showmesh:
//
//	go run generate_fixtures.go <output-dir>
package main

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/showmeshsystems/showmesh/pkg/coordsig"
	"github.com/showmeshsystems/showmesh/pkg/fallbackprogram"
)

func buildProgram(packageID, revision, show string) fallbackprogram.Program {
	expires := time.Date(2026, 9, 8, 12, 0, 0, 0, time.UTC)
	compiled := time.Date(2026, 9, 8, 11, 0, 0, 0, time.UTC)
	return fallbackprogram.Program{
		SchemaVersion:   fallbackprogram.SchemaVersion,
		PackageID:       packageID,
		Revision:        revision,
		ExpiresAt:       expires,
		CompiledAt:      compiled,
		FPPInstanceUUID: "22222222-2222-4222-8222-222222222222",
		Show:            show,
		Generation:      1,
		PlaylistRevisions: map[string]int64{
			"pl-main": 7,
		},
		CatalogRevisions: map[string]string{
			"node-a": "cat-rev-1",
		},
		Entries: []fallbackprogram.EntryMapping{
			{
				EntryKey:    "entry-0",
				CueID:       "cue-a",
				CueRevision: 3,
				Targets: []fallbackprogram.NodeTarget{
					{
						NodeID: "node-a",
						Render: &fallbackprogram.RenderActivation{
							Sequence:    "seq-a",
							Filename:    "seq-a.fseq",
							AssetHashes: []string{"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
						},
					},
				},
			},
		},
		Rules: fallbackprogram.FixedRules,
	}
}

// buildResolverEdgeCasesProgram is a validly-signed program exercising
// shapes the wire format permits but the coordinator's own compiler
// never emits (fallbackprogram.go's NodeTarget doc comment: "a compiler
// that resolved neither for a node does not include that node as a
// target at all"; nothing stops two EntryMapping values sharing an
// EntryKey either). It exists only for the FPP plugin's local resolver
// tests (Lane A), which must treat these as an untrusted document could
// realistically arrive shaped this way regardless of what today's
// compiler happens to produce.
func buildResolverEdgeCasesProgram() fallbackprogram.Program {
	p := buildProgram("55555555-5555-4555-8555-555555555555", "test-revision-0004", "Resolver Edge Cases")
	p.Entries = []fallbackprogram.EntryMapping{
		{
			EntryKey:    "entry-dup",
			CueID:       "cue-dup-a",
			CueRevision: 1,
			Targets: []fallbackprogram.NodeTarget{
				{NodeID: "node-a", Render: &fallbackprogram.RenderActivation{Sequence: "seq-a", Filename: "seq-a.fseq"}},
			},
		},
		{
			EntryKey:    "entry-dup",
			CueID:       "cue-dup-b",
			CueRevision: 1,
			Targets: []fallbackprogram.NodeTarget{
				{NodeID: "node-a", Render: &fallbackprogram.RenderActivation{Sequence: "seq-b", Filename: "seq-b.fseq"}},
			},
		},
		{
			EntryKey:    "entry-empty",
			CueID:       "cue-empty",
			CueRevision: 1,
			Targets:     []fallbackprogram.NodeTarget{},
		},
		{
			EntryKey:    "entry-no-activation",
			CueID:       "cue-no-activation",
			CueRevision: 1,
			Targets: []fallbackprogram.NodeTarget{
				{NodeID: "node-b"},
			},
		},
	}
	return p
}

func sign(program fallbackprogram.Program, priv ed25519.PrivateKey) fallbackprogram.SignedProgram {
	payload, err := program.CanonicalBytes()
	if err != nil {
		panic(err)
	}
	sig := ed25519.Sign(priv, payload)
	return fallbackprogram.SignedProgram{Program: program, Signature: coordsig.Signature(sig)}
}

func mustMarshal(v any) []byte {
	b, err := json.Marshal(v)
	if err != nil {
		panic(err)
	}
	return b
}

func writeFile(dir, name string, contents []byte) {
	if err := os.WriteFile(filepath.Join(dir, name), contents, 0o644); err != nil {
		panic(err)
	}
}

// mirrorGetResponse copies v1.FallbackProgramResponse's JSON shape
// (internal/coordinator/api/v1/fallbackprograms.go) field for field and
// tag for tag. It is a copy, not the real type, only because that
// package is internal to the coordinator module; the wire shape is
// public contract (api/openapi.yaml), not an implementation detail this
// script is free to invent.
type mirrorGetResponse struct {
	ServerTime      string          `json:"serverTime"`
	FPPInstanceUUID string          `json:"fppInstanceUuid"`
	Published       bool            `json:"published"`
	Program         json.RawMessage `json:"program,omitempty"`
	SignatureBase64 string          `json:"signatureBase64,omitempty"`

	AcknowledgedStatus string `json:"acknowledgedStatus"`
}

// wrapAsGetResponse extracts "program" and "signature" out of an already
// fully-marshaled SignedProgram document (signedDocumentBytes) via
// json.RawMessage, the identical verbatim-slice technique
// extractStoredProgramBytes uses coordinator-side, and re-homes them as
// mirrorGetResponse's sibling fields. The program bytes are never
// decoded into a Go struct and re-marshaled: only the envelope around
// them, built fresh here, goes through json.Marshal.
func wrapAsGetResponse(signedDocumentBytes []byte, published bool) []byte {
	var decoded struct {
		Program   json.RawMessage `json:"program"`
		Signature string          `json:"signature"`
	}
	if err := json.Unmarshal(signedDocumentBytes, &decoded); err != nil {
		panic(err)
	}
	response := mirrorGetResponse{
		ServerTime:         "2026-09-08T09:00:00Z",
		FPPInstanceUUID:    "22222222-2222-4222-8222-222222222222",
		Published:          published,
		Program:            decoded.Program,
		SignatureBase64:    decoded.Signature,
		AcknowledgedStatus: "fallback-program-unacknowledged",
	}
	return mustMarshal(response)
}

func notPublishedGetResponse() []byte {
	response := mirrorGetResponse{
		ServerTime:         "2026-09-08T09:00:00Z",
		FPPInstanceUUID:    "22222222-2222-4222-8222-222222222222",
		Published:          false,
		AcknowledgedStatus: "fallback-program-unacknowledged",
	}
	return mustMarshal(response)
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: main <output-dir>")
		os.Exit(1)
	}
	outDir := os.Args[1]
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		panic(err)
	}

	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		panic(err)
	}
	wrongPub, wrongPriv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		panic(err)
	}

	program := buildProgram("11111111-1111-4111-8111-111111111111", "test-revision-0001", "Test Show")

	valid := sign(program, priv)
	validBytes := mustMarshal(valid)
	writeFile(outDir, "valid.json", validBytes)

	// One byte changed inside a string field value, keeping the document
	// syntactically valid JSON, so the refusal below is a signature
	// mismatch against what the coordinator signed, not a parse failure.
	marker := []byte("Test Show")
	idx := bytes.Index(validBytes, marker)
	if idx < 0 {
		panic("marker \"Test Show\" not found in marshaled document")
	}
	tampered := append([]byte(nil), validBytes...)
	tampered[idx] = 'X'
	if bytes.Equal(tampered, validBytes) {
		panic("tamper produced no change")
	}
	writeFile(outDir, "tampered-one-byte.json", tampered)

	wrongKeySigned := sign(program, wrongPriv)
	writeFile(outDir, "wrong-key.json", mustMarshal(wrongKeySigned))

	resolverEdgeCases := sign(buildResolverEdgeCasesProgram(), priv)
	writeFile(outDir, "resolver-edge-cases.json", mustMarshal(resolverEdgeCases))

	// A second, distinct valid program (different content, same real
	// key), for the restart-survival and overwrite-on-reinstall tests.
	program2 := buildProgram("33333333-3333-4333-8333-333333333333", "test-revision-0002", "Second Test Show")
	valid2 := sign(program2, priv)
	writeFile(outDir, "valid-second.json", mustMarshal(valid2))

	// GET /api/v1/fallback-programs/{fppInstanceId}'s actual response
	// shape (v1.FallbackProgramResponse), wrapping each signed document
	// above: "program" as raw bytes (never re-marshaled: see
	// wrapAsGetResponse's own comment) and "signature" traveling as the
	// sibling field "signatureBase64", never nested. v1 is
	// internal to the coordinator module and this throwaway script does
	// not import it; the shape below is copied from
	// internal/coordinator/api/v1/fallbackprograms.go's own struct
	// tags, not reimplemented independently, and is also documented in
	// this repository's api/openapi.yaml.
	writeFile(outDir, "valid-get-response.json", wrapAsGetResponse(validBytes, true))
	writeFile(outDir, "tampered-get-response.json", wrapAsGetResponse(tampered, true))
	writeFile(outDir, "wrong-key-get-response.json", wrapAsGetResponse(mustMarshal(wrongKeySigned), true))
	writeFile(outDir, "not-published-get-response.json", notPublishedGetResponse())

	keys := map[string]string{
		"coordinatorPublicKeyBase64": base64.StdEncoding.EncodeToString(pub),
		"wrongPublicKeyBase64":       base64.StdEncoding.EncodeToString(wrongPub),
		"note":                       "throwaway ed25519 keypairs generated only for these fixtures, never a real coordinator signing key",
	}
	keysBytes, err := json.MarshalIndent(keys, "", "  ")
	if err != nil {
		panic(err)
	}
	writeFile(outDir, "keys.json", keysBytes)

	fmt.Println("wrote fixtures to", outDir)
}
