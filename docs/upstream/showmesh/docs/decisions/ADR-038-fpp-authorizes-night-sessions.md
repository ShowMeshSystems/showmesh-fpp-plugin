# ADR-038: FPP Authorizes Night Sessions; ShowMesh Advances Them

Status: Accepted (decision 2's "closed command vocabulary" narrowed by [ADR-041](ADR-041-operator-recovery-is-not-a-calendar-intent.md))  
Date: 2026-08-16

## Context

[ADR-001](ADR-001-fpp-is-authoritative.md) made FPP the authoritative scheduler because the reference installation already operates through FPP schedules and because a second calendar creates split brain. That decision assigned scheduled show start to FPP, but it did not define the repeated presentation cycle between an installation opening and closing.

The reference installation previously used the FPP Background Music plugin to own that cycle. An xLights command embedded in the resting sequence triggered the fade into a show, and the plugin returned the installation to resting afterward. It demonstrated the required ownership shape, but the implementation was brittle: the transition depended on an authored command cue, its state and failures were difficult to observe, and end-of-night behavior could hard-stop a running show.

The required cycle is content-driven rather than calendar-driven. A configured resting FSEQ has an authored duration. Its observed playback boundary determines when the next transition begins; independent audio, lighting, and projection cues are offsets around that boundary. FPP still determines when the operating day opens, when the final show is requested, when the presentation fades out, and when scheduled infrastructure actions occur.

## Decision

### 1. FPP owns calendar time and authorizes a bounded night session

FPP owns dates, wall-clock times, weekdays, recurring schedules, and the installation operating window. It invokes named ShowMesh lifecycle commands such as:

- `prepare-site`
- `run-readiness`
- `start-preshow`
- `start-night`
- `request-final-show`
- `fade-out-night`
- optional `power-down-presentation`, only where ShowMesh-managed power exists

ShowMesh does not activate a presentation because a local clock reached a configured time. The core configuration accepts no cron expression, weekday, time zone, date, or wall-clock `at` field.

### 2. ShowMesh owns progression inside the authorized session

After `start-night`, ShowMesh advances the event-driven cycle:

```text
resting -> transition-to-show -> live -> transition-to-resting -> resting
```

It may start configured FPP playlists, observe completion, run relative transition cues, and repeat the cycle until FPP closes admission. The resting FSEQ is played through an FPP-owned resting playlist whose expected item is the configured exact asset variant; ShowMesh does not invent an unsupported start-sequence primitive. This is an explicit narrowing of ADR-001's phrase “scheduled show start”: FPP owns the scheduled start of the night session; ShowMesh owns individual presentation starts inside that authorized session.

The night-session controller is not a general scheduler, an hours-long macro run, or desired-state reconciliation. It is a dedicated persisted lifecycle controller with a closed state machine and a closed command vocabulary.

### 3. Authored media owns presentation duration

Inter-show rest length is not entered as a second duration. The configured, exact deployed resting FSEQ variant is the timing authority.

ShowMesh extracts its duration, anchors timing to confirmed FPP playback, and schedules only relative choreography: fade lead times, fade durations, blackout holds, announcement placement, confirmation deadlines, and safety timeouts. It reconciles armed deadlines against FPP's observed playlist, position, and remaining time.

If the asset is missing, its duration cannot be read, FPP confirms a different item, or the observed duration materially disagrees, readiness fails. ShowMesh does not guess.

### 4. Closing admission and fading out are different commands

`request-final-show` means:

- while live, the current show is final;
- while resting, the next normally timed show is final;
- during pre-show, the first show is final;
- after finalization has begun, a duplicate is an idempotent no-op.

After the final playlist ends, ShowMesh enters end-of-night resting and starts its configured resting FSEQ in repeat mode. No further show transition is armed.

`fade-out-night` ends that resting presentation. If it arrives while a show is unexpectedly live, ordinary behavior is to defer the fade until the playlist finishes. Interrupting a live show requires a separate, explicit emergency or force command.

Shutdown intent is monotonic. Receiving `fade-out-night`, or a configured `power-down-presentation`, closes admission immediately, cancels any armed future-show transition, and cannot be undone by a late `request-final-show`. A transition commits by atomically persisting its marker and a durable outbox record for the first outward-facing cue before dispatch; after that marker, the show is treated as final and allowed to complete rather than reversing a transition the audience has begun to see or hear. Cue recovery observes first and retries only with a stable end-to-end idempotency identity; ambiguity never satisfies the show-launch barrier.

Power actions, environmental integration, and lifecycle interlocks are optional configuration, not universal night-session requirements. Deployments without them run the same content loop and report those optional phases as `not_configured`. A configured interlock explicitly chooses observe-only, blocking, or disabled behavior and can affect only its declared lifecycle phase.

Configured power bindings declare their power domain and whether that classification is provider-supplied or operator-declared. Generic MQTT declarations are not evidence of physical wiring. Presentation power-off accepts only presentation-domain targets and requires an explicit immediate-safe attestation or an ordered prerequisite policy; it never guesses whether cooldown or another shutdown step is needed.

### 5. A future scheduler is a replaceable authority, not a core feature

A future scheduler companion may emit the same lifecycle intents, but only one calendar authority may be active for an installation. Adding it does not add calendar fields to the night-session controller and must have its own ADR and conflict-prevention model.

## Consequences

- The reliable replacement for the previous Background Music plugin loop belongs in ShowMesh.
- FPP schedules remain the operator's source of calendar truth.
- The resting FSEQ becomes both presentation content and the authoritative inter-show duration.
- Relative timers are permitted and required; calendar timers in ShowMesh core are forbidden.
- Existing finite macro runs remain finite and retain [ADR-031](ADR-031-macro-execution-model.md)'s restart behavior. The durable night controller has separate persistence and recovery semantics.
- Coordinator loss never stops an already-running FPP playlist. It can prevent the next ShowMesh-driven transition; recovery is observational and conservative, as specified in [RESTING-MODE](../architecture/RESTING-MODE.md).
- FPP remains the calendar authority for brightness schedules, but those entries invoke the ShowMesh FPP plugin's ceiling command because stock FPP has no direct brightness command. ShowMesh transition gain composes with, rather than overwrites, that current ceiling. The component and migration are specified in [RES-018](../research/RES-018-fpp-brightness-control.md) and remain unverified on a real host.

## Alternatives considered

**One FPP schedule entry per show** was rejected because it recreates the scheduler workaround, can hard-stop at the end of the operating window, and cannot express a content-derived resting interval cleanly.

**An xLights command embedded near the end of every resting FSEQ** was rejected as the primary trigger because it adds an authored command cue to the critical path and repeats the previous installation's opaque failure mode. Such commands remain usable as manual or compatibility inputs, never as the sole timing authority.

**A manually entered five-minute rest duration** was rejected because it duplicates the FSEQ duration and eventually drifts from it.

**A ShowMesh calendar scheduler** was rejected for now because it duplicates FPP and violates the operational boundary established by ADR-001.

**Implementing the full night as one macro** was rejected because macros are finite, do not resume after coordinator restart, refuse overlap, and do not provide the wait, branch, loop, or observation semantics a night session requires.

## Related documents

[Resting Mode and Night Session](../architecture/RESTING-MODE.md) · [RES-018](../research/RES-018-fpp-brightness-control.md) · [ADR-001](ADR-001-fpp-is-authoritative.md) · [ADR-017](ADR-017-showmesh-owns-audience-audio.md) · [ADR-029](ADR-029-logical-actions-and-integration-bindings.md) · [ADR-031](ADR-031-macro-execution-model.md) · [FPP command capture](../bench/fpp-command-vocabulary.md)
