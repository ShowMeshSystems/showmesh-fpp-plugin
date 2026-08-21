# FPP plugin-load bench

This is bench scaffolding for exercising the ShowMesh FPP adapters inside a
containerized `fppd`. It is not part of the ShowMesh product, is never
installed on a real host, and ships nothing.

## What this bench can and cannot establish

Read this before running anything, and re-read it before quoting any result
below.

**A container is not a real host, and nothing here closes the real-host
install gate.** Every assertion in this bench runs against `fppd` in a Docker
container that the bench itself set up, using an install the bench itself
performed. Real-host install, filesystem permissions and ownership,
packaging, and cross-version compatibility on the fleet remain unproven and
belong to the real-host install work. `native/adapters/FPP-PINS.md` says the
same thing about the compile alone, and this bench does not change that.

**Every run so far was emulated.** The host was `arm64`; both FPP images are
`linux/amd64`, and Docker printed its platform-mismatch warning on every
container start. So every container observed here executed under x86_64
emulation, which is a further step away from a real host than a container
alone. A `ps` inside a running container showed apache re-exec'd through
`/run/rosetta/rosetta`, so the emulation layer actually observed was Rosetta.
The script prints the host architecture and an unconditional emulation notice
for exactly this reason. **Nothing here speaks to native arm64 or ARMv7
behaviour on real fleet hardware.**

**The in-container compile duration is not a real-host number.** The final
runs measured the adapter compile at **28.6 s (FPP 9, 28569 ms)** and
**46.3 s (FPP 10, 46279 ms)**. Earlier runs of the identical compile, on a
less loaded machine, took 18.4 s and 19.4 s for FPP 9 and 18.9 s and 20.2 s
for FPP 10. All of those were measured under emulated x86_64 on an arm64
host, and the final pair was measured while other emulated bench containers
were also running. Never quote any of these figures without that caveat, and
note the stronger point: the same work varied from 18 s to 46 s across runs
on one machine. That spread, more than the caveat, is why none of these
numbers is a packaging time estimate or a real-host figure.

**No latency, throughput, or per-frame cost claim can be made from this
bench.** There is no pixel output hardware here, the frame path runs under
emulation, and nothing in the assertion set measures cost.

**Driving `playlistCallback` through to a published observation is
structurally blocked, and that is a known gap.** The observation sink
contract is not frozen, and both shipped adapters deliberately pass
`nullptr`. No sink was added to the shipped adapters for this bench. So
end-to-end observation delivery is untestable here until that contract lands.
This bench asserts nothing about it.

One further gap worth naming: FPP 9's settings-listener withdrawal is not
independently observable from outside the process. Only process exit and the
crash check are observable there, and on FPP 9 the crash check currently
fails (see the assertion table).

What the bench *does* produce is real evidence that a pinned `fppd` finds,
`dlopen`s, constructs, and registers the adapter; that the registered command
carries the vocabulary the adapter declares; that an invoke reaches the
adapter's own argument handling; that channel data downstream of
`modifyChannelData` is scaled and faded exactly as the plugin's arithmetic
says; that no second UDP 32320 listener appears; that the plugin survives a
container recreate; and, on FPP 10, that a runtime unload really withdraws
the registration.

## What it runs, and the pins

Both FPP versions are built from source at a commit-pinned tree, taken from
`native/adapters/FPP-PINS.md`:

| Major | Tag | Commit | Plugin ABI version declared by the header |
|---|---|---|---|
| FPP 9 | `9.5.3` | `7979a4bb0bb9068fea71f3b447e273d5c0ea01e3` | not versioned |
| FPP 10 | `10.0-beta5` | `741cfc4344bd0d1b913941d507c3a123a8c82e5a` | 6 |

FPP 9 is the default. FPP 10 is a documented switch, `--major fpp10`. The
script verifies that the tag it fetched resolves to the pinned commit before
building, because a tag is a moving name and the pin is the commit. It also
prints the resolved image reference, and labels a local image ID as a local
build output rather than presenting it as a digest anyone else can pull.

`git describe --tags` inside the two final containers returned `9.5.3` and
`10.0-beta5` respectively, matching the pinned commits the script printed.

## Layout

```
bench/fpp-plugin-load/
  docker-compose.yml           FPP service, built from a commit-pinned local checkout
  docker-compose.prebuilt.yml  override: private prebuilt FPP 9 fixture image, opt-in
  .env.example                 copy to .env and adjust, or use the script flags
  plugin/callbacks             the plugin declaration file fppd execs
scripts/test-plugin-load-fpp.sh  drives a run and reports each assertion
```

## Quickstart

```
scripts/test-plugin-load-fpp.sh --major fpp9
scripts/test-plugin-load-fpp.sh --major fpp10 --id ci --port 8290
scripts/test-plugin-load-fpp.sh --id ci --down
```

Or through the root `Makefile`, which forwards `MAJOR`, `BENCH_ID`, and
`BENCH_HTTP_PORT` in either the make-variable or the environment form:

```
make test-plugin-load-fpp
make test-plugin-load-fpp MAJOR=fpp10 BENCH_ID=demo BENCH_HTTP_PORT=8299
BENCH_ID=envid BENCH_HTTP_PORT=8298 make test-plugin-load-fpp
```

The target is deliberately not part of `make check`: it needs a real
container runtime and a multi-minute image build on a fresh machine.

Flags:

| Flag | Meaning |
|---|---|
| `--major fpp9\|fpp10` | Which pinned FPP major to stand up. Default `fpp9`. |
| `--id ID` | Isolates this run. Default `local`. See below. |
| `--port PORT` | Host port for the container's HTTP API. Default `8190`. |
| `--prebuilt` | Use the private prebuilt FPP 9 fixture image instead of a source build. Refused for `fpp10`. |
| `--down` | Tear this run down and exit, including its named media volume. |
| `-h`, `--help` | Full flag list. |

Environment variables, all with working defaults and all listed in
`.env.example`: `BENCH_ID`, `BENCH_HTTP_PORT`, `BENCH_FPP_MAJOR`,
`BENCH_USE_PREBUILT`, `FPP_PREBUILT_IMAGE`. The default port is 8190 rather
than 8090 so this bench and the sibling multisync bench can run at the same
time.

`docker`, `curl`, and `jq` are hard requirements; a missing one is a failure,
never a silent skip. A normal run **leaves the container running**, the same
asymmetry the sibling bench documents, because the image build is expensive.

## Per-run isolation

`BENCH_ID` parameterizes the compose project name, the container name, the
network name, and the named media volume; `--port` parameterizes the
published host port. Every one of those carries the id, so two runs with
different ids share nothing.

This exists because a shared singleton bench `fppd` with global state is a
known failure mode: `/home/fpp/media` is `fppd`'s own state, holding settings,
the installed plugin, and `fppd.log`, and leaking it between runs makes
results from one run silently depend on another.

Isolation was verified by running three benches concurrently, including two
on the same FPP major, which is the stronger case. Observed while all three
were live: three distinct containers on ports 8230, 8221, and 8220, three
distinct networks, three distinct media volumes, and each port answering
independently with exactly one ShowMesh command registered. A fourth
concurrent id was live later in the same session. No collision at any point.

`--down` passes `docker compose down -v`, so it removes the named media
volume as well as the container and network, and is a real reset. Without
`-v`, `fppd.log`, the settings file, and the installed plugin survived into
the next run with the same id, which is precisely what made an earlier
version of one log check permanently satisfied.

## The bench-owned install

The packaging repository has **no fetch, compile, or activate step for the
native component today**, and ships no `callbacks` file. So this bench
performs its own install:

1. mounts this repository's `native/` tree read-only into the container,
2. copies it to a writable path so nothing can be written back into the
   read-only mount,
3. compiles the adapter in-container against **that image's own
   `/opt/fpp/src`**, and times the compile,
4. places `libshowmesh-fpp<major>.so` and the `callbacks` file directly into
   `/home/fpp/media/plugins/fpp-showmesh/`,
5. raises `LogLevel_Plugin` to `debug` so a load failure is visible,
6. recreates the container so `fppd` re-scans the plugin directory.

**This is bench-owned and is not the real installer.** It proves nothing
about how the plugin would arrive on, or be activated on, a real host.

Nothing under `native/` is modified by a bench run. The bench passes FPP 10's
`-fno-gnu-unique` on its own compile line, which is now redundant but harmless:
`native/adapters/Makefile` sets that flag for the FPP 10 link itself, so a real
host compiling through the product makefile gets it too. It is applied to
FPP 10 only, because only the FPP 10 adapter declares an unload contract.

Two settings the bench writes are bench-owned accommodations and not product
settings: `LogLevel_Plugin = debug`, and `DisableFakeNetworkBridges = 1`,
which is needed only because this bench captures channel output on loopback
inside the same container that sends it.

Restarts go through `docker compose up -d --force-recreate`. A plain
`docker restart` reproduces the apache/php stale-pid crash-loop the sibling
bench documents.

## The load mechanic, and its traps

Described behaviourally. This bench copies no upstream code.

- `fppd` scans `/home/fpp/media/plugins`. For each directory it looks for a
  declaration file named `callbacks`, extensionless and executable, and runs
  it through FPP's own event-script wrapper with `--list`.
- The declaration file's output is trimmed at the ends and split on commas,
  and the resulting tokens are compared exactly and case-sensitively. **It
  must print exactly one `c++:<filename>` token**, with no spaces, no
  trailing comma, and no `media`, `playlist`, or `lifecycle` token anywhere.
  If any of those three tokens appears, the directory is treated as a script
  plugin and `dlopen` is never reached at all.
- **The shared object path derives from the plugin directory name**, not from
  the plugin's own name. The bare `c++` token selects `lib<dirname>.so`,
  which does not match `libshowmesh-fpp9.so` or `libshowmesh-fpp10.so`, so
  the `c++:<filename>` form is required. The committed `plugin/callbacks`
  checks which of the two shared objects the bench installed next to it, so
  one committed file serves both majors.
- FPP 9 looks up exactly one symbol, the plugin factory, and performs no ABI
  check. **FPP 10 additionally gates on the plugin's own ABI version symbol**
  and on a set of `sizeof` fingerprint checks. Both adapters already export
  what each major looks for.
- Plugin log lines are read from `/home/fpp/media/logs/fppd.log` through
  `docker exec` on both majors. On FPP 10, `fppd` writes nothing to stdout
  when a log file is configured and stdout is not a TTY, so `docker logs` is
  empty for plugin lines. No assertion in this bench takes `docker logs` as
  input; it is used only for diagnostics.
- FPP does not validate command arguments in either major. The `min` and
  `max` values from the command vocabulary are UI metadata and are never
  consulted on the invoke path, and there is no integer type check. **The
  refusal text for an out-of-range or non-integer argument comes from the
  ShowMesh runtime's own brightness command handler, not from FPP.**

## The assertions, and their observed results

Each assertion is named, and each records PASS or FAIL independently. An
early exit cannot look green: the summary is printed from an `EXIT` trap, and
recording fewer than eight results is itself counted as a failure.

Observed on the final version of the script:

| Assertion | What it actually asserts | FPP 9 | FPP 10 |
|---|---|---|---|
| `A1_loads_plugin` | The plugin's command is registered with the declaration file in place and absent without it, and the log tail from *this* restart carries the load line and no load-failure line. | PASS | PASS |
| `A2_command_vocabulary` | The registered command declares `targetPercent` min 0 max 100, and `fadeSeconds` min 0 max 86400 default 0, under those exact key names. | PASS | PASS |
| `A3_invoke_and_ranges` | An in-range invoke returns 200; out-of-range `targetPercent`, non-integer `targetPercent`, and out-of-range `fadeSeconds` each return 500 carrying the ShowMesh runtime's own refusal text. | PASS | PASS |
| `A4_channel_output_scaled` | Real channel output on the wire, downstream of `modifyChannelData`: a `0xff` source byte stays `0xff` at ceiling 100 and becomes `0x80` at ceiling 50, computed from the plugin's own integer formula rather than hardcoded. | PASS | PASS |
| `A5_fade_monotonic_exact_and_immediate` | Over a fade: enough frames sampled to be a measurement, strictly decreasing overall, non-increasing frame to frame, landing on the exact 75 percent value `0xbf`, and `fadeSeconds` 0 stepping straight to the target with no intermediate value. Observed 129 frames on FPP 9 and 133 on FPP 10. | PASS | PASS |
| `A6_clean_teardown` | `fppd` exits after the stop signal with the plugin still resident and writes no crash signature to the log tail taken after that signal; on FPP 10, additionally that a runtime unload returns `loaded:false` **and** withdraws the plugin's registered command, and that a second shutdown with the plugin unloaded is also clean. | PASS | PASS |
| `A7_no_second_multisync_listener` | Exactly one UDP 32320 socket exists, held by `fppd`, both before and after a restart. | PASS | PASS |
| `A8_restart_survival` | The command is registered again after a container recreate, and that restart's own log tail carries the load line with no load-failure line. | PASS | PASS |

**Both runs exit 0, with all eight assertions passing on both majors.**

### A6 on FPP 9 caught a real product defect, since fixed

This assertion failed on FPP 9 when the bench first ran, and the failure was a
genuine defect rather than a bench problem. `fppd` caught the stop signal
cleanly, logged its shutdown, and then segfaulted during plugin teardown:

```
Crash handler called in thread 51:  signal=11 (SIGSEGV: Segmentation fault)
```

The crash handler's backtrace put the fault inside the FPP 9 adapter's
destructor, reached from `PluginManager::Cleanup()`. FPP 9 deletes every
registered command immediately before it destroys plugins, so the destructor
was reaching through an already-freed command to unregister it and then
deleting it a second time. Reproduced with the pre-fix adapter both under
emulation and natively, so it was not an emulation artifact. The adapter now
withdraws the command by name and never touches the freed pointer, and A6
passes on both majors.

The bench was not changed to accommodate any of this. The assertion is the
same one that failed, and it stayed failing until the product was fixed.

### Every assertion was proved capable of failing

Twelve deliberate break cases were run against live containers, one per
assertion branch, applied to a throwaway copy of the script so the committed
script was byte-identical before and after each case. Eight of the twelve
broke the environment or the product rather than an expected value, which is
the stronger form: corrupting the installed shared object, making the two
ceiling captures identical, making the plugin inert, turning an instant apply
into a fade, never signalling `fppd`, binding a second UDP 32320 socket with
`SO_REUSEPORT`, and deleting the declaration file before a restart. Each
named assertion reported FAIL and each case exited 1.

One case earned its keep. Pointing the FPP 10 unload endpoint at a plugin
directory that does not exist still answered `200` with `loaded:false`,
because that endpoint is idempotent by design and answers the same way for a
plugin that was never loaded. The unload response alone was therefore not
evidence that anything had been unloaded. A6 was **strengthened** in response,
to require the plugin's own registered command to be present immediately
before the unload and absent immediately after, and the same break then
failed as it should. Nothing was relaxed.

A6's crash-signature branch needs no synthetic break: the real FPP 9 run
fails it on a real SIGSEGV.

## Why the build context diverges from the sibling bench

The sibling multisync bench builds FPP from a remote git-URL Docker build
context. This bench cannot, and the divergence is forced rather than chosen.

Keeping the commit-pinned tree requires `EXTRA_INSTALL_FLAG=--skip-clone`;
without it FPP's installer deletes `/opt/fpp` and re-clones from GitHub, so
the image would not carry the tree the build context supplied. But
`--skip-clone` needs **both a real `.git` directory and a ref literally named
by the tag**, and a git-URL context provides neither: it exports a working
tree with no `.git`, and a checkout made by fetching a bare commit SHA has no
ref named by the tag. Both failures were observed, on both majors:

```
fatal: not a git repository (or any of the parent directories): .git
error: pathspec '10.0-beta5' did not match any file(s) known to git
```

FPP 10 additionally could not be built from a git-URL context at all, because
Docker initialises submodules recursively for such a context and that failed:

```
fatal: No url found for submodule path 'external/rpi_ws281x' in .gitmodules
```

So the script prepares a **local, commit-pinned checkout** per major under
`.fpp-src/` (git-ignored, fetched on demand and verified against the pinned
commit) and uses that as the build context. The detail behind these failures
lives in the issue tracker and the bench's own working notes, not here.

## The prebuilt image override

`docker-compose.prebuilt.yml` points the service at a private prebuilt FPP 9
fixture image published to GHCR by the related CI work. It is selected only
by an explicit `--prebuilt` (or `BENCH_USE_PREBUILT=1`) and requires
`FPP_PREBUILT_IMAGE` to be set to an exact reference. When selected, the
script pulls before bringing the service up and **hard-fails the whole run if
that pull does not succeed**, specifically so a failed pull can never quietly
fall through to a source build and be mistaken for having exercised the
fixture path.

**This path was not usable from the machine this bench was built on.**
`docker manifest inspect` on the fixture returned `denied`: the package is
private and the available token carried no `read:packages` scope. That is an
access problem with one machine's credentials and not evidence the fixture
itself is broken; a developer or CI runner with GHCR access should be able to
use it. It was not exercised here.

There is **no FPP 10 fixture image**. `--major fpp10 --prebuilt` is refused
before it reaches the compose files; FPP 10 is source build only.

## Tearing down

A normal run leaves its container running on purpose. Tear one run down,
including its named media volume, without touching any other run:

```
scripts/test-plugin-load-fpp.sh --id <id> --down
```

To see what a session left behind:

```
docker ps -a   --filter name=showmesh-fppbench
docker volume ls --format '{{.Name}}' | grep fppbench
docker network ls --format '{{.Name}}' | grep fppbench
```

The local FPP checkouts under `.fpp-src/` and the built
`showmesh-bench/fpp:*` images are not removed by `--down`. Delete them by
hand when you want the disk back; the next run re-fetches and re-verifies the
checkout against its pinned commit, and rebuilds the image.
