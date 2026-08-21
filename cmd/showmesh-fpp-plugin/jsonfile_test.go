package main

import (
	"os"
	"path/filepath"
	"testing"
)

type jsonfileFixture struct {
	A string `json:"a"`
	B int    `json:"b"`
}

func TestWriteReadJSONFileRoundTrip(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "f.json")
	if err := writeJSONFile(path, jsonfileFixture{A: "x", B: 3}); err != nil {
		t.Fatal(err)
	}
	var out jsonfileFixture
	ok, err := readJSONFile(path, &out)
	if err != nil || !ok {
		t.Fatalf("ok=%v err=%v", ok, err)
	}
	if out.A != "x" || out.B != 3 {
		t.Errorf("out = %+v, want {x 3}", out)
	}
}

func TestWriteJSONFileLeavesNoTempFileBehind(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "f.json")
	if err := writeJSONFile(path, jsonfileFixture{A: "x"}); err != nil {
		t.Fatal(err)
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 || entries[0].Name() != "f.json" {
		var names []string
		for _, e := range entries {
			names = append(names, e.Name())
		}
		t.Errorf("directory contains %v, want exactly [f.json] (no leftover temp file)", names)
	}
}

func TestWriteJSONFileMode0600(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "f.json")
	if err := writeJSONFile(path, jsonfileFixture{A: "x"}); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if mode := info.Mode().Perm(); mode != 0o600 {
		t.Errorf("mode = %04o, want 0600", mode)
	}
}

func TestReadJSONFileMissingIsNotError(t *testing.T) {
	dir := t.TempDir()
	var out jsonfileFixture
	ok, err := readJSONFile(filepath.Join(dir, "nope.json"), &out)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if ok {
		t.Error("ok = true for a nonexistent file, want false")
	}
}

func TestReadJSONFileCorruptIsError(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "bad.json")
	if err := os.WriteFile(path, []byte("{not json"), 0o600); err != nil {
		t.Fatal(err)
	}
	var out jsonfileFixture
	_, err := readJSONFile(path, &out)
	if err == nil {
		t.Error("expected an error decoding a corrupt JSON file, got nil")
	}
}
