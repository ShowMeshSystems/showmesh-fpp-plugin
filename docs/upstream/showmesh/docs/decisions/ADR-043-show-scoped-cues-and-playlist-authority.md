# ADR-043: Show-Scoped Cues and Pluggable Playlist Authority

Status: Accepted (owner, 2026-08-20)
Date: 2026-08-20

## Context

ShowMesh already treats a Show as a namespace for the configuration and assets used to operate one authored show. Surfaces, actions, macros, and assets carry a show reference, while `show.active` identifies the one show allowed to affect the running system. This exists so Christmas can be prepared while Halloween is active without an edit or command crossing between them.

The playback model has not reached the same level of clarity.

For an FPP-backed show, FPP owns schedules, playlist order, and the current playlist entry. ShowMesh can observe the active FPP playlist name, playlist index, sequence filename, media filename, and playhead position. MultiSync supplies the filename and frame-accurate position used by render nodes. REST and MQTT supply playlist metadata and health.

ShowMesh does not currently join those observations to a show-owned playback item. Rendering pins one manually selected FSEQ for the whole session. Audio has a `PlaylistRef`, but that object is a caller-supplied ordered list of audio assets that the audio agent advances itself. It contains no FPP playlist identity, FPP entry identity, sequence filename, or per-entry LTC offset. It is therefore both too narrow—it describes only audio—and too authoritative—it advances independently of FPP—for the main show-program path.

The immediate requirement is to bind each FPP playlist entry to the synchronized content ShowMesh must run: render assets, audience audio, and a distinct LTC start offset. The broader requirement is to avoid making FPP a permanent structural dependency. A future ShowMesh installation may use ShowMesh itself as the playlist runner, either because FPP is absent or because ShowMesh offers a better playlist system. The content model must survive that change without redefining every playback item.

There is also a hard isolation requirement. A Halloween FPP sequence must not activate Halloween audio, video, LTC, actions, or macros while Christmas is the active ShowMesh show, even if the runtime filename matches a known Halloween binding and even if the required assets are already present on a node.

## Decision

### 1. A Cue is the show-scoped unit of synchronized playback

ShowMesh will introduce a first-class `show.cue` configuration object.

A Cue describes one logical item that can be activated as part of a show. It carries a required `show` reference and a stable cue identifier. It may resolve the synchronized outputs and behavior needed for that item, including:

- the logical sequence whose target-specific render assets must be selected;
- the audience-audio asset and playback policy;
- the LTC start offset for that item;
- optional show actions or macros associated with cue entry or exit, when those relationships are defined by a later decision;
- readiness requirements and operator-facing metadata.

A Cue does not contain an FPP playlist index, FPP playlist name, or any other runner-specific trigger. Those fields describe how a particular playlist runner selects the Cue, not what the Cue is.

This separates stable show intent from integration details. The same Cue may appear in more than one playlist, and a playlist may change runners without changing the Cue's content identity.

### 2. Shows may own Playlists, Macros, and Cues as separate configuration objects

ShowMesh will introduce a first-class `show.playlist` configuration object. Like `show.cue`, `show.macro`, and `show.action`, every Playlist carries a required `show` reference and exists as its own revisioned configuration object rather than being nested inside the Show payload.

The relationship is:

```text
Show
├── Playlists — ordered playback programs
│   └── entries reference Cues
├── Cues — synchronized playback items
├── Macros — ordered operational steps
│   └── steps reference Actions
├── Actions — integration-specific commands
└── Surfaces and assets — physical mapping and content
```

A Playlist contains ordered entries. Every entry has its own stable entry identifier and references a Cue belonging to the same show. An entry may also carry runner-specific binding data and entry-level transition policy.

The Cue owns the per-item LTC start offset because LTC is part of the synchronized content identity. A later requirement to run the same Cue at different timecode ranges in different playlists would justify an explicit per-entry override; absence of that requirement does not justify introducing two competing offset fields now.

### 3. Playlist authority is explicit and pluggable

Every Playlist declares a runner. The runner owns selection of the current entry, progression, repeat behavior, and the authoritative playhead for that playlist run.

The runner contract is integration-neutral. It must produce a common cue-activation model containing at least:

- show identifier and active-show revision or generation;
- playlist identifier and revision;
- playlist-entry identifier;
- cue identifier and cue revision;
- runner identity;
- playback state and current position;
- evidence time and activation identity.

The initial implemented runners are `fpp` and `showmesh-audio`.

For an FPP-backed playlist:

- FPP remains authoritative for schedule start, playlist order, entry selection, and progression.
- ShowMesh imports or snapshots the FPP playlist definition and binds each FPP entry to a ShowMesh Cue.
- MultiSync filename and position provide the low-latency runtime edge.
- FPP REST or MQTT playlist name and index corroborate which imported entry is active.
- ShowMesh does not independently advance the FPP-backed playlist.

The `showmesh-audio` runner is required for day-one auxiliary audio that does not exist on FPP: background-music playlists, preshow audio, and similar show-scoped beds. ShowMesh owns those playlists' selection, progression, repeat behavior, and audio playhead even while FPP independently runs a resting or lighting sequence. These playlists do not emit LTC unless a Cue explicitly declares it.

Announcements are directly activatable show-scoped Cues by default, not necessarily Playlist entries. ShowMesh owns their audio and applies the Cue's configured duck, mix, or interrupt policy to the active background-audio session. FPP continues its resting or show sequence unless the announcement explicitly invokes a same-show FPP Action or Macro.

The model also reserves a future general `showmesh` runner. That runner may later own primary-show playlist order and progression across lighting, rendering, audio, and other outputs without changing Cue identity or the downstream activation contract. This ADR does not implement that general runner and does not decide whether ShowMesh will also become a calendar scheduler. Playlist-running authority and calendar-scheduling authority are separate concerns.

### 4. ADR-001 is narrowed to FPP-backed playlists

ADR-001 remains correct for existing FPP shows but is no longer a universal constraint on every future ShowMesh deployment.

FPP is authoritative when a Playlist's runner is `fpp`, and FPP remains the day-0 calendar and schedule authority. ShowMesh is authoritative for auxiliary-audio progression when a Playlist's runner is `showmesh-audio`. A future Playlist whose runner is the general `showmesh` runner may make ShowMesh authoritative for primary-show order and progression. Introducing a ShowMesh calendar scheduler would require a separate decision because it creates a broader split-brain risk than playlist execution alone.

This record therefore narrows, rather than discards, ADR-001: authority belongs to the configured runner, and two runners may never be authoritative for the same playlist run.

### 5. FPP binding belongs to the Playlist entry

An FPP-backed Playlist entry records the imported identity needed to match FPP runtime evidence to its Cue. The expected shape includes:

- FPP instance;
- FPP playlist name;
- playlist section, when applicable;
- entry index;
- expected sequence filename;
- expected media filename;
- imported playlist revision or content-derived hash;
- the referenced ShowMesh Cue.

FPP does not currently expose a durable UUID for each playlist entry. Playlist name plus index is positional, while filename alone is ambiguous when the same file appears more than once. ShowMesh will therefore treat the imported playlist revision and expected filenames as validation evidence, not as globally stable Cue identity.

Until the FPP integration supplies a stronger entry token, an FPP-backed playlist must either reject ambiguous duplicate bindings or require an explicit operator-visible disambiguation rule. It must never guess between two Cues that could match the same runtime evidence.

The ShowMesh FPP integration will therefore define and publish one atomic entry-identity message for every playlist transition. It will include at least:

- the FPP instance UUID;
- playlist name;
- a canonical content hash of the complete FPP playlist definition;
- section and zero-based section position;
- the entry's sequence and media filenames, when present;
- playback action or state;
- an event sequence and observation time.

The common entry key is derived from the FPP instance UUID, playlist name, playlist content hash, section, and position. It is deterministic for an unchanged playlist, survives FPP and plugin restarts, distinguishes duplicate filenames at different positions, and changes deliberately when the imported playlist definition changes. ShowMesh then refuses the old binding until it is reconciled against the new playlist revision.

This is preferred over minting an independent random UUID inside each FPP entry. FPP does preserve entry configuration in its runtime object, but a custom field's survival through every FPP editor, API, import, and upgrade path is not yet established. A persisted explicit UUID may be added later if preserving entry identity across reordering proves valuable, but it is not required for safe day-one matching.

SM-63's approved hybrid plugin structure supplies a resident native component with version-matched FPP 9 and FPP 10 adapters alongside the forked Go macro helper. The native component will use FPP's playlist callback to capture playlist information, action, section, and item position atomically and will publish the entry-identity message through its coordinator-facing contract. Existing independent MQTT topics remain compatibility evidence, but they are not the canonical identity event because their fields can arrive separately and describe different instants during a transition.

### 6. Active-show isolation is enforced at configuration, activation, dispatch, and execution

Show scoping is not a UI filter. It is a runtime safety invariant enforced at every boundary.

The following rules apply:

1. A Playlist may reference only Cues belonging to the same show.
2. A Cue may reference only assets, actions, macros, surfaces, and other show-owned objects belonging to the same show.
3. A runner observation can activate a Playlist only when that Playlist belongs to the active show.
4. Every cue activation and downstream node assignment carries the show identifier and active-show revision or generation that authorized it.
5. A coordinator or node rejects an activation whose show does not equal its currently authorized active show, even when the filename, Cue ID, asset hash, or playlist entry otherwise resolves.
6. Switching `show.active` invalidates the previous show's activation authority and requires readiness for the newly active show's playlists, Cues, and assets.
7. Runtime matching never falls back across show namespaces.

If FPP plays a Halloween sequence while Christmas is active, ShowMesh records an external-playback/show mismatch and does not activate the Halloween Cue. It also does not search Christmas for a Cue with a coincidentally matching filename. The affected FPP-bound ShowMesh playback path enters an explicit held or mismatched state and surfaces operator-visible evidence. The precise output policy while held—freeze, black, silence, or a configured safe Cue—must be settled before implementation; cross-show activation is never one of the options.

**Amended 2026-09-02: a refused observation is positive evidence too, and a persisted discontinuity in it deactivates the affected cue's own outputs.** The paragraph above governs the case where FPP still reports playback evidence that contradicts the active show's own binding. It does not cover a narrower, earlier case: the coordinator's own observation stream for one FPP instance losing continuity, most commonly because fppd or its plugin restarted mid show. FPP-PLUGIN-COORDINATOR-CONTRACTS.md section 1.5 refuses any observation whose sequence regresses against that instance's stored anchor, and that refusal is itself positive evidence, not an absence of it. An observation that arrives and fails validation is an observed discontinuity, worth exactly as much as an observation that arrives and validates; an observation that never arrives at all is not evidence of anything and must never be treated as if it were. A momentary network or broker gap produces no observation either way and must never deactivate anything on its own; a persisted sequence regression is what deactivates, scoped to exactly the affected cue's own previously declared outputs, never the whole node, the background bed, or a session staged ahead for the next cue.

This discontinuity is first-class state, a nullable timestamp (`evidence_broken_at_millis`, schema v29) on the affected instance's own observation row, read by the coordinator's activation decision ahead of anything else that row's content would otherwise resolve to. It is deliberately never derived by reading the audit log entry the same refusal also writes, even though that entry carries the identical fact, for two reasons. First, the audit write is best effort: its own error is logged and swallowed, correct for a forensic record and wrong for a control input, because a write failure there would silently degrade a real discontinuity back into looking like ordinary silence at exactly the moment the store is already unhealthy. Second, reading the audit log from the activation path would couple show behavior to a forensic record's own retention or pruning policy; whether a cue deactivates must never become a side effect of how long audit rows happen to be kept. The marker is a timestamp rather than a boolean because it must record when the coordinator's own evidence for that instance went blind, not merely that it did: the dispatch that stops the affected cue's outputs derives its own idempotency key from the instance's UUID together with that timestamp, which is what makes a repeat tick over the same unresolved break replay instead of re-dispatching.

The marker clears on the instance's next accepted observation, which happens automatically once that instance's own sequence counter climbs back past the stored anchor, with no operator action anywhere in that path. It also clears through the existing operator reset route, which deletes the stored observation and its sequence anchor for that instance outright. Both are reachable, and the operator route exists precisely because the automatic one is unbounded: an fppd restart resets a plugin's own in-memory sequence counter to zero, so an instance can stay marked broken for however long its own counter takes to climb back past whatever the coordinator last accepted, arbitrarily long on a long-running show, and the operator reset is the fast path out of that wait, never the only one. The refusal that sets the marker is per observation, never a lock on the instance: only a sequence lower than the stored anchor is refused, and a higher one is always accepted on its own merits regardless of whether an operator has done anything. `TestFPPPlaylistEntryObservationAcceptClearsEvidenceBrokenMarker` proves the automatic path directly: an instance marked broken at sequence 5 clears the moment an ordinary accepted observation at sequence 6 arrives.

The discontinuity is surfaced to an operator on two surfaces because they answer different questions. `GET /current-runs` collapses it into that run's own `reconciliation.state` (`evidence-broken`), outranking whatever the ordinary reconciliation outcome would otherwise report for the same row, because the primary Show Night view needs one word per run, not two overlapping facts. The existing per-instance reconciliation route instead reports it as an additive `evidenceBrokenAt` field alongside the raw, uncollapsed outcome and reason, because a caller drilling into one instance, an operator following up on a current-runs alert, or an integrator working with no UI at all, needs both facts distinctly: what ordinary reconciliation says about the row's own content, and that the row's continuity separately broke. Collapsing that route the same way `GET /current-runs` does would lose exactly the distinction a diagnostic view exists to preserve.

This rule applies even if nodes retain inactive-show assets locally. Asset presence grants no authority to execute them.

### 7. Agents receive resolved Cue catalogs before showtime

The coordinator resolves show configuration and deploys the active show's required Cue catalog and assets to participating nodes before playback. Nodes use the common cue-activation contract to switch local render, audio, and LTC state.

The coordinator is not inserted into the frame-accurate timing path. For an FPP runner, agents use MultiSync to observe filename and position, then select only from the pre-authorized active-show catalog. REST and MQTT metadata provide corroboration and mismatch evidence. This preserves operation through coordinator or broker loss after the required catalog has been deployed.

An unknown, stale, ambiguous, or cross-show binding becomes a stated fault or hold. A node must not continue rendering the previous FSEQ while reporting healthy, and it must not substitute a same-named asset from another show.

### 8. ShowMesh owns auxiliary audio and announcement execution

A `showmesh-audio` Playlist is a first-class show-owned Playlist whose entries reference audio-capable Cues. It may run concurrently with an FPP-backed Playlist when their declared resource claims do not conflict. The normal day-one case is FPP running a resting lighting sequence while ShowMesh independently advances a background-music Playlist.

A background-audio Cue claims the configured program-audio route. An announcement Cue claims an announcement session and applies its declared duck, mix, or interrupt relationship to the background session. Neither Cue affects FPP, rendering, or LTC merely because it is active; those effects occur only when the Cue explicitly declares the corresponding same-show output or Action.

Switching `show.active` invalidates the previous show's auxiliary-audio authority. Background playlists and announcements from the previous show are stopped or cleared under an explicit transition policy, and the newly active show's auxiliary audio must pass readiness before it can start.

### 9. Existing audio PlaylistRef is not the show-program model

The current audio `PlaylistRef` may remain as a local audio-engine primitive beneath the `showmesh-audio` runner. It is not itself the show-level authoring model and must not be used as the model of an FPP-backed show Playlist.

For an FPP-backed Cue activation, the audio engine selects the Cue's audio asset, aligns it to the runner's observed position, and emits LTC at:

```text
Cue LTC start offset + current Cue position
```

The audio engine does not autonomously advance to the next FPP-backed Cue. Natural completion and progression belong to the configured runner.

## Consequences

- `show.cue` and `show.playlist` become new revisioned configuration kinds with API, CLI, UI, validation, audit, and readiness parity.
- The UI can present a Show as the namespace containing its Playlists, Cues, Macros, Actions, Surfaces, and assets without nesting all of those objects into one revision.
- FPP import becomes an authoring and reconciliation surface rather than a second scheduler.
- A minimal ShowMesh-owned runner is day-one scope for background and auxiliary audio; only the general primary-show runner remains future work.
- Announcements are directly activatable Cues whose audio and mix policy remain independent of FPP unless explicitly linked to a same-show FPP Action or Macro.
- The FPP integration publishes one atomic, revisioned entry identity instead of asking consumers to correlate separately arriving playlist topics.
- Per-Cue LTC offsets solve the immediate multi-song Resolume requirement without embedding FPP vocabulary into the audio engine.
- Rendering and audio consume the same Cue activation instead of independently inferring which show item is active.
- A future ShowMesh-native playlist runner can reuse Cue definitions and node execution without migrating FPP-specific objects into a new content model.
- Active-show switching becomes a runtime authorization boundary as well as an audited configuration change.
- Agents need an explicit active-show generation and catalog-revision mechanism; carrying only filenames or asset hashes is insufficient.
- FPP playlist edits can invalidate imported bindings. Staleness must appear in readiness before showtime and as mismatch evidence at runtime.
- The current single-FSEQ render assignment and autonomous audio-playlist progression remain incomplete for FPP-backed shows until they consume Cue activations.

## Alternatives considered

### Put FPP trigger fields directly on Cue

Rejected. It makes Cues FPP-specific, prevents clean reuse across playlists, and makes a future ShowMesh runner a migration rather than another implementation of the same contract.

### Treat the runtime filename as the Cue identifier

Rejected. Filenames are not globally stable, can repeat within a playlist, can exist in more than one show, and do not prove active-show authority.

### Add LTC offset only to the existing audio PlaylistItem

Rejected. It fixes one field while preserving the wrong progression authority and leaves rendering, FPP identity, UI authoring, and cross-show isolation unsolved.

### Make ShowMesh the general primary-show playlist runner immediately

Rejected for the initial implementation. Existing shows already rely on FPP scheduling and playlist operation. A narrow `showmesh-audio` runner is still required immediately for background and auxiliary audio; the model permits a general ShowMesh runner later without requiring it before the FPP-backed primary-show path works.

### Keep one global Cue or asset catalog and filter it in the UI

Rejected. UI filtering cannot prevent a stale event, API call, restored session, or node-local mapping from crossing from one seasonal show into another.

### Nest Playlists, Cues, and Macros inside the Show payload

Rejected for the same revision-history reason as ADR-027: editing one Cue should create a Cue revision, not an opaque revision of the entire show.

## Follow-up decisions required before implementation

1. What exact held-output policy applies when FPP playback contradicts the active show: freeze, black and silence, or a configured safe Cue?
2. Does an LTC offset belong only to a Cue, or is there a demonstrated need for a Playlist-entry override when one Cue appears more than once?
3. Which Cue relationships are declarative on day 0: render sequence, audio, LTC only, or also entry/exit Actions and Macros?
4. Beyond the known FPP-plus-background-audio case, which resource claims permit concurrent Playlists and prevent two Cues from driving the same audio route, surface, or LTC output?

## Related decisions and work

- ADR-001: FPP Is the Authoritative Scheduler — narrowed by this record to FPP-backed playlists.
- ADR-027: The Show and Surface Model, and Who Owns Authoring — retained; this record extends its show namespace.
- ADR-028: Show Asset Store and Identity — retained as the downstream asset identity model.
- ADR-029: Logical Actions and Integration Bindings — precedent for separating stable show intent from integration bindings.
- ADR-031: The Macro Execution Model — retained; Playlists and Cues do not replace Macros and Actions.
- SM-40: multi-sequence FPP playlists do not switch the render source.
- SM-63 / RES-018: FPP 9/10 hybrid plugin runtime and three-repository release flow.
- SM-145: LTC behavior across playlist items and announcements.

## Supersession

This record does not supersede ADR-027, ADR-028, ADR-029, or ADR-031. It narrows ADR-001's universal wording: FPP is authoritative for FPP-backed Playlists, while playlist authority is otherwise assigned to the Playlist's configured runner.
