# Track H: Cue and Playlist Runtime

[Build plan](BUILD-PLAN.md) · [ADR-043](../decisions/ADR-043-show-scoped-cues-and-playlist-authority.md) · [Track B](TRACK-B-nodes-and-projection.md) · [Track C](TRACK-C-audio-node.md) · [Track F](TRACK-F-resting-mode.md) · [RES-018](../research/RES-018-fpp-brightness-control.md) · [SM-63 handoff](SM-63-FPP-PLUGIN-HANDOFF.md)

Status: H0 closed and H1 and H2 specified, 2026-08-22. Specified 2026-08-20 from ADR-043 and the owner-approved FPP-plus-ShowMesh-audio operating model. The FPP plugin foundation remains SM-63; the first real-host plugin gate remains SM-14.

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

#### Decisions closed 2026-08-22

ADR-043's four follow-up questions and the H0 seam list are closed here. These
are narrowing decisions inside an accepted ADR, not new architecture. Nothing
below permits a cross-show activation, and nothing below adds a scheduler.

##### H0.1 Identifiers are already reserved

`show.cue` and `show.playlist` were reserved in
[IDENTIFIER-REGISTER](IDENTIFIER-REGISTER.md) before this seam opened, together
with the rule that an FPP playlist name, index, filename, or imported hash
lives inside the `show.playlist` runner binding and never becomes a global
kind or a Cue id. H0 adds no further reservation.

##### H0.2 Held-output policy when FPP contradicts the active Show

**Decision: the policy is authored on the FPP-backed Playlist, it defaults to
`hold`, and `safeCue` must name a same-show Cue.**

`show.playlist.mismatchPolicy` takes one of three values:

| Value | Effect while the mismatch stands |
|---|---|
| `hold` (default) | No new Cue is activated. Every output already authorized by the last good activation keeps running unchanged. The renderer holds its current sequence, audio keeps its current asset, LTC keeps advancing from the Cue it was already following. |
| `blackAndSilence` | The renderer blacks its surfaces and ShowMesh-owned audio silences. LTC stops. FPP itself is untouched. |
| `safeCue` | The named same-show Cue is activated in place of the mismatched entry. `safeCueRef` is required, must belong to the same Show, and must pass readiness with the rest of the catalog. |

`hold` is the default because it is the only one of the three that cannot make
a running show worse than the observation that triggered it. A mismatch is
usually an authoring error found while something else is playing correctly;
blacking the wall converts an operator-visible binding problem into an
audience-visible failure. Both louder policies remain available because a
site that would rather go dark than show the wrong content must be able to say
so.

The policy applies to a contradicting FPP observation of any kind: an entry
bound to a non-active Show, an unknown entry key, an ambiguous binding, a
changed playlist hash, or an event-sequence regression. The state is
`mismatched` in every case and carries the observed evidence, so the three
policies differ only in what the outputs do, never in what the operator is
told.

The policy is resolved from the active Show's `fpp`-runner Playlist bound to
the reporting FPP instance. When no such Playlist exists, no ShowMesh output
was ever authorized by that instance, so there is nothing to hold and the
observation is recorded as unbound.

Cross-show fallback is not an option under any of the three, and no policy may
search another Show's namespace for a Cue, an asset, or a surface.

##### H0.3 LTC start offset stays on the Cue

**Decision: day 0 has exactly one LTC offset field and it lives on the Cue.**

This confirms ADR-043 decision 2 and its follow-up question 2 with no change.
No same-Cue/different-time-range requirement has been recorded, and a
Playlist-entry override added now would create two competing sources for one
value before anything reads either. When a concrete requirement appears, the
override is additive: an absent entry override means the Cue's offset, which
is the behavior shipped here.

SM-145 asks a different question, whether LTC restarts per playlist item or
continues across the playlist. The Cue offset is what answers it either way:
per-item restart is offset `0` on each Cue, and a continuous timeline is an
increasing offset per Cue. Track H ships the field and the arithmetic
(`Cue LTC start offset + current Cue position`); it does not decide the show's
authoring convention.

##### H0.4 Day-0 Cue outputs

**Decision: a Cue declares at most four outputs, and entry/exit Actions and
Macros are not among them.**

| Output | What it declares |
|---|---|
| `render` | The logical sequence whose target-specific FSEQ the participating render nodes select. |
| `audio` | The audience-audio asset and its playback policy. |
| `ltc` | The LTC start offset, emitted from the program-audio clock domain. |
| `announcement` | The duck, mix, or interrupt policy an announcement Cue applies to the active background-audio session. |

An `announcement` Cue is a Cue that declares the `announcement` output. It is
directly activatable and is not required to be a Playlist entry.

Entry and exit Actions and Macros are deliberately absent. ADR-043 makes them
conditional on "a later decision", and the decision they need is a failure
policy, not a field: a Macro that fails on Cue entry has to either hold the
Cue, run it anyway, or abort the entry, and each of those is a different
runtime contract. SM-48's open question about action idempotency is the same
gap seen from Track F. Adding the field before that policy exists would ship
an unspecified failure direction into the activation path.

##### H0.5 Resource claims

**Decision: claims are derived from a Cue's declared outputs, never authored
by hand, and exclusivity is checked at authoring, activation, and dispatch.**

| Claim | Derived from | Exclusivity |
|---|---|---|
| `program-audio-route:<node>:<route>` | `audio` output | Exclusive. One Cue owns the program route. |
| `render-surface:<surfaceId>` | `render` output, expanded through the Show's surfaces | Exclusive per surface. |
| `ltc-output:<node>:<route>` | `ltc` output | Exclusive. |
| `announcement-session:<node>` | `announcement` output | Exclusive. One announcement at a time. |

An announcement Cue claims only `announcement-session`. It does not claim
`program-audio-route`, because its declared duck, mix, or interrupt policy is
a relationship with whoever holds that route rather than a seizure of it. That
is what lets an announcement play over background music without either Cue
being refused.

**An announcement Cue still declares `audio`, and that is not a contradiction.**
The `audio` output says what the announcement plays; the `announcement` output
says it plays through the announcement session rather than the program route.
So the rule the claim derivation implements is: a Cue claims
`program-audio-route` when it declares `audio` and does not declare
`announcement`. Reading the claim off `audio` alone would make every
announcement collide with the background music it is supposed to duck, which
is the one case this table exists to permit.

FPP's own lighting and sequence playback is not a ShowMesh claim. This is the
whole point of the model: FPP running a resting sequence while a
`showmesh-audio` Playlist runs background music is two independent authorities
with a disjoint claim set, so it is permitted by construction rather than by
exception.

A refused claim names both Cues and the exact claim string. Nothing is
silently preempted, and a claim conflict is a readiness failure before
showtime rather than a runtime surprise.

##### H0.6 Where these decisions are enforced

Recorded here so H1 through H5 do not each re-derive the boundary:

- **Authoring** rejects cross-show references, a `safeCue` outside the Show,
  and two same-show Cues that a single Playlist could make concurrently active
  with a colliding exclusive claim.
- **Readiness** rejects stale imports, missing assets, a node without the
  authorized catalog revision, and conflicting claims across the Playlists a
  Show can run concurrently.
- **Activation** rejects an inactive Show, a stale active-show generation, an
  unknown or ambiguous entry key, and a sequence regression.
- **Dispatch and node execution** reject the same set again against the
  catalog the node actually holds. Asset presence grants no authority.

##### H0.7 Switching the active Show

**Decision: a switch revokes the previous Show's authority immediately.
ShowMesh-owned audio stops, rendering holds but is reported as superseded,
and nothing restores the previous Show's output after a restart.**

This is a different situation from H0.2's mismatch and takes a different
answer. A mismatch is one contradicting observation inside a Show that is
still authorized, and reconciling it is a live possibility. A Show switch is
an operator deciding that the previous Show may no longer affect anything, so
there is nothing to hold open for.

| Output | On switch |
|---|---|
| ShowMesh-owned auxiliary audio and announcements | Stop, using the configured default fade. Their content belongs to a Show that is no longer authorized. |
| LTC | Stops with the audio it was emitted from. |
| Rendering | Holds its current output, reported as `superseded` with the Show and generation that authorized it. It is never reported as current or healthy. |
| FPP | Untouched. FPP is not ShowMesh's to stop, and ADR-001 as narrowed by ADR-043 leaves its playback where it is. |

Rendering holds rather than blacking for H0.2's reason: an operator switching
Shows during setup should not see the wall go dark as a side effect of an
authoring action. Holding is also not an activation. The node continues
something that was authorized when it started and says so honestly; it does
not select anything new.

**A held output is never restored.** A node that restarts, or whose render
pipeline restarts, comes up cleared rather than re-applying a persisted
assignment whose authorization tuple no longer matches the active Show and
generation. This is the case that would otherwise bite hardest, because the
render agent already resumes its last persisted assignment at boot: without
this rule, rebooting a node in November would put Halloween back on the wall.

The newly active Show's Cues cannot run until its catalog is deployed and
acknowledged, which is H3. Asset presence on the node grants nothing.


### H1. Versioned Cue and Playlist configuration

Specified in detail in [TRACK-H-H1-SPEC](TRACK-H-H1-SPEC.md).

Add `show.cue` and `show.playlist` through the store, public API, OpenAPI, `showmeshctl`, export/import, audit, revision history, and copy guards before adding a UI.

A Cue carries a required Show reference, stable id, readiness metadata, and typed output declarations. A Playlist carries a required Show reference, a runner, and ordered entries. Each entry has its own stable id, references a same-show Cue, and may carry only runner-specific binding and entry transition policy.

Validation refuses cross-show references, duplicate entry ids, missing Cues, unsupported runner fields, conflicting resource claims, invalid LTC offsets, and a Cue output the selected nodes cannot execute. Updating a Cue or Playlist creates a new revision; an active run pins exact Cue and Playlist revisions.

### H2. FPP import, reconciliation, and entry identity

Specified in detail in [TRACK-H-H2-SPEC](TRACK-H-H2-SPEC.md). Its plugin-facing half is frozen as section 3 of [FPP plugin coordinator contracts](FPP-PLUGIN-COORDINATOR-CONTRACTS.md), so the plugin repository can build against it without reading this track.

Provide an authoring flow that reads an FPP playlist definition, stores its canonical revision, and maps each entry to a Cue. Import never makes a filename the Cue identity.

The SM-63 native component publishes the versioned atomic identity event defined by RES-018 and frozen byte for byte in [FPP plugin coordinator contracts](FPP-PLUGIN-COORDINATOR-CONTRACTS.md). Ingestion of that event, its authorization scope, its refusal vocabulary, and its storage landed under SM-150; H2 consumes the stored latest observation rather than redefining the wire shape. Track H ingests it and resolves the deterministic entry key from FPP instance UUID, playlist name, canonical playlist hash, section, and position. Expected sequence and media filenames are validation evidence.

Reconciliation refuses a changed playlist hash, an unknown entry, ambiguous duplicates, an event-sequence regression, or an inactive-show binding. A playlist edit becomes visible in readiness before showtime and becomes an explicit mismatch at runtime until the operator reconciles it.

Independent FPP MQTT topics remain corroboration and health evidence. They are never assembled into a synthetic atomic transition.

### H3. Active-show generation and resolved Cue catalogs

Specified in detail in [TRACK-H-H3-SPEC](TRACK-H-H3-SPEC.md).

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

**SM-285's status against this list (2026-08-26).** Two of these seven were
already built (stale imports, and cross-show/unresolved references folded
into `cue-not-ready`). SM-285 built two more, both by reusing an existing
resolver rather than deriving a second one: `node-catalog-stale` (SM-282's
own cue-catalog acknowledgement resolution, `internal/coordinator/api/
cuecatalog.go`, extracted into `internal/coordinator/fppreconcile.
NodeCatalogAckStatus` so the read route and this readiness condition
compute the identical answer) and `exclusive-claim-conflict`
(`assetsync.ResolveCueCatalog`'s own `Catalog.Conflicts`, the same
computation the cue-catalog deploy path already refuses a deploy on).
`assets-missing` builds on `assetsync.ExpectedAssetsForNode` once SM-287's
concurrent narrowing of that function lands on `main`.

**`output-policy-unsupported` and `plugin-capability-ungated` are
explicitly out of scope for this season**, not half-built. Neither has any
representation in this codebase — no field, no validation rule, no route —
and H0.6 ("Where these decisions are enforced," above) never assigns
either one to Authoring, Readiness, Activation, or Dispatch: only this
section's own aspirational list and the track's acceptance criterion 7
name them. Building either honestly first requires deciding WHERE it is
enforced (H0.6's own boundary), which is design work this Lane's readiness
seam did not scope and is not itself a readiness-shaped change. See
this file's own "Out of scope" list below.
### H7. Integrated and failure verification

The bench that runs this path, and the record of what a first assembled run
actually showed, are [bench/track-h-chain](../../bench/track-h-chain/README.md)
and [TRACK-H-CHAIN](../bench/TRACK-H-CHAIN.md).

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
- **`output-policy-unsupported` as a readiness condition (SM-285).** There
  is no stored rule anywhere naming which output policies a given node or
  runner combination supports, so there is nothing for readiness to check
  yet; deciding that rule is H0.6-shaped design work, not a readiness-seam
  change. Building this later starts by deciding H0.6's enforcement point
  for it (Authoring, most likely, alongside its sibling authoring
  refusals), then adding a readiness condition only if runtime state can
  still diverge from that authoring-time check.
- **`plugin-capability-ungated` as a readiness condition (SM-285).** The
  SM-63 native FPP plugin capability and its compatibility gate have no
  representation in this codebase at all — no stored capability record, no
  gate result, no route. Building this starts with that plugin-side
  representation (RES-015/RES-018's own territory), which readiness would
  then read; it cannot be built from the readiness side alone.
