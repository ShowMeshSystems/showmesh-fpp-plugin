package main

import (
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// Operator-facing strings carry no repo path, .md reference, ADR number,
// RES-nnn, or "section <digit>". A message this binary writes to
// status.json, to stderr, or into the local policy statement a refusal
// prints is read by an operator standing at the FPP host with a dead
// network connection, who cannot look up an internal citation from there.
//
// This test parses the SOURCE of every non-test .go file in this
// package's own directory and inspects every string literal that appears
// in CODE. A `//` comment is not part of the tree go/parser builds unless
// asked to retain it, and this walk never asks, so a citation left in a
// comment (this file's own convention) never trips this test.
var forbiddenCopyPattern = regexp.MustCompile(
	`docs/|\.md\b|ADR-\d+|RES-\d{3}|(?i)\bsection\s+\d|api/openapi\.yaml`,
)

// copyGuardExemption is one (file, exact string literal VALUE) pair this
// guard does not fail on, matched against the raw source text of the
// literal including its surrounding quotes.
type copyGuardExemption struct {
	file  string
	value string
}

// copyGuardExemptions starts empty on purpose: every string literal in
// this package as first written is either genuinely operator-facing (and
// so must never carry an internal citation) or a doc comment (which this
// guard does not see at all). Add an entry here only after confirming by
// hand that the exempted string is never rendered to an operator.
var copyGuardExemptions = []copyGuardExemption{}

func copyGuardExemptionSet() map[copyGuardExemption]bool {
	set := make(map[copyGuardExemption]bool, len(copyGuardExemptions))
	for _, e := range copyGuardExemptions {
		set[e] = true
	}
	return set
}

func copyGuardTargetFiles(t *testing.T) []string {
	t.Helper()
	entries, err := os.ReadDir(".")
	if err != nil {
		t.Fatalf("reading this package's own directory: %v", err)
	}
	var files []string
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".go") || strings.HasSuffix(name, "_test.go") {
			continue
		}
		files = append(files, name)
	}
	sort.Strings(files)
	return files
}

// TestOperatorFacingStringsCarryNoInternalCitation is this package's
// regression guard, verified per this project's standing rule ("break the
// behavior, confirm the test fails, restore"): temporarily reintroducing a
// citation like "STEP-9-SPEC.md section 8.1" into run.go's
// reportDegraded and rerunning this test turns it from passing to
// failing, naming the exact file, line, and offending substring — checked
// by hand while writing this file, then reverted.
func TestOperatorFacingStringsCarryNoInternalCitation(t *testing.T) {
	exemptions := copyGuardExemptionSet()
	for _, path := range copyGuardTargetFiles(t) {
		path := path
		t.Run(path, func(t *testing.T) {
			fset := token.NewFileSet()
			file, err := parser.ParseFile(fset, path, nil, 0)
			if err != nil {
				t.Fatalf("parsing %s: %v", path, err)
			}
			ast.Inspect(file, func(n ast.Node) bool {
				lit, ok := n.(*ast.BasicLit)
				if !ok || lit.Kind != token.STRING {
					return true
				}
				if exemptions[copyGuardExemption{file: filepath.Base(path), value: lit.Value}] {
					return true
				}
				if loc := forbiddenCopyPattern.FindString(lit.Value); loc != "" {
					pos := fset.Position(lit.Pos())
					t.Errorf(
						"%s:%d: string literal carries an internal citation (%q matched by forbiddenCopyPattern): %s\n"+
							"move the citation into a // comment; the string itself must read as if no internal doc, "+
							"ADR, or research record existed.",
						path, pos.Line, loc, lit.Value)
				}
				return true
			})
		})
	}
}
