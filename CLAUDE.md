# showmesh-fpp-plugin

The ShowMesh runtime that lives on an FPP host: a forked Go macro helper and a
resident C++ brightness and playlist-identity component, for FPP 9.4-9.x and
FPP 10.x.

Read [`README.md`](README.md) for what this plugin is and how the three
repositories divide the work. This file is the execution contract.

## Scope of this repository

This repository owns runtime source only. It does not own the coordinator, the
public API, the OpenAPI contract, the UI, the CLI, or packaging.

- Coordinator surfaces and the API contract live in `ShowMeshSystems/showmesh`.
- Plugin Manager packaging lives in `ShowMeshSystems/fpp-showmesh`.
- Everything else on an FPP host lives here.

**Never implement coordinator-side work in this repository.** If a coordinator
contract is missing or ambiguous, stop that seam and report the exact
conflicting citations. Do not invent a parallel wire shape and reconcile later.

## The governing records are frozen

The product and architecture decisions are already made. They are snapshotted at
a pinned upstream commit under
[`docs/upstream/showmesh/`](docs/upstream/showmesh/UPSTREAM.md), and that
directory's rules govern how they are read and refreshed.

Read RES-018 before touching anything, and read ADR-043 and TRACK-H before
touching playlist identity. You implement these records. You do not run another
architecture pass, relitigate a settled choice, or replace the work breakdown.

If reality contradicts a record, that is a real finding and worth raising. Raise
it once, precisely, with citations, and stop the affected seam. Do not work
around it silently.

## Frozen implementation rules

These are not preferences. Each is either an accepted decision or a defect this
project already paid for.

- **Apache-2.0.** Upstream GPL code, `FalconChristmas/fpp-brightness`
  specifically, is behavioral and source evidence only. It is not copied or
  linked unless a separate compatibility review explicitly permits it.
- **FPP 9.4-9.x and the latest published FPP 10 beta at build time.** Pin the
  exact tags and commits in CI evidence. A build that floats silently across FPP
  versions is not a supported build.
- **The Go helper stays a forked command helper.** The C++ component is resident
  and uses isolated FPP 9 (libhttpserver) and FPP 10 (Drogon) adapters wrapped
  around host-neutral cores. The C++ component does not replace the script
  command.
- **Use `playlistCallback` for playlist identity.** Never `eventCallback`, never
  `MultiSyncPlugin`.
- **The callback copies bounded evidence and returns.** No HTTP, no retry, no
  hashing, no playlist fetch, no persistence, and no sleep runs on FPP's
  callback thread. That thread belongs to a running show.
- **The resident worker implements RES-018 section 6 exactly**: authenticated
  HTTP, the `fpp:observe` scope, an RFC 8785 canonical playlist hash, the
  deterministic entry key, a persistent monotonic sequence, bounded latest-state
  coalescing, and explicit gap evidence.
- **Never bind a second UDP 32320 listener.** ADR-013.
- **Preserve the existing Go helper's handwritten HTTP behavior** and its
  2xx-only prior-failure flush. Prior failures ride in the run request so the
  buffer clears only on a successful response. That is transactional on purpose.
- **A credential is never a command argument, a URL query parameter, or a child
  process environment variable.** FPP broadcasts command executions with their
  arguments in cleartext over MQTT.
- **Candidate artifacts stay private** until the first real-host install gate
  passes and the owner approves publication.

## Evidence standard

**Passing unit tests is not completion.** A seam closes when its stated
acceptance behavior is observed, not when the suite is green.

- Run the complete relevant gate and report the exact commands and results.
- Try the failure cases the issue names. A test that passes whether or not the
  bug is present also reports success, which is worse than no test.
- Inspect the diff for copied secrets and generated artifacts before calling
  anything done.
- Use `should` for anything you have not observed. Never write a comment, log
  line, string, or document that claims verification that did not happen.
- Hardware-only evidence stays open. A unit test, a container, and a fake are
  not a real-host result, and the first real-host install is a separate gate
  this repository does not close on its own.

## Code comments

A code comment is not documentation. Write the minimum a reader needs to
understand *this code*, and nothing else.

- One or two sentences, explaining a non-obvious invariant, a safety boundary,
  or an external-system trap that the names and types cannot express.
- No narrative history, no design essays, no rationale, no review findings, no
  restatement of the next statement.
- **Never put an issue tracker reference in source, tests, or any published
  contract.** No issue key, no tracker URL, no ticket number in any form. A
  reader a year from now cannot open the ticket and does not care which one
  produced the line. A necessary TODO is the one exception and names its issue.
- Citations to a decision record belong where the constraint is operative, not
  scattered as decoration.

If it explains why the project made a choice, it belongs in an upstream ADR. If
it explains what was measured, it belongs in a research record. If it explains
what changed, it belongs in the commit message or the issue.

## Work tracking

Linear is the work ledger, and it is not public. Work one assigned child issue
at a time unless the dependency graph explicitly permits parallel work.

- Read the issue and its parents and relations before editing code.
- Move the child to In Progress when work actually starts.
- Put decisions and acceptance evidence on the child, not only in terminal
  output.
- Do not create replacement issues, broaden scope, or reopen frozen decisions.
  If a real gap appears, post one concise blocker with the exact conflicting
  citations and stop that seam.
- Do not post running narration. Comment only for a genuine blocker, a contract
  change affecting another child, or completion.
- Keep a comment to at most four short sentences plus links or a compact gate
  block.
- On completion, attach the pull request, report the gates you actually
  observed, name what remains unverified, and move the child to In Review. Never
  mark the parent or the real-host gate complete from a child session.

## Voice

Be direct. Lead with the outcome, then only the evidence needed to review it.

- Do not overthink settled choices or restate the whole issue back.
- Do not produce essay-length plans or progress reports.
- README prose is human-readable and complete, but edited for repetition.
- Test names and failure messages state behavior precisely, without commentary.
- Never use an em-dash. Use a comma, a period, a colon, a semicolon, or reword.

## Publishing

Do not publish, commit to another repository, push a release, or make this
repository public without explicit authorization in that moment.
