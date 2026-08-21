package main

import (
	"os/exec"
	"strings"
	"testing"
)

// monorepoModule is the ShowMesh coordinator module. This program decodes
// the wire contract on its own terms, so importing anything from that
// module would let a JSON tag rename on the server rename the field on
// both sides of this program's own decode at once, which is the precise
// failure mode types.go exists to prevent. The whole module is forbidden
// rather than an enumerated package list: a package added after this test
// was written must fail by default rather than needing this test edited
// first.
const monorepoModule = "github.com/showmeshsystems/showmesh"

// thisModuleVersion is this repository's own build-stamping package,
// asserted present so the check below cannot silently stop meaning
// anything if the import were ever removed.
const thisModuleVersion = "github.com/showmeshsystems/showmesh-fpp-plugin/internal/version"

func TestNoForbiddenImports(t *testing.T) {
	out, err := exec.Command("go", "list", "-deps", ".").CombinedOutput()
	if err != nil {
		t.Fatalf("go list -deps . failed: %v\noutput:\n%s", err, out)
	}

	deps := strings.Split(strings.TrimSpace(string(out)), "\n")
	depSet := make(map[string]bool, len(deps))
	for _, d := range deps {
		depSet[d] = true
	}

	for _, d := range deps {
		if d == monorepoModule || strings.HasPrefix(d, monorepoModule+"/") {
			t.Errorf("showmesh-fpp-plugin transitively imports %q from the coordinator module (this program must decode the wire contract independently, not share types with the server)", d)
		}
	}

	if !depSet[thisModuleVersion] {
		t.Error("expected showmesh-fpp-plugin to import this repository's internal/version (build stamping); it appears to be missing")
	}
}

// TestNoThirdPartyDependencies keeps the extracted helper dependency-free.
// It is cross-compiled statically and installed on hosts that carry no Go
// toolchain, so every added module is a new supply-chain surface on a show
// host; adding one is a deliberate change to this test, not a side effect.
func TestNoThirdPartyDependencies(t *testing.T) {
	out, err := exec.Command("go", "list", "-deps", "-f", "{{if .Module}}{{.Module.Path}}{{end}}", ".").CombinedOutput()
	if err != nil {
		t.Fatalf("go list -deps failed: %v\noutput:\n%s", err, out)
	}
	for _, m := range strings.Split(strings.TrimSpace(string(out)), "\n") {
		if m == "" || m == "github.com/showmeshsystems/showmesh-fpp-plugin" {
			continue
		}
		t.Errorf("showmesh-fpp-plugin depends on external module %q; this binary is intended to build from the standard library alone", m)
	}
}
