# Track H: Cue and Playlist Runtime

[Build plan](BUILD-PLAN.md) · [ADR-043](../decisions/ADR-043-show-scoped-cues-and-playlist-authority.md) · [Track B](TRACK-B-nodes-and-projection.md) · [Track C](TRACK-C-audio-node.md) · [Track F](TRACK-F-resting-mode.md) · [RES-018](../research/RES-018-fpp-brightness-control.md) · [SM-63 handoff](SM-63-FPP-PLUGIN-HANDOFF.md)

Status: not started. Specified 2026-08-20 from ADR-043 and the owner-approved FPP-plus-ShowMesh-audio operating model. The FPP plugin foundation remains SM-63; the first real-host plugin gate remains SM-14.

## Goal

Make a Cue the show-scoped unit of synchronized playback and make Playlist authority explicit. FPP-backed Playlists follow the entry FPP is actually playing. ShowMesh-owned auxiliary-audio Playlists may run concurrently while FPP runs lighting or a resting sequence. Every activation remains authorized by the active Show, so content from one seasonal show cannot execute merely because FPP played a matching filename.

This track delivers the common model and runtime contract shared by rendering, audience audio, LTC, announcements, and future playlist runners. It does not turn ShowMesh into a calendar scheduler and does not build the future general primary-show runner.

## Why this is a track

The current implementation has three independent models that do not join:

- FPP owns playlist order and exposes runtime playlist and sequence observations.
- The render agent pins one FSEQ for a session rather than switching per FPP entry.
- The audio engine accepts a caller-owned `PlaylistRef` and can advance it independently.

Fixing only one consumer would leave the others inferring different playback items. Adding an LTC field to the audio item would leave FPP identity, renderer switching, active-show isolation, authoring, readiness, and announcements unresolved. Track H instead establishes one activation contract and then makes every consumer prove that it follows that contract.

## Authority and dependencies

- ADR-043 governs the model. A Show owns separate Playlists, Cues, Macros, Actions, Surfaces, and assets. A Playlist entry references a same-show Cue.
- FPP remains the day-0 scheduler and owns order, progression, and playhead for a Playlist whose runner is `fpp`.
- `showmesh-audio` owns order and progression only for auxiliary-audio Playlists whose media does not live on FPP.
- SM-63 supplies the resident FPP 9/10 native component and its atomic playlist-entry identity event. Track H consumes that event; it does not redesign the plugin runtime.
- Track E supplies Show, asset, revision, and active-show configuration foundations.
- Track B supplies render-agent catalog deployment and FSEQ switching.
- Track C supplies local audio playback, program/LTC clock ownership, announcements, and mix policy.
- Track D consumes LTC and provides same-show Actions for Resolume when a Cue explicitly declares them.
- Track F consumes the resulting Playlists and Cues for the night-session lifecycle. It does not embed a competing playlist model.

These dependencies do not block H0 and H1. H2 may build against a recorded plugin event contract before SM-63 produces a binary. H3 through H5 can build against fake runners and agents. They prevent an integrated completion claim until their real consumers are present.

## Seams

### H0. Close implementation decisions and reserve identifiers

- Reserve `show.cue` and `show.playlist` before parallel implementation begins.
- Decide the held-output policy for an FPP observation that contradicts the active Show. The policy may freeze, black and silence, or select an explicitly configured safe Cue; cross-show fallback is forbidden.
- Keep LTC start offset on the Cue for day 0. Add a Playlist-entry override only after a concrete same-Cue/different-time-range requirement is recorded.
- Limit day-0 Cue outputs to render sequence, audience audio, LTC offset, and announcement mix policy. Entry/exit Actions and Macros require a later explicit relationship and failure policy.
- Define resource claims for program-audio routes, announcement sessions, render surfaces, and LTC outputs. The known FPP-plus-background-audio case is permitted; two active Cues may not own the same exclusive resource.
- Record every owner decision in the Track document or a narrowing ADR before dependent code starts.

### H1. Versioned Cue and Playlist configuration

Add `show.cue` and `show.playlist` through the store, public API, OpenAPI, `showmeshctl`, export/import, audit, revision history, and copy guards before adding a UI.

A Cue carries a required Show reference, stable id, readiness metadata, and typed output declarations. A Playlist carries a required Show reference, a runner, and ordered entries. Each entry has its own stable id, references a same-show Cue, and may carry only runner-specific binding and entry transition policy.

Validation refuses cross-show references, duplicate entry ids, missing Cues, unsupported runner fields, conflicting resource claims, invalid LTC offsets, and a Cue output the selected nodes cannot execute. Updating a Cue or Playlist creates a new revision; an active run pins exact Cue and Playlist revisions.

### H2. FPP import, reconciliation, and entry identity

Provide an authoring flow that reads an FPP playlist definition, stores its canonical revision, and maps each entry to a Cue. Import never makes a filename the Cue identity.

The SM-63 native component publishes the versioned atomic identity event defined by RES-018. Track H ingests it and resolves the deterministic entry key from FPP instance UUID, playlist name, canonical playlist hash, section, and position. Expected sequence and media filenames are validation evidence.

Reconciliation refuses a changed playlist hash, an unknown entry, ambiguous duplicates, an event-sequence regression, or an inactive-show binding. A playlist edit becomes visible in readiness before showtime and becomes an explicit mismatch at runtime until the operator reconciles it.

Independent FPP MQTT topics remain corroboration and health evidence. They are never assembled into a synthetic atomic transition.

### H3. Active-show generation and resolved Cue catalogs

Define an active-show generation that changes whenever `show.active` changes or its authorization is deliberately reissued. Every Cue activation and node assignment carries the Show id, active-show generation, Playlist revision, entry id, Cue id, and Cue revision that authorized it.

The coordinator resolves the active Show's required Cue catalog and deploys it to participating nodes before playback. Nodes acknowledge the exact catalog revision and required assets. Asset presence alone grants no execution authority.

A node rejects unknown, stale, ambiguous, or cross-show activations even when the filename or content hash exists locally. Switching the active Show invalidates the prior catalog and stops or clears the prior Show's auxiliary-audio authority under the H0 transition policy.

### H4. Common activation for render, audio, and LTC

Define one runner-neutral Cue activation envelope. It carries runner identity, activation identity, full state, authoritative position, evidence time, and the pinned Show/Playlist/entry/Cue identities from H3.

- The renderer selects the Cue's resolved target-specific FSEQ instead of retaining the session's original FSEQ.
- The audio engine selects the Cue's resolved local audio asset and aligns it to the runner's position.
- LTC emits `Cue LTC start offset + current Cue position` from the program-audio clock domain.
- Natural completion and next-entry selection remain the configured runner's authority.

The coordinator is not inserted into the frame-rate timing path. For FPP-backed playback, agents use MultiSync position with the already authorized catalog while the plugin event establishes entry identity and revision evidence.

### H5. ShowMesh audio runner, announcements, and concurrency

Implement the narrow `showmesh-audio` runner for background music, preshow beds, and other auxiliary audio absent from FPP. It owns its Playlist order, repeat behavior, progression, and audio playhead. It emits no LTC unless a Cue explicitly declares LTC.

Announcements are directly activatable same-show Cues. They apply a declared duck, mix, or interrupt policy to the active background session. They do not stop or alter FPP, rendering, or LTC unless the Cue explicitly declares that output or invokes a same-show Action under a later approved relationship.

Resource arbitration must be deterministic and observable. A refused concurrent claim names both owners and the resource; it never silently steals an audio route, surface, or LTC output.

### H6. Operator UI and readiness

Add Show-scoped Playlist and Cue authoring after API and CLI parity exists. The UI must expose:

- runner and authority;
- ordered entries and their referenced Cues;
- FPP import source, canonical hash, and reconciliation status;
- Cue outputs, LTC offset, asset readiness, and resource claims;
- active Playlist, entry, Cue, runner evidence, and mismatch state;
- auxiliary-audio and announcement state;
- explicit reasons an activation or concurrent claim was refused.

Readiness refuses stale FPP imports, unresolved or cross-show references, missing assets, nodes without the authorized catalog revision, unsupported output policy, conflicting exclusive claims, and a required SM-63 plugin capability that has not passed its compatibility gate.

### H7. Integrated and failure verification

Prove the complete path with running binaries, not only unit tests:

- FPP advances through two entries and rendering, audio, and LTC all select the second Cue exactly once.
- Duplicate sequence filenames at different positions resolve to different entry identities without guessing.
- Editing or reordering the FPP playlist changes the canonical hash and holds the old binding.
- Christmas active plus a Halloween FPP event produces an operator-visible mismatch and executes no Halloween output on coordinator or nodes.
- Coordinator or broker loss after catalog deployment does not invalidate authorized node-local position following, and recovery does not replay a stale Cue over a newer one.
- Switching active Show invalidates prior auxiliary audio and catalog generations.
- An FPP-backed Playlist and a background `showmesh-audio` Playlist run concurrently without competing for the same authority.
- Announcements exercise duck, mix, and interrupt policies while FPP continues its current sequence.
- Unknown Cue, stale catalog, sequence regression, missing asset, and resource conflict each fail visibly in the documented safe direction.

Real FPP, audio hardware, and Resolume gates remain explicit where the development environment cannot supply them. A fake or container closes software behavior only; it does not become hardware evidence through repetition.

## Safety and failure direction

- Show scoping is checked at authoring, activation, dispatch, and execution.
- Runtime matching never searches another Show namespace.
- A changed FPP playlist revision invalidates rather than silently remaps bindings.
- A node never continues an old Cue while reporting the requested Cue healthy.
- A missing or stale authority observation becomes `unknown`, `held`, or `mismatched`; it never becomes success.
- Queue pressure, event loss, and event-sequence gaps are observable and bounded.
- Two runners never own one Playlist run.

## Acceptance criteria

1. Cue and Playlist configuration has API, CLI, UI, revision, audit, export/import, and validation parity.
2. FPP import and the SM-63 event resolve duplicate filenames by deterministic entry identity and reject stale playlist revisions.
3. Every activation carries and enforces active-show generation through the coordinator and nodes.
4. Renderer, audio, and LTC consume the same pinned Cue activation.
5. `showmesh-audio` can run background audio while FPP runs a resting sequence.
6. Announcements apply their declared audio policy without implicitly altering FPP.
7. Readiness proves imports, assets, catalogs, capabilities, and resource claims before showtime.
8. Cross-show and stale-generation attempts are rejected at both coordinator and node boundaries.
9. The H7 running-binary matrix passes, with hardware-only cases left honestly open until observed.

## Bound by

- [ADR-001](../decisions/ADR-001-fpp-is-authoritative.md), as narrowed by ADR-043
- [ADR-017](../decisions/ADR-017-showmesh-owns-audience-audio.md)
- [ADR-018](../decisions/ADR-018-program-and-ltc-share-a-clock-domain.md)
- [ADR-024](../decisions/ADR-024-identity-authorization-and-audit.md)
- [ADR-027](../decisions/ADR-027-show-and-surface-model.md)
- [ADR-028](../decisions/ADR-028-show-asset-store-and-identity.md)
- [ADR-030](../decisions/ADR-030-operator-ui-is-the-authoring-surface.md)
- [ADR-043](../decisions/ADR-043-show-scoped-cues-and-playlist-authority.md)
- [RES-015](../research/RES-015-fpp-plugin-distribution-model.md)
- [RES-018](../research/RES-018-fpp-brightness-control.md)

## Out of scope

- A general ShowMesh primary-show Playlist runner.
- A ShowMesh calendar scheduler.
- Replacing FPP as the day-0 lighting or schedule authority.
- Cross-show Cue, Playlist, Action, Macro, Surface, or asset fallback.
- Persisting random UUIDs inside FPP playlist entries before FPP editor/API survival is proven.
- Treating the Audio Engine's internal `PlaylistRef` as the show-level authoring model.
