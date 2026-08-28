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
rule is missing or contradicts the pinned governing records, name the exact
conflict in your response and ask the owner. Do not invent a parallel wire
shape.

## Governing records

Product and architecture decisions are snapshotted at a pinned upstream commit
under [`docs/upstream/showmesh/`](docs/upstream/showmesh/UPSTREAM.md). Follow
that directory's provenance and refresh rules.

- Read RES-018 for changes to the plugin runtime or release shape.
- Read `docs/build/FPP-PLUGIN-COORDINATOR-CONTRACTS.md` before touching the
  observation or definition wire shape. It is frozen and byte exact.
- Read ADR-043 and Track H for playlist-identity work.
- Treat accepted decisions as frozen implementation inputs. Do not silently
  replace a design. If evidence contradicts one, say so in your response and ask
  the owner. Do not stop the work, open a correction record, or start a review
  pass unless the owner asks for one.

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

The owner runs the real-hardware test. An agent's work ends at built, unit
tested, benched in a container where the bench covers it, and pushed.

The containerized `fppd` bench under `bench/` is yours. Run it, keep it working,
and trust its result. What is not yours is a real player, real fixtures, a real
SD card, and the deployed fleet. Never simulate those to manufacture evidence,
and never treat new test scaffolding as the deliverable when the assigned task
was a fix.

- Choose gates from the final diff. A prose-only change receives prose, link,
  and consistency checks, not unrelated builds or integration suites.
- For executable changes, run the focused tests plus `make check` when the diff
  can affect shipped binaries, native code, packaging, or generated output.
- Run the container bench when the diff can change what it covers. Write
  real-hardware behavior down as unobserved and hand it to the owner rather than
  reaching for it.
- Exercise named failure cases. A test that still passes after the behavior it
  claims to constrain is broken is not evidence.
- Inspect the final diff for secrets, generated artifacts, and accidental files.
- Distinguish observed behavior, inference, and unverified expectations. Use
  `should` for behavior that was not observed.

## Stay inside the assigned task

The assigned task is the whole deliverable. Fix what you were asked to fix.

- File at most one follow-up issue per task, and only when the thing you found
  breaks the work you were assigned. Everything else goes in your response as a
  sentence for the owner to rule on.
- Never fix an adjacent defect, refactor neighbouring code, or improve a nearby
  document because you were in the file.
- Never create a decision record, plan document, review pass, or replacement
  issue unless the owner asked for one.

## Code comments

Three lines of comment, maximum, unless the file documents a wire format or an
external system's exact behavior. Explain a non-obvious rule, safety boundary,
caller-visible guarantee, or external-system trap that names and types cannot
express. If the comment is longer than the code it describes, it is wrong.

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
