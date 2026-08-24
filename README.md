# showmesh-fpp-plugin

The ShowMesh runtime that lives on an FPP host.

**Status: the Go macro helper, the host-neutral C++ core, both FPP adapters,
and the outbound coordinator client exist and are built and tested in CI. The
client implements the frozen wire contract the coordinator repository owns, and
has run against fakes only.** Nothing here has been installed on a real FPP
host, no observation has reached a real coordinator, and no release, public or
private, has been published.

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

## The host-neutral C++ core

`native/` holds the part of the resident component that has no FPP in it: the
brightness engine, the playlist-identity derivation, and the bounded callback
handoff. It includes no FPP header and links no third-party library, so it
builds and its tests run on any machine with a C++17 compiler, long before an
FPP host is involved. A make target asserts the no-FPP-header rule rather than
trusting it.

```sh
make native         # build the static core library
make native-test    # the no-FPP-header check, then the test binary
make -C native test CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined"
make -C native test CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=thread"
```

The test harness is a hundred lines in `native/tests/check.h`, for the same
reason there are no library dependencies: this source is compiled on an FPP
host with whatever toolchain that host has, and nothing may need fetching.

**Brightness.** Two independently owned values, each with its own fade:

```text
effective output = round(ceiling * transition_gain / 100)
```

The ceiling is what FPP's scheduler and operator commands write; the transition
gain is the coordinator's alone and is deliberately not reachable from any FPP
action. Both default to 100, both fade linearly per frame, and they compose on
every frame, so a ceiling change during a gain fade takes effect at once and a
later gain of 100 reveals the current ceiling rather than a cached earlier one.
A fade started while another is running begins at the current interpolated
value, so replacement introduces no jump. Channels outside the configured apply
ranges, and channels inside an exclusion, are never written.

Full state, including any active fade, is what nodes exchange and what is
persisted: never a relative adjustment, so a duplicated or delayed payload is
harmless and a stale or unreadable one is rejected rather than guessed at.
After a restart the engine resumes a fade whose recorded timing it can place,
and otherwise settles on the darker of the last applied value and the target.
It never comes back brighter than what it was already applying.

**Playlist identity.** The canonical playlist hash is SHA-256 over the RFC 8785
canonicalization of the complete definition FPP returned, with no field
removed. Both the canonicalizer and the hash are implemented here; the number
formatting was differentially tested against a JavaScript engine, since RFC 8785
adopts ECMAScript's number-to-string rules wholesale.

The entry key hashes a canonical JSON object of the instance UUID, playlist
name, playlist hash, section, and position, rather than a delimited string, so
a name containing a separator character cannot collide with a different entry.
It is stable across restarts for an unchanged definition and changes when the
definition does. Duplicate filenames at different positions stay distinct.
Missing evidence produces an explicit unavailable reason and never falls back
to filename identity.

**The callback boundary.** `CallbackEvidence` is a fixed-size, allocation-free
struct, and `CallbackHandoff` is a bounded queue with a mutex and nothing else.
Copy the evidence and return: no network request, retry sleep, definition
fetch, hash, or filesystem write belongs on FPP's callback thread, which is a
running show's thread. When the queue is full the oldest pending observation is
dropped so the newest state survives, and the drop is counted. That count is
the gap evidence: current-state convergence is the invariant, and a coalesced
delivery must never read as a complete event history.

**Sequence persistence.** The per-instance monotonic sequence the worker
stamps on every drained observation (`showmesh/playlist_identity.h`'s
`SequenceState`) is persisted by `SequenceFileStore`
(`showmesh/sequence_store.h`) so a restarted plugin resumes above the highest
value it ever issued, never at 0. The coordinator refuses a repeated sequence,
so resuming at 0 after every `fppd` restart, which FPP 10's runtime plugin
load/unload made an ordinary operator action rather than a rare event, would
wedge every later observation behind a 409 the plugin cannot recover from on
its own.

`SequenceFileStore`'s constructor also attempts to create its directory if it
does not already exist (best effort, mode 0755), since nothing else in this
repository provisions it. When that attempt fails, or the directory exists but
is not writable, `store()` keeps returning `false` and nothing is silently
lost: `drainOnce()` still publishes the observation, and counts the failure in
`ShowMeshRuntime::sequencePersistFailureCount()` so the condition is visible
rather than only showing up as a later restart resuming below what was
actually issued.

`load()`'s 0 for a missing, unreadable, or corrupt store on both sides covers
two different situations: a genuine first run, where 0 is simply correct, and
a restart after both files are present but neither validates, where a value
was certainly issued before and its true height is unknown.
`SequenceFileStore::loadDetailed()` tells them apart via its
`filesPresentButInvalid` flag, which `ShowMeshRuntime` latches once at
construction as `sequenceFilesWereAllInvalidAtStartup()`. The runtime still
resumes at the same value `load()` would give (0), since there is no other
safe number to pick without a real height to resume from; making the
situation visible is what lets an operator use the coordinator's own
sequence-reset route instead of the wedge going unnoticed.

Each adapter constructs its store over `resolveSequenceStateDir()`, which
follows the same precedence and the same two environment variables as the Go
helper's own state directory
(`cmd/showmesh-fpp-plugin/config.go`'s `resolveConfigDir`):
`SHOWMESH_FPP_PLUGIN_CONFIG_DIR`, then `MEDIADIR` with `plugindata/fpp-showmesh`
appended, then the pinned literal `/home/fpp/media/plugindata/fpp-showmesh`.
This is deliberately not the credential's fixed `/etc/showmesh-fpp-plugin`
directory: that path is reserved for the one secret file, and it is
deliberately not inside this repository's own checkout either, since FPP 10's
`upgrade_plugin` still runs `git clean -fd` there as an upgrade fallback and
would delete anything untracked it found.

Every drained observation writes: `SequenceFileStore::store()` is called once
per accepted item from the callback handoff (identity-resolved or not, sink
accepted or not), which is the FPP playlist-event rate under the handoff's own
bounded coalescing, not a fixed tick; there is no per-output-frame write. Each
write is two files (a primary and a rotated backup, the latter a metadata-only
rename rather than a data copy) via a temp-file-then-`rename()` sequence with
an `fsync`, so an unclean shutdown mid-write leaves either the file's previous
complete contents or its new complete contents, never a torn one, and a
checksum line catches corruption an atomic rename does not (a bit flip, a hand
edit, a non-`rename()`-atomic filesystem). `load()` trusts only a file whose
checksum still validates and returns the higher of the two; a missing,
unreadable, or corrupt store on both sides returns 0, the same value a brand
new `SequenceState` already starts at, so there is nothing further to rewind.
`store()` itself also refuses to persist a value lower than what `load()`
currently returns, enforcing the same never-goes-backward rule on disk that
`SequenceState::restore()` already enforces in memory.

**The sending half.** `CoordinatorClient` (`showmesh/coordinator_client.h`) is
the `ObservationSink` and `DefinitionPublisher` both adapters install. It posts
a resolved observation to `/api/v1/integrations/fpp/playlist-entry-observations`
and the complete definition behind its hash to
`/api/v1/integrations/fpp/playlist-definitions`, both authenticated as a bearer
token and both guarded coordinator-side by `fpp:observe`. Everything it does
runs on the resident worker thread.

The base URL comes from `<state-dir>/config.json`, the same file and key the Go
helper reads. The credential comes from the fixed `/etc/showmesh-fpp-plugin`
directory and is refused unless the file's mode is exactly 0600. It is never
logged, never written anywhere, and never reaches the local status record.

Delivery is bounded in both directions: at most five attempts with a doubling
backoff capped at 30 seconds, and only for a failure that could still succeed
(no response at all, a 429, or a 5xx). A 400, 401, 403, 409, or 413 is terminal,
because repeating bytes the coordinator understood and refused only spends the
show LAN and its audit log. A refused observation acknowledges nothing, so its
gap evidence rides forward on the next one. Counts for each of those cases, the
last outcome, and the last error land in `<state-dir>/observation-status.json`,
so an operator can tell a wrong credential from a missing scope from an
unreachable coordinator without the coordinator's help.

Definitions are content addressed, so the plugin tracks the hashes it has
already posted in memory only and a restart simply re-posts. It sweeps every
playlist definition on the host when the worker starts, which is what lets an
operator author against a playlist in the afternoon while FPP is idle and has
played nothing, and re-scans no more often than every 60 seconds after that.
Resolving an entry identity also publishes the definition behind that hash,
before the observation citing it.

The concrete HTTP client is not in the host-neutral core. `HttpTransport`
(`showmesh/http_transport.h`) is an interface the tests fake; the libcurl
implementation lives in `native/adapters/shared/curl_http_transport.h`, beside
the adapters, because an https coordinator means a TLS-capable library and the
core links none.

`ShowMeshRuntime::flushSequenceState()` persists the current value on demand,
independent of the per-observation write above. It is not called from either
adapter today; it exists as the seam a future explicit shutdown callback can
use for one extra, cheap guarantee before the process exits.

## The FPP adapters

`native/adapters/` is the only code in this repository that includes an FPP
header. Each adapter owns nothing but its major's plugin lifecycle and forwards
everything else into the host-neutral runtime:

| | FPP 9 | FPP 10 |
|---|---|---|
| Plugin ABI | unversioned | versioned and checked at `dlopen` |
| Teardown | destructor | `shutdown()`, before the object is destroyed |
| HTTP registration | libhttpserver surface | that surface is gone |
| Header prerequisites | jsoncpp and libhttpserver | jsoncpp |

They are separate translation units producing separate shared objects. One
binary cannot serve both majors, and an `#ifdef`'d lifecycle would hide that.

```sh
make -C native/adapters fpp9  FPP_SRC=/opt/fpp/src
make -C native/adapters fpp10 FPP_SRC=/opt/fpp/src
make -C native/adapters verify-fpp10 FPP_SRC=/opt/fpp/src
```

`FPP_SRC` points at the installed FPP tree, which is how a host compiles it:
against that host's own headers, never against a version assumed at release
time. The verify targets check that the shared object exports what the loader
looks up by name, including FPP 10's ABI-version and logger-layout
fingerprints, which that loader refuses a plugin without.

CI compiles both against pinned tags and commits recorded in
[`native/adapters/FPP-PINS.md`](native/adapters/FPP-PINS.md), and fails if a
tag has been repointed away from its pinned commit. Compiling proves the
adapters agree with those headers and export the right symbols. It is not
evidence that the plugin loads into a running `fppd`.

The callback does one thing: read the bounded fields out of FPP's playlist
JSON and hand them to the runtime. The definition fetch, the hash, the
sequence, and the publish all happen on the worker thread.

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

The coordinator ingestion contract for playlist-entry observations and playlist
definitions is owned by the coordinator repository and is mirrored under
[`docs/upstream/showmesh/docs/build/FPP-PLUGIN-COORDINATOR-CONTRACTS.md`](docs/upstream/showmesh/docs/build/FPP-PLUGIN-COORDINATOR-CONTRACTS.md).
The sending half described above implements that mirrored contract byte for
byte; a disagreement is a blocker raised against both repositories, never a
parallel wire shape invented on one side. It has run against fakes and unit
tests only. Neither route has been exercised against a real coordinator, and no
observation has left a real FPP host.

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
