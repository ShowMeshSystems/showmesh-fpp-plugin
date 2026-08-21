// Package version holds build-time version metadata for ShowMesh binaries.
// Values are overridden at build time via -ldflags -X.
package version

import (
	"fmt"
	"runtime"
)

// Version, Commit, and BuildDate are populated at build time via
// -ldflags "-X github.com/showmeshsystems/showmesh-fpp-plugin/internal/version.Version=...".
// The defaults below apply to unversioned local builds (e.g. `go run`).
var (
	Version   = "dev"
	Commit    = "none"
	BuildDate = "unknown"
)

// String returns a single-line, human-readable summary of the build,
// including the Go runtime version used to build it.
func String() string {
	return fmt.Sprintf("%s (commit %s, built %s, %s)", Version, Commit, BuildDate, runtime.Version())
}
