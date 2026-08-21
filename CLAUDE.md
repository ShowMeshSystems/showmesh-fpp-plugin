# showmesh-fpp-plugin Claude guidance

## Scope

[`AGENTS.md`](AGENTS.md) is this repository's canonical cross-agent contract.
Follow it in full. This file adds only Claude-specific operating guidance;
repository architecture, security, evidence, contribution, and publishing rules
remain authoritative.

Read [`README.md`](README.md) for the product and repository split. Use the
pinned records under [`docs/upstream/showmesh/`](docs/upstream/showmesh/UPSTREAM.md)
for implementation decisions instead of remembered state.

## Work proportionally

- Implement a small, settled change directly. Do not create a broad plan,
  subagent tree, architecture review, or replacement issue unless the work
  actually needs one.
- For multi-file, ambiguous, architectural, or risky work, inspect the affected
  code and governing records, then present a concrete file-level plan before
  editing.
- Treat accepted decisions as frozen. Raise one precise contradiction with
  citations when observed; do not repeatedly relitigate or silently work around
  it.
- Decide reversible implementation details and continue. Stop only for a real
  conflict, destructive action, missing authority, or inaccessible required
  evidence.
- After two materially similar failed attempts, change the approach or report
  the observed blocker. Do not loop.

## Evidence and delivery

Follow `AGENTS.md`'s proportional gates and publishing boundary. Never claim a
real-host, FPP-version, release, or runtime result that was not observed. For an
authorized implementation task, ordinary completion includes committing and
pushing the task branch; it does not include release publication or fleet
installation.

## Private maintainer overlays

A private overlay may add internal tracking, current priorities, owner workflow,
and personal preferences. It must live outside this repository, must not be
required for a contribution, and may not override `AGENTS.md`.

When asked to change a local override, follow `AGENTS.md` and the maintenance
file in the override's resolved canonical root. Preserve unrelated user
configuration and never copy private content into this repository.
