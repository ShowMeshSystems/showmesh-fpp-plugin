# Repository agent contract

These instructions apply to every agent and model working in this repository.
Tool-specific instruction files may add operating guidance, but they do not
replace this contract.

## Repository scope

This repository owns the ShowMesh runtime that lives on an FPP host: the forked
Go macro helper plus the resident C++ brightness and playlist-identity
component. It supports FPP 9.4-9.x and FPP 10.x.

- Coordinator surfaces and the API contract live in `ShowMeshSystems/showmesh`.
- Plugin Manager packaging lives in `ShowMeshSystems/fpp-showmesh`.
- The remaining FPP-host runtime, tests, and release assets live here.

Never implement coordinator-side work in this repository. If a coordinator
contract is missing or contradicts the pinned governing records, report the
exact conflict and stop the affected work. Do not invent a parallel wire shape.

## Governing records

Product and architecture decisions are snapshotted at a pinned upstream commit
under [`docs/upstream/showmesh/`](docs/upstream/showmesh/UPSTREAM.md). Follow
that directory's provenance and refresh rules.

- Read RES-018 for changes to the plugin runtime or release shape.
- Read `docs/build/FPP-PLUGIN-COORDINATOR-CONTRACTS.md` before touching the
  observation or definition wire shape. It is frozen and byte exact.
- Read ADR-043 and Track H for playlist-identity work.
- Treat accepted decisions as frozen implementation inputs. New observed
  evidence may justify an upstream correction; it does not justify silently
  replacing the design in this repository.

## Frozen implementation rules

- **Apache-2.0.** Upstream GPL code, including
  `FalconChristmas/fpp-brightness`, is behavioral and source evidence only. Do
  not copy or link it without a separate compatibility review.
- **Pin supported FPP versions in CI evidence.** Cover FPP 9.4-9.x and the
  latest published FPP 10 beta selected for the build. Never float silently
  between host versions.
- **Keep the Go helper as a forked command helper.** The resident C++ component
  uses isolated FPP 9 (libhttpserver) and FPP 10 (Drogon) adapters around
  host-neutral cores. It does not replace the script command.
- **Use `playlistCallback` for playlist identity.** Never use `eventCallback` or
  `MultiSyncPlugin` for that contract.
- **Keep callback work bounded.** The callback copies evidence and returns. No
  HTTP, retry, hashing, playlist fetch, persistence, or sleep runs on FPP's
  callback thread.
- **Implement RES-018 section 6 exactly.** The resident worker owns
  authenticated HTTP, `fpp:observe`, RFC 8785 playlist hashing, deterministic
  entry keys, a persistent monotonic sequence, bounded newest-state coalescing,
  and explicit gap evidence.
- **Never bind a second UDP 32320 listener.** Follow ADR-013.
- **Preserve the Go helper's handwritten HTTP behavior and 2xx-only
  prior-failure flush.** Prior failures ride in the run request and clear only
  after a successful response.
- **Never expose a credential through FPP commands.** A credential is not a
  command argument, URL query parameter, child-process environment variable,
  public MQTT payload, or world-readable configuration value.
- **Keep candidate artifacts private.** Do not publish a release or install it
  on the real fleet before the recorded real-host gate passes and the owner
  authorizes publication.

## Evidence and verification

Passing unit tests is not completion. A change closes when its stated behavior
is observed at the applicable boundary.

- Choose gates from the final diff. A prose-only change receives prose, link,
  and consistency checks, not unrelated builds or integration suites.
- For executable changes, run the focused tests plus `make check` when the diff
  can affect shipped binaries, native code, packaging, or generated output.
- Run version-specific or real-host gates only when the changed behavior or
  acceptance criteria require them. Keep unavailable hardware evidence open.
- Exercise named failure cases. A test that still passes after the behavior it
  claims to constrain is broken is not evidence.
- Inspect the final diff for secrets, generated artifacts, and accidental files.
- Distinguish observed behavior, inference, and unverified expectations. Use
  `should` for behavior that was not observed.

## Code comments

Write the minimum needed to explain a non-obvious invariant, safety boundary,
caller-visible contract, or external-system trap that names and types cannot
express.

- Do not preserve narrative history, design essays, review findings, or a
  restatement of the next statement in code comments.
- Never put a private tracker identifier or URL in source, tests, published
  contracts, commit messages, or pull requests. A necessary TODO may cite a
  public GitHub issue.
- Put durable design rationale in the upstream decision record, measurements in
  research, and change history in commits and pull requests.

## Public work and delivery

GitHub issues and pull requests are the contributor surface. Maintainers may
triage work privately, but a public contribution must not require access to a
private tracker or local overlay.

- Work on a task branch and preserve unrelated changes.
- Commit and push coherent completed work after the applicable gates pass.
- Push before CI; CI evaluates a published commit and is not permission to
  publish the branch.
- Merge only when the task includes merge authority and GitHub's actual required
  gates permit it. Prefer auto-merge over waiting when only required CI remains.
- Never force-push, rewrite shared history, publish a release, make the
  repository public, or install on the real fleet without exact authority.

## Local agent overrides

A user may keep private or personal agent guidance outside the repository. A
local override may supplement workflow, but it may not override this contract
or become a requirement for contributors.

When asked to install, update, repair, or remove one:

1. Resolve the active loader and canonical external source. Do not edit an
   injected copy, cache, stale draft, or file inside this repository.
2. Read its maintenance contract. Back up only affected files and preserve
   unrelated hooks, settings, plugins, and global instructions.
3. Keep reusable mechanics generic and instance details private.
4. Verify both a matching and non-matching repository, then read back the live
   configuration and permissions.
5. Never place credentials in prompt-visible guidance or copy private material
   into the repository to bypass a local permission boundary.

## Voice

Lead with the outcome and include only the evidence needed to review it. Do not
reopen settled design during implementation or turn small changes into broad
planning exercises. Keep README prose complete but edited for repetition. Use
precise test names and failure messages. Do not use em dashes.
