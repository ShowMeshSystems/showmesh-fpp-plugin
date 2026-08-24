# Upstream snapshot: ShowMesh governing records

This directory holds read-only copies of the ShowMesh records that govern this
repository's implementation. They are evidence, not working documents.

## Source

| Field | Value |
|---|---|
| Source repository | https://github.com/ShowMeshSystems/showmesh |
| Source commit | `50e42fa2f91225b585dda6bc3a43f9f97d29c77f` |
| Commit date | 2026-08-20 |
| Snapshot date | 2026-08-21 |
| Snapshot taken by | Repository bootstrap, refreshed during the Go helper extraction |

### Records pinned to a different upstream commit

One record is not yet on upstream `main` and is pinned to the commit that
carries it. The table above still governs every other file here.

| Upstream path | Pinned commit | Commit date | Note |
|---|---|---|---|
| `docs/build/FPP-PLUGIN-COORDINATOR-CONTRACTS.md` | `0f39b1762fe7d80a18d0112826dc0ca117bde2ea` | 2026-08-23 | On the upstream branch that carries the 2026-08-23 owner-ruled correction to sections 2.2, 2.3, and 3.1, open as a pull request. Re-pin to the merge commit when it lands on `main`. |

## Why copies rather than links

A link follows `main` and drifts. These records carry frozen decisions that this
repository implements literally, including the RES-018 section 6 wire contract
and ADR-043's authority model. A reader who follows a link to `main` may read a
record that changed after this repository was built against it, and the
divergence would be silent. A pinned copy makes the divergence a diff.

## Rules

1. **Never edit a file under this directory to change its meaning.** These are
   snapshots of another repository's records. If a record is wrong, fix it
   upstream in `showmesh` and refresh the snapshot.
2. **A refresh is a deliberate commit.** Re-copy from a named upstream commit,
   update the table above, and describe in the commit message what changed and
   what it means for this repository's code. Never refresh as a side effect of
   unrelated work.
3. **Snapshots do not override upstream.** Where a snapshot and current upstream
   `main` disagree, upstream is the current truth and this snapshot is the
   record of what this code was built against. Reconcile deliberately.
4. **Do not add records here speculatively.** This directory carries exactly the
   records named as governing in the plugin implementation handoff. Adding more
   makes the refresh burden grow without making any decision clearer.

## Copied paths

Each file preserves its upstream path beneath this directory, so relative links
inside the documents remain intelligible.

| Upstream path | Why this repository needs it |
|---|---|
| `SECURITY.md` | Vulnerability reporting and credential-handling posture. |
| `docs/research/RES-015-fpp-plugin-distribution-model.md` | Plugin distribution and install model, including the script-command registration this helper depends on. |
| `docs/research/RES-018-fpp-brightness-control.md` | The governing record for this repository: supported FPP versions, the three-repository split, the install and release contract, the Go client seam, and the section 6 playlist-entry observation contract. |
| `docs/decisions/ADR-010-apache-2-license.md` | Apache-2.0 is decided, and it constrains what upstream GPL code may be read versus copied. |
| `docs/decisions/ADR-013-no-fpp-control-port-sharing.md` | The absolute rule against binding a second UDP 32320 listener alongside a running `fppd`. |
| `docs/decisions/ADR-024-identity-authorization-and-audit.md` | Authentication, the `fpp:observe` scope, and the rule that an authorization refusal is not a coordinator outage. |
| `docs/decisions/ADR-038-fpp-authorizes-night-sessions.md` | FPP owns the calendar and authorizes the operating window. |
| `docs/decisions/ADR-043-show-scoped-cues-and-playlist-authority.md` | Show-scoped Cues and explicit playlist authority; the model the identity publisher feeds. |
| `docs/architecture/RESTING-MODE.md` | The night-session lifecycle this plugin's brightness and observation work serves. |
| `docs/build/STEP-9-SPEC.md` | The macro execution surface the Go helper calls. |
| `docs/build/FPP-PLUGIN-COORDINATOR-CONTRACTS.md` | The frozen wire contracts this repository implements byte for byte: playlist-entry observation ingestion, the brightness transition gain, and playlist definition publication. |
| `docs/build/TRACK-H-cues-and-playlists.md` | The consumer of the atomic playlist-entry identity event. |
