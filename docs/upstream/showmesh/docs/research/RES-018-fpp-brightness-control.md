# RES-018: FPP Brightness, Playlist Identity, and Plugin Runtime

[Architecture](../architecture/RESTING-MODE.md) · [Distribution research](RES-015-fpp-plugin-distribution-model.md) · [Track F](../build/TRACK-F-resting-mode.md) · [Track H](../build/TRACK-H-cues-and-playlists.md) · [SM-63 handoff](../build/SM-63-FPP-PLUGIN-HANDOFF.md) · [Tracker](README.md)

Status: planned · Risk: **high** · Verification: **L1 source evidence** for the FPP plugin mechanisms and **L2 bench evidence** for the absence of a stock FPP brightness command; no ShowMesh brightness component has been built or installed on an FPP host

Decision dates: 2026-08-18 for brightness and repository structure; 2026-08-20 for Apache-2.0 licensing and the ADR-043 playlist-identity publisher. This record does not claim that the selected design, the third repository, release artifacts, or real-host installation exist yet.

Linear tracking: SM-63 owns the runtime/repository build; SM-14 owns the first real-host install and release-publication gate. SM-49 is complete because the owner decision is made.

## 1. Finding

Stock FPP does not expose the compositional brightness seam required by [RESTING-MODE §7.3](../architecture/RESTING-MODE.md#73-brightness-composition). The bench FPP 9.5.3 command vocabulary contains no brightness command. Its brightness-shaped API replaces the complete output-processor configuration, and the running status has no separately observable scheduled ceiling.

`FalconChristmas/fpp-brightness` proves that the correct per-frame extension point exists: `modifyChannelData` can multiply rendered channel data without rewriting the output-processor configuration. The upstream plugin does not itself solve ShowMesh's requirement because it owns one scalar. HTTP, MQTT, FPP commands, and MultiSync all overwrite that same value; there is no independent ceiling and transition gain.

The selected design is therefore a ShowMesh-owned FPP component with two independently owned values:

```text
effective output = round(ceiling * transition_gain / 100)
```

- `ceiling` is 0–100 and is written by the FPP schedule/operator command path. The plugin will register a parameterized FPP Action named `ShowMesh: Set Brightness Ceiling` with `targetPercent` (integer 0–100) and `fadeSeconds` (integer 0–86400), so ordinary FPP scheduler entries and manual presets can set it. `fadeSeconds: 0` applies immediately; a positive value linearly interpolates the ceiling to the target per output frame.
- `transition_gain` is 0–100 and is written by the ShowMesh night-session controller.
- Both default to 100. A ceiling change during a fade takes effect immediately; a later gain of 100 reveals the current ceiling, never a cached earlier ceiling.
- A new ceiling action received during an active ceiling fade starts from the currently interpolated ceiling, not the previous start or target, so it introduces no discontinuity. Ceiling fades and ShowMesh transition-gain fades may run concurrently and compose on every frame.
- The component applies only to configured channel ranges. Overlap and out-of-bounds ranges are rejected.
- MultiSync carries the complete versioned brightness and active-fade state, not relative adjustments. A late-joining node can converge on the current fade position and target; stale or incompatible payloads are rejected rather than guessed at.
- Fade state and target are persisted before activation. After restart the component reconstructs the current fade position from persisted timing; if that timing cannot be trusted, it chooses the darker of the last applied ceiling and target rather than jumping brighter.
- The transition gain is not exposed as an FPP Action; only the coordinator-facing night-session contract may write it. MQTT setters, relative brighten/dim commands, and hard-coded fade-up/fade-down commands are out of scope. They would add writers or hide ownership.

**Reference-installation fact, confirmed by the owner 2026-08-18:** the deployed 10 PM and 11 PM scheduler entries invoke commands from `FalconChristmas/fpp-brightness` and use timed fades rather than jumps. Migration retargets those entries to `ShowMesh: Set Brightness Ceiling` with the same target percentages and fade durations in seconds. The old plugin is removed only after the new action, MultiSync propagation, fade behavior, and effective output are verified on the participating hosts.

## 2. Supported FPP versions

The build targets **FPP 9.4 through 9.x and FPP 10.x**. FPP 8 is not supported.

The build and verification target is the **latest published FPP 10 beta available when the work starts or is revalidated**, rather than a beta number embedded in this record. FPP 10 changes the HTTP implementation from libhttpserver to Drogon and introduces a revamped Plugin Manager, so one C++ binary cannot be assumed compatible across majors. Every CI and acceptance result still records the exact FPP tag and source commit it used; moving prose is not permission for an irreproducible build.

The runtime will have one shared brightness engine and two narrow FPP adapters:

- FPP 9 adapter: the 9.x plugin/lifecycle and libhttpserver surface.
- FPP 10 adapter: the 10.x plugin/lifecycle and Drogon surface.

The latest published FPP 10 beta is an implementation target from the first build. Each later beta, RC, and final release is added to the compatibility matrix and rebuilt against its installed headers; a version label alone is not evidence of ABI compatibility.

## 3. Repository decision

The approved target is three repositories with one-way release dependencies:

| Repository | Responsibility |
|---|---|
| `showmesh` | Coordinator, API/OpenAPI contract, UI/CLI, collectors, night-session integration, brightness contract, and playlist-entry observation ingestion. |
| `fpp-showmesh` | Thin FPP Plugin Manager repository: `pluginInfo.json`, command descriptions, lifecycle scripts, version pins, and committed artifact hashes. |
| `showmesh-fpp-plugin` | Apache-2.0 FPP runtime source: the Go macro helper, C++ brightness engine, native playlist observer, version adapters, tests, and release artifacts. |

All three repositories now exist under `ShowMeshSystems`, but only `showmesh` holds runtime code, so the split is created rather than implemented. **No document may describe the target split as implemented.**

Current reality as of 2026-08-20:

- **`showmesh-fpp-plugin` is created and private**, bootstrapped at commit `a94de7f`. It holds its Apache-2.0 license, a README, a canonical `CLAUDE.md` execution contract with an `AGENTS.md` pointing every other agent at it, and provenance-stamped snapshots of the eleven governing records under `docs/upstream/showmesh/`, pinned to `showmesh` commit `883e94a`. **It contains no runtime code**: the Go helper has not been extracted, the `internal/version` dependency has not been replaced, the `CoordinatorClient` seam does not exist, the C++ component has not been started, and no cross-build has been produced.
- **The Go helper still remains under `showmesh/cmd/showmesh-fpp-plugin`** and is still the only implementation. Extraction is the remaining half of the bootstrap and was deliberately deferred so it happens from inside the new repository rather than across two checkouts.
- **`fpp-showmesh` exists as a private repository** rather than only a local sibling. Its locked-artifact packaging has not been rebuilt.
- Nothing in any of the three has been installed on a real FPP host, and no release, public or private, has been published.

## 4. Install and release contract

After the SM-14 gate passes, `showmesh-fpp-plugin` will publish one versioned release containing:

- statically linked Go helper archives for `linux/amd64`, `linux/arm64`, and `linux/arm/v7`;
- one architecture-independent C++ source bundle containing the shared engine and both FPP adapters;
- a release manifest suitable for producing the packaging repository's lock file.

`fpp-showmesh` commits `artifacts.lock.json` with the exact release version, filenames, and SHA-256 digest for every downloadable artifact. Installation downloads only those exact files, verifies them against the committed hashes, and fails closed on a mismatch. A checksum manifest fetched from the same mutable release location is not the trust anchor.

The Go helper is installed prebuilt. The C++ adapter is selected from the installed FPP major and compiled locally against that host's installed FPP headers and libraries. Installation stages the new files, validates the binary and compiled component, and atomically activates them; a failed download, verification, compile, or activation leaves the previous working version in place. Uninstall reverses every file and service created outside the plugin directory.

This local C++ build is the highest-risk implementation assumption. It must be measured on the slowest supported ARMv7 host for compiler availability, headers, memory, build time, and rollback behavior before official listing or a readiness claim.

The new runtime repository uses Apache-2.0 to match ShowMesh and ADR-010. `FalconChristmas/fpp-brightness` is GPL-3.0 and may be read as behavioral and source evidence, but its implementation is not copied or linked into the Apache-2.0 runtime unless a separate compatibility review explicitly permits that use. The repository records attribution and licenses for every dependency and copied source file.

## 5. Go client seam and future SDK

No shared ShowMesh Go SDK is built now. As part of SM-63, the runtime repository will define a narrow consumer-side `CoordinatorClient` for command handling:

- run a macro, with any buffered prior failures included in the request;
- read a macro definition for the local cache/status behavior.

The existing handwritten HTTP client will move behind that interface without behavioral changes. Local persistence, cached macro data, status rendering, refusal/outage classification, and failure-buffer policy remain plugin-owned.

Prior failures are not a separate client call. They remain part of the run request so the buffer is cleared only after a successful 2xx response, preserving the current transactional behavior.

A future versioned SDK should be integrated through a thin adapter in `showmesh-fpp-plugin`. It cannot directly implement an interface whose method signatures use plugin-local named request/result types: Go requires identical named types, and a `package main` consumer is not importable. Creating a shared contract package now would be the shared dependency this decision intentionally defers. The adapter preserves the seam without forcing that dependency.

The resident C++ component remains independent of this Go interface and any future SDK. It uses FPP-native APIs plus narrow coordinator-facing brightness and playlist-observation contracts.

## 6. Atomic playlist-entry observation contract

ADR-043 requires one atomic entry-identity observation for FPP-backed Playlists. The resident native component owns capture and publication. Independent FPP MQTT topics remain corroborating evidence and are never correlated into a synthetic transition.

### 6.1 Callback boundary and delivery

The FPP 9 and FPP 10 adapters register `playlistCallback`. The callback copies the playlist name, action, section, item position, sequence/media evidence, and local observation time into a bounded in-process handoff and returns. It performs no network request, retry sleep, playlist-definition fetch, hash computation over unbounded input, or filesystem write on FPP's callback thread.

A resident worker resolves the complete playlist definition, constructs the full observation, persists the latest complete state and sequence, and sends an authenticated HTTP `POST` to:

```text
/api/v1/integrations/fpp/playlist-entry-observations
```

The worker reuses the plugin's coordinator base URL and credential source. It does not add a broker credential or a second MQTT publication path. The endpoint is guarded by a new `fpp:observe` scope. The installed plugin principal holds `show:macro:run` and `fpp:observe`; the human operator scopes and `fpp:command` do not become prerequisites for observation ingestion.

The queue is bounded. Under pressure or coordinator outage, intermediate observations may be coalesced to the newest complete state for one FPP instance. The plugin persists the newest unsent state, the monotonic sequence, and a coalesced/dropped count. It retries with bounded backoff outside the callback thread. A successful 2xx acknowledgment clears only the state it acknowledged. `401`, `403`, schema incompatibility, queue pressure, coalescing, and transport outage remain visible in local plugin status.

Current-state convergence is the invariant; replaying every transition is not. The sequence and coalesced count expose a gap so the coordinator never mistakes coalesced delivery for a complete event history.

### 6.2 Identity and canonical playlist revision

The observation schema is versioned independently of the plugin release. Version 1 contains at least:

- schema version;
- persistent FPP instance UUID from the supported FPP system-information API;
- playlist name;
- SHA-256 canonical playlist hash;
- section and zero-based section position;
- sequence and media filenames when present;
- callback action (`start`, `playing`, `stop`, or `query_next`);
- monotonic per-instance event sequence;
- plugin observation time;
- coalesced/dropped count since the previous acknowledged observation.

The canonical playlist hash is SHA-256 over the RFC 8785 JSON Canonicalization Scheme serialization of the complete playlist definition returned by FPP's playlist-definition API. No runtime field is silently removed. The adapter stores the exact normalized definition used for import/reconciliation evidence. Missing UUID, missing definition, unsupported JSON shape, or hash failure produces an explicit unavailable observation and never falls back to filename identity.

The common entry key is derived from instance UUID, playlist name, canonical playlist hash, section, and position. It is deterministic across plugin restarts for an unchanged definition and changes when the definition changes. Duplicate filenames at different positions remain distinct.

### 6.3 Coordinator ingestion

The coordinator authenticates before parsing the full body, applies a strict size bound, validates schema version and instance identity, and accepts an observation only when its sequence is newer than the last accepted sequence for that instance. Repeating the same sequence and identical body is idempotent. A reused sequence with different content, a regression, an unsupported version, or a body whose derived entry key disagrees with its fields is refused and audited.

The endpoint stores the latest full observation and publishes one observation/change-stream update. Track H performs Show, Playlist, Cue, and active-show authorization after ingestion. Accepting plugin evidence is not permission to execute the referenced content.

## 7. Build sequence

1. Extract the current Go runtime and its release workflow from `showmesh` into `showmesh-fpp-plugin`, replacing its `internal/version` dependency and keeping behavior unchanged.
2. Add the consumer-side `CoordinatorClient` and fake-driven command tests; retain HTTP-level regression tests for request, status-code, and failure-buffer behavior.
3. Implement the host-neutral two-value brightness engine, range validation, persistence, versioned full-state synchronization, canonical playlist hashing, identity-event model, and fake-driven tests without FPP headers.
4. Add isolated FPP 9 and latest-beta FPP 10 adapters. Compile each against pinned upstream/bench headers in CI and record exact tags and commits.
5. Implement the bounded callback handoff, worker, persistent latest-state delivery, HTTP client, and coordinator ingestion contract from §6.
6. Produce local/private candidate runtime assets, generate `artifacts.lock.json`, and update `fpp-showmesh` to verify, stage, compile, activate, roll back, upgrade, and uninstall them.
7. Wire the coordinator's brightness contract through Track F and the playlist-entry observation through Track H.
8. Run the packaging linter, container/bench matrix, then the first real-host install under SM-14 using the local/private candidate assets. Publish the versioned release only after that gate passes, unless the owner explicitly changes the standing rule.

## 8. Acceptance evidence required

- FPP 9.4/9.x and the latest published FPP 10 beta at verification time each install, compile, load, upgrade, reboot, and uninstall cleanly, with exact source tags and commits recorded.
- amd64, arm64, and armv7 select and verify the correct Go artifact without relying on `uname -m` alone.
- A corrupt binary or source bundle, missing compiler/header, compile error, incompatible adapter, and interrupted upgrade all preserve the previous working installation.
- Ceiling and gain are independently readable and writable; either can change while the other remains stable.
- `ShowMesh: Set Brightness Ceiling` appears in the FPP Action/Command UI on both supported majors, accepts `targetPercent` 0–100 and `fadeSeconds` 0–86400, rejects missing/malformed/out-of-range input, changes only the ceiling, and can be invoked by an ordinary FPP scheduler entry.
- A 100→75 command with a positive fade duration produces monotonic per-frame values and reaches exactly 75 at the deadline; zero seconds applies 75 immediately.
- Replacing an in-progress fade begins at its current interpolated value without a jump. Restart and late-join tests converge on the same target without ever failing brighter.
- The decisive composition case passes: ceiling 60, fade gain downward, ceiling changes to 40 during the fade, then gain returns to 100 and effective output is 40.
- Configured excluded ranges are byte-identical before and after a fade.
- MultiSync convergence survives duplicate, delayed, stale, and version-incompatible brightness payloads without applying a relative change twice.
- Coordinator outage, `401`, and `403` retain the Go helper's existing local status and prior-failure behavior.
- `playlistCallback` handles all four observed actions, and repeated `playing` with a new section/position is recognized as item advancement.
- Duplicate filenames at different positions produce distinct entry keys; an unchanged definition produces the same key after restart; editing or reordering the definition changes the hash and invalidates the old binding.
- The callback handoff remains within its measured latency budget while the coordinator is slow or unavailable; network, retry, hashing, and persistence never run on the callback thread.
- Queue overflow and outage coalesce only to the newest full state, persist sequence/gap evidence, and converge after reconnect without presenting a complete-history claim.
- The ingestion endpoint rejects wrong scope, oversized payload, unsupported schema, missing UUID, sequence regression, conflicting duplicate sequence, and a mismatched derived entry key.
- The installed component survives reboot and does not bind a second UDP 32320 listener.

Until these run on the intended hosts, Track F readiness rejects any configuration that requires compositional FPP brightness. The design decision removes an architecture unknown; it does not remove the verification gate.

## 9. Open deployment facts

- Record the current 10 PM/11 PM percentages and the participating hosts before migrating the confirmed `fpp-brightness` scheduler entries.
- Inventory the installed `FalconChristmas/fpp-brightness` versions so removal and rollback are explicit.
- Record channel ranges that must be included or excluded on each host.
- Measure local C++ compilation on the slowest ARMv7 target and confirm the available FPP development headers for both supported majors.
- Revalidate against the latest FPP 10 beta at build time, then at each RC/final release and whenever FPP changes its plugin ABI, HTTP framework, or Plugin Manager lifecycle.
