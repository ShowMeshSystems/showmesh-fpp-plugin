# Prebuilt native plugin distribution: can the .so ship instead of compile on host?

[Distribution research](../upstream/showmesh/docs/research/RES-015-fpp-plugin-distribution-model.md) · [Brightness/plugin runtime research](../upstream/showmesh/docs/research/RES-018-fpp-brightness-control.md) · [Pinned FPP versions](../../native/adapters/FPP-PINS.md)

Status: research, no design change proposed yet · Verification: L1 source reads against the pinned FPP commits and this repository's own release tooling, for every answer except item 5, which is an emulated compile measurement labelled separately below. No dlopen was observed on any FPP build, no install was run on a real or emulated host except the emulated compile, and nothing here has been installed on a real player.

## Recommendation

Ship a prebuilt native object for FPP 10.x, keep on-host compilation for FPP 9.x. FPP 10's own plugin loader independently checks an API version integer plus four ABI probe values at `dlopen` time (question 1) and refuses a mismatched object with a logged error, so a stale or wrong prebuilt object fails loudly, the same failure mode an unbuildable host already produces today. FPP 9 has no such check: any `.so` that exports `createPlugin` loads and runs, whether or not its ABI actually matches the running `fppd`. A wrong prebuilt object on FPP 9 would load silently and misbehave or crash at runtime, which is strictly worse than today's compile failure. Compiling on FPP 9 hosts is not just the current mechanism, it is also the only one FPP 9 can self-defend against getting wrong.

This is a partial answer, not a full replacement of the on-host build. It reduces the FPP 10 install path from "compile local source against host headers" to "fetch and verify one of a small set of prebuilt objects, or fall back to compiling," while leaving FPP 9 exactly as designed today.

## 1. What the Plugin API 6 fingerprint encodes

Read at FPP 10.0, commit `370e62ed7e8c8318da6ee5b01312b8b75082d952`.

`src/Plugins.cpp` lines 677-747 gate every C++ plugin load in two stages, both run inside `PluginManager::loadPlugin` right after `dlsym(handle, "createPlugin")` succeeds:

- Lines 689-707: `dlsym` for `fpp_plugin_api_version`, confirmed to be the plugin's own symbol (not `libfpp.so`'s copy of the same weak symbol) via `dladdr` base-address comparison against `createPlugin`'s own base, then compared against `FPP_PLUGIN_API_VERSION`, defined in `src/Plugin.h:85` as `6`. A mismatch or a missing/foreign symbol refuses the load with `WarningHolder::AddWarning(5, ...)`.
- Lines 710-747: an `abiSizeMatches` lambda independently compares four values the plugin's own headers compute against the four the running `fppd` computes: `sizeof(Command::CommandArg)` and `sizeof(Command)` (both defined in `src/commands/Commands.h:252,255`), `sizeof(FPPLoggerInstance)`, and a measured pointer-offset span across `FPPLogger`'s facility members (`src/Plugin.h:103-109`). Each is exported as its own weak symbol and each comparison uses the same provenance check as the API version.

The header comment at `src/Plugin.h:79-86` and the code comment at `src/Plugins.cpp:710-716` both explain why: the API version integer only helps "if somebody remembers to bump it," and it has been forgotten before (a member was added to `CommandArg` without a version bump, twice, per the comment). The four ABI probes are the second, automatic gate for exactly that failure mode.

**What this is, concretely:** an integer API version plus four measured struct-size/offset values for the specific types FPP has been burned by before. It is not a compiler-identity check, not a compiler-flags check, and not an FPP commit or version string check. It is coarser than a full build fingerprint and narrower than a general ABI checksum: it catches known ABI hazards in `Command`, `CommandArg`, and the logger layout, and nothing else. A plugin using a different C++ standard library ABI, or disagreeing about any other struct FPP's headers export, is not caught by this mechanism at all.

**Load-portability answer, and where it is inference rather than an observed result.** `FPP-PINS.md` already recorded that `Plugin.h` and `Plugins.h` are byte-identical between the `10.0-beta5` pin and the `10.0` final tag, so those two builds already collapse to one fingerprint value. Based on what the fingerprint is derived from, a `.so` built against `10.0` would be accepted by a `10.0.x` point release that keeps `FPP_PLUGIN_API_VERSION` at `6` and does not change the layout of `Command`, `CommandArg`, `FPPLoggerInstance`, or the logger facility span. That conclusion is inference from source, not an observed `dlopen` result: nothing was actually loaded into a running `fppd` for this record, and the fingerprint's own comment says a human has to remember to bump the version when a change requires it, so a point release could in principle change an unrelated ABI-sensitive detail this mechanism does not probe (a different struct, a different STL, a different compiler) without the load being refused at all.

Read at FPP 9.5.3, commit `7979a4bb0bb9068fea71f3b447e273d5c0ea01e3`: `Plugins.cpp` loads a `.so` with `dlopen`, resolves `createPlugin` via `dlsym`, and calls it. There is no API version symbol, no `dladdr` provenance check, and no ABI probe of any kind on this major. `FPP-PINS.md`'s "not versioned" note is confirmed directly in this major's own `Plugins.cpp`.

## 2. How many prebuilt objects the supported range needs

Supported range: FPP 9.4-9.x (no check at all, so every point release is architecture-distinct only, per §1), FPP 10.x (fingerprint-distinct, per §1), across arm64 and armv7, plus amd64 for the bench.

- **FPP 9.4-9.x:** three architecture objects (arm64, armv7, amd64), one build. Because FPP 9 performs no ABI check, "distinct" here means only architecture, not build. That is not evidence of safety, it is the opposite: nothing on the FPP 9 side would refuse a `.so` built against a different 9.x point release's headers if one silently differs, so a single build serving every 9.x point release is an assumption this record cannot verify from source, only from the fact that FPP declines to check.
- **FPP 10.x:** three architecture objects (arm64, armv7, amd64) per distinct fingerprint value. Today there is exactly one fingerprint value in the wild for the pinned range (`10.0-beta5` and `10.0` final already collapsed, per §1), so the current supported range needs three objects. A future FPP 10 release that bumps `FPP_PLUGIN_API_VERSION` or changes one of the four ABI probes would add three more; one that does not needs no new build.

**Concrete count for the range as pinned today: six objects** (three per major, times two majors), collapsing to three per major as long as each major's builds keep the same fingerprint value. What makes two builds distinct: for FPP 10, the `(FPP_PLUGIN_API_VERSION, four ABI probe values)` tuple; for FPP 9, architecture alone, because no build-identity check exists to make anything finer meaningful.

## 3. Digest-locked prebuilt shape, and the fallback question

The Go helper's existing discipline, read from `Makefile`, `scripts/write-release-manifest.sh`, and `scripts/verify-release-manifest.sh`: each architecture's tarball is built, hashed with `sha256sum`, and the resulting filename, size, and digest are written into `release-manifest.json` (`kind: "go-helper"`), which `fpp-showmesh`'s `artifacts.lock.json` commits as its own trust anchor. `scripts/lib/verify.sh`'s `sm_verify_sha256` in the packaging repository checks a download against that committed hash, never against a manifest fetched from the same host as the artifact.

**What the release manifest gains.** A new artifact kind, for example `native-prebuilt-fpp10`, one entry per `(architecture)` for the currently pinned FPP 10 fingerprint, following the exact shape `write-release-manifest.sh` already uses for the Go helper: filename, size, sha256. The manifest already carries `fppSupport.fpp10.tag` and `.commit`, so the fingerprint the artifacts were built against is already recorded; nothing new is needed there. Nothing changes for the FPP 9 `native-source` bundle or for the Go helper artifacts.

**How the installer decides which object to fetch.** `scripts/lib/native.sh`'s `sm_fpp_major` already reads the running host's FPP major out of `$FPPDIR/www/fppversion.php`, generated at FPP build time from the checked-out tag. For FPP 10, the installer would need one more fact this file does not currently expose: whether the running host's fingerprint matches the one the prebuilt object was built against. The fingerprint has two parts, and they are readable differently. `FPP_PLUGIN_API_VERSION` is a plain literal, `src/Plugin.h:85`, and `Plugin.h` is present on every host at `$FPPDIR/src`, the same directory `sm_native_compile` already requires to compile against, so the installer can read that integer off the host with a grep, no compile needed. The four ABI probe values cannot be read the same way: they are `sizeof()` and pointer-offset results, not literals, so getting them off a live host means compiling a small probe or trusting a build already known to match. A host-side read of the API version therefore narrows the match question but does not close it: two hosts could report the same `FPP_PLUGIN_API_VERSION` while differing in one of the four probe values FPP has already changed underneath that number before, per the comment cited in §1. The safe selection rule is therefore not "any FPP 10.x host gets the prebuilt object," it is "a host whose reported FPP 10 version is one this repository has verified against the fingerprint the artifact was built for gets the prebuilt object; everything else compiles." That verification step is exactly what `FPP-PINS.md` already does for compiling ("added and rebuilt against its own headers rather than assumed compatible") and does not go away for a prebuilt catalog, it changes what the release process produces once verification passes: a binary instead of confirmation that source still compiles.

**How it verifies.** `sm_lock_expected_sha256` (already in `scripts/lib/lock.sh`) resolved before any network access, then `sm_verify_sha256` against the downloaded bytes, identical to the Go helper's existing gate.

**Fallback, answered explicitly.** When no prebuilt object matches the running FPP 10 version, the installer falls back to the existing on-host compile path, it does not refuse the install. The operator-facing reason: on-host compile is the currently accepted, already-working mechanism, and refusing an install on a host running a legitimate but not-yet-cataloged FPP 10 point release (for example, a release published between two prebuilt catalog updates) would regress a working install path for a marginal build-time saving. The prebuilt object is a shortcut for known, verified builds, not a hard requirement.

**FPP 9 gets no prebuilt object under this recommendation**, so this question does not arise there; every FPP 9 install continues to compile, as it does today.

## 4. Precedent in FPP's own plugin ecosystem

**Plugin manager's own install path** (`scripts/functions` at FPP 10.0, lines 61-66 and 517-523): FPP's core upgrade path runs `make -C <plugin-dir> -f <plugin-dir>/Makefile SRCDIR=$FPPDIR/src` unconditionally for any installed plugin directory that carries a Makefile. This is the mechanism `fpp-showmesh/scripts/lib/native.sh`'s own header comment already cites as the reason it must build the resident component itself rather than rely on FPP: "neither FPP 9.5.3 nor FPP 10.0 runs `make` on a plugin directory" through the Plugin Manager's install/upgrade callback, only through this unrelated core-upgrade sweep. FPP's plugin manager neither builds a plugin for the installer at install time nor forbids a plugin from building itself; the install script (`scripts/fpp_install.sh`, run as root by `install_plugin`) is free to do either.

**`FalconChristmas/fpp-brightness`** (read-only, source and behavior evidence only, no code copied; GPL-3.0, this repository is Apache-2.0): its `fpp_install.sh` runs `make "SRCDIR=${SRCDIR}"` in the plugin directory at install time. It ships C++ source and a Makefile, no compiled `.so`, and compiles on every install. This matches RES-015's corpus finding directly: 16 of 56 surveyed plugins build on device, and a grep across every blob in that corpus found no precedent for committing a compiled binary at all.

**Conclusion:** upstream's own reference C++ plugin compiles on the host. Nothing in FPP's plugin manager either provides or requires that compile step; the plugin's own install script owns it, exactly as this repository's does today.

## 5. On-host compile time and peak memory on the slowest supported ARMv7 host

**PLACEHOLDER.** This measurement is being taken by the task's manager in a QEMU armv7 container on the build VM, not by this record's author, and will be labelled explicitly as an emulated result (emulating host and load stated) when it lands, not as a real-host measurement. It is not blocking the rest of this record.

## What this record does not prove

- No dlopen was observed on any real FPP build; §1's load-portability conclusion is inference from source, stated as such.
- No install, upgrade, or uninstall was run on a real or emulated FPP host of any kind (the one emulated result covers compile time only, once received).
- No fleet evidence: nothing here has been tried against `fpp-player`, `fpp-remote-a`, or `fpp-remote-b`.
- The prebuilt shape in §3 is a design description, not an implementation. No release manifest, artifact, or installer change described here has been built.

## Implementation issues this recommendation implies, if adopted

- Extend `native/adapters/Makefile` (or add a sibling target) to produce a prebuilt FPP 10 `.so` per architecture, built against the pinned FPP 10 headers the same way CI already compiles the adapter for verification.
- Extend `scripts/write-release-manifest.sh` and `scripts/verify-release-manifest.sh` with a `native-prebuilt-fpp10` artifact kind, one entry per architecture, alongside the existing `go-helper` and `native-source` kinds.
- Record, alongside `native/adapters/FPP-PINS.md`'s existing table, which FPP 10 point releases have been verified to share the current `(FPP_PLUGIN_API_VERSION, four ABI probe values)` fingerprint, so the packaging repository's install script knows when a new FPP 10 release is safe to match against an existing prebuilt object versus needing a fresh one or a fallback to compile.
- Change `fpp-showmesh/scripts/lib/native.sh`'s `sm_install_native` (or add a sibling path) to attempt a digest-verified prebuilt fetch for FPP 10 hosts whose reported version is in the verified set, and fall back to the existing `sm_native_compile` path otherwise; FPP 9 is unchanged.
- Add a bench case exercising the fallback: an FPP 10 host reporting a version outside the verified set still ends up with a working, compiled component.
- Evaluate, when the installer change is designed, whether reading `FPP_PLUGIN_API_VERSION` from the host's installed `Plugin.h` is worth adding as a cheap pre-check before choosing a prebuilt object, given it narrows but does not close the match question on its own.
