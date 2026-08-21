# showmesh-fpp-plugin

The ShowMesh runtime that lives on an FPP host.

**Status: the Go macro helper is extracted and builds here independently. The
C++ component has not been written.** Nothing here has been installed on a real
FPP host, and no release, public or private, has been published.

## What this plugin does and why it exists

FPP (Falcon Player) is the authoritative calendar scheduler for a ShowMesh
display. ShowMesh coordinates the rest of the show around it: renderers,
audience audio, timecode, projection, and Resolume. Two of those jobs cannot be
done from outside the FPP host, which is why this repository exists.

**Firing a show macro from FPP.** FPP's own scheduler is what decides that the
show starts. A ShowMesh macro is what makes everything else start with it. The
Go helper is a small binary FPP invokes as a script command, which fires a macro
run against the coordinator and writes an honest local record of what happened,
including when the coordinator was unreachable. That record has to be readable
on the FPP host itself, because the case it exists for is the case where the
coordinator cannot be reached to read it.

**Brightness and playlist identity.** Stock FPP has no brightness command at
all, and FPP's HTTP observations cannot tell you unambiguously which playlist
entry is playing. Both gaps need code resident inside `fppd`. The C++ component
supplies a two-value brightness engine with a fadeable ceiling exposed as an FPP
Action, and a `playlistCallback` observer that publishes an atomic, versioned
playlist-entry identity event the coordinator can trust.

## Go helper versus resident C++ component

They are separate programs with different lifetimes, and neither replaces the
other.

| | Go macro helper | Resident C++ component |
|---|---|---|
| Lifetime | Forked per invocation, exits | Resident inside `fppd` |
| Invoked by | FPP script command | FPP plugin lifecycle and callbacks |
| Job | Fire a macro run, keep a local status record | Brightness engine, playlist-entry identity publisher |
| Distribution | Prebuilt static archives per architecture | Architecture-independent source, compiled on the host |
| FPP coupling | None beyond the command registration | Tight: separate FPP 9 and FPP 10 adapters |

The C++ component is compiled locally against the host's installed FPP headers
because FPP 10 replaces libhttpserver with Drogon and revamps the Plugin
Manager, so one binary cannot be assumed compatible across majors. That local
build is the highest-risk assumption in the whole install model and has to be
measured on the slowest supported ARMv7 host before any readiness claim.

## The three repositories

One-way release dependencies, no cycles.

| Repository | Owns |
|---|---|
| [`showmesh`](https://github.com/ShowMeshSystems/showmesh) | Coordinator, the public API and OpenAPI contract, UI and CLI, collectors, night-session integration, the brightness contract, and ingestion of playlist-entry observations. |
| `showmesh-fpp-plugin` (this repository) | Apache-2.0 FPP runtime source: the Go macro helper, the C++ brightness engine, the native playlist observer, the version adapters, tests, and release artifacts. |
| `fpp-showmesh` | Thin FPP Plugin Manager packaging: `pluginInfo.json`, command descriptions, lifecycle scripts, version pins, and committed artifact hashes. |

This repository never reaches into coordinator internals. It consumes the public
API and the frozen wire contracts, and a coordinator-side rename must break this
build rather than drift silently past it.

## Supported FPP versions

**FPP 9.4 through 9.x, and FPP 10.x. FPP 8 is not supported.**

The FPP 10 build and verification target is the latest published beta available
when the work runs or is revalidated, rather than a beta number frozen into
prose. Every CI and acceptance result records the exact FPP tag and source
commit it built against. A version label is not evidence of ABI compatibility:
each later beta, release candidate, and final release is added to the
compatibility matrix and rebuilt against its own installed headers.

## Build, test, and lint

The Go helper builds from the standard library alone: no module dependencies, no
cgo, no monorepo checkout. A test asserts that, so adding a dependency is a
deliberate change rather than a side effect.

```sh
make build          # host binary into ./bin
make test           # go test ./...
make test-race      # the same suite under -race
make vet            # go vet ./...
make fmt-check      # fails on anything gofmt would rewrite
make lint           # golangci-lint, downloaded on demand if not installed
make check          # fmt-check, vet, lint, test
```

Cross-build the three release architectures, write the pinned checksum manifest,
and verify it against the tarballs just produced:

```sh
make release VERSION=0.0.0-local
```

That writes `dist/showmesh-fpp-plugin_<VERSION>_linux_{amd64,arm64,armv7}.tar.gz`
and `dist/showmesh-fpp-plugin_<VERSION>_SHA256SUMS`. Each binary is
`CGO_ENABLED=0`, `-trimpath`, statically linked, and named `showmesh-fpp-plugin`
at mode 0755 inside its archive.

```sh
make verify-reproducible VERSION=0.0.0-local
```

builds and packages one architecture twice, independently, and fails unless the
two tarballs are byte-identical. This needs GNU tar (`gtar` on macOS via
`brew install gnu-tar`); without it the target says it could not confirm
reproducibility rather than reporting a comparison it never made.

CI runs the same targets: formatting, vet, and the race suite on both the
minimum Go version `go.mod` claims and the current toolchain; lint; then the
release pipeline, its reproducibility check, and a per-architecture assertion
that every artifact really is a static Linux binary for the architecture its
filename names. CI publishes nothing and uploads no workflow artifact.

## Coordinator seam

Command logic talks to the coordinator only through `CoordinatorClient`, a
two-call interface: submit a macro run, and read a macro definition for the
local cache. The handwritten HTTP client sits behind it unchanged. Local
persistence, cached macro data, status rendering, refusal-versus-outage
classification, and failure-buffer policy stay plugin-owned above the seam.

Buffered prior failures are not a separate call. They ride in the run request,
so the local buffer clears only after a 2xx response and a failed report is
never silently discarded. Both directions of that rule are tested.

## Artifact and packaging flow

After the first real-host install gate passes and the owner approves
publication, this repository publishes one versioned release containing:

- statically linked Go helper archives for `linux/amd64`, `linux/arm64`, and
  `linux/arm/v7`;
- one architecture-independent C++ source bundle with the shared engine and both
  FPP adapters;
- a release manifest suitable for producing the packaging repository's lock
  file.

`fpp-showmesh` then commits an `artifacts.lock.json` pinning the exact release
version, filenames, and SHA-256 digest of every downloadable artifact.
Installation downloads only those exact files, verifies them against the
committed hashes, and fails closed on a mismatch. **A checksum manifest fetched
from the same mutable release location is not the trust anchor.**

Installation stages new files, validates the binary and the compiled component,
and activates atomically. A failed download, verification, compile, or
activation leaves the previous working version in place. Uninstall reverses
every file and service created outside the plugin directory.

## Security and credential posture

Report vulnerabilities per the snapshot at
[`docs/upstream/showmesh/SECURITY.md`](docs/upstream/showmesh/SECURITY.md).

Three constraints are structural rather than advisory, and each was paid for
once already:

- **The coordinator credential is never a command argument.** FPP publishes
  every command execution, with its arguments, in cleartext to the MQTT topic
  `command/run`. A token passed as an argument is broadcast on every invocation.
  It is read from a mode-0600 file the plugin owns, at a fixed path outside
  FPP's web root, outside the media tree, and outside the plugin's own checkout.
- **The credential does not live in FPP's configuration directory.** FPP serves
  and accepts writes under that directory over unauthenticated HTTP endpoints,
  and its backup redaction is an exact key-name match list that would not redact
  a file named `credential`.
- **Never bind a second listener on UDP 32320.** `SO_REUSEPORT` load-balances
  unicast datagrams by 4-tuple hash, so a co-located listener can silently steal
  FPP's own sync stream and desync a live show.

No secret, token, credential, or host-identifying value from a real
installation enters a commit, a test fixture, or an example in this repository.

## Verification limits

Everything in this repository is developed against bench instances. A unit test,
a container, and a fake are not a real-host result, and no comment, log line, or
string here may claim otherwise.

Specifically unproven, and each requires a real FPP host:

- on-host install, filesystem permissions, and packaging;
- local C++ compilation on the slowest supported ARMv7 host, including compiler
  availability, headers, memory, build time, and rollback;
- cross-version behavior on FPP 9 and FPP 10 hosts;
- the callback boundary's real timing inside a running `fppd`.

The first real-host install is a separate tracked gate that this repository does
not close on its own, and it also owns public release approval. Candidate
artifacts stay private until it passes.

## License

Apache-2.0. See [`LICENSE`](LICENSE).

`FalconChristmas/fpp-brightness` is GPL-3.0. It may be read as behavioral and
source evidence. Its implementation is not copied into or linked against this
repository unless a separate license compatibility review explicitly permits it.
Attribution and licenses are recorded for every dependency and every copied
source file.
