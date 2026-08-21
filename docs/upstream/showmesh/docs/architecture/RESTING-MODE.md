# Resting Mode and Night Session Specification

[Documentation index](../README.md) · [Architecture specification](ARCHITECTURE.md) · [Audio Engine](AUDIO-ENGINE.md) · [ADR-038](../decisions/ADR-038-fpp-authorizes-night-sessions.md) · [Track F](../build/TRACK-F-resting-mode.md)

Status: Accepted architecture baseline — specified, not implemented or integrated; optional site-control/interlock posture clarified 2026-08-17
Audience: Maintainers, operators, and agents implementing the night-session controller

## 1. Purpose

Resting mode is the coordinated presentation between shows, not a background-audio toggle. It may contain:

- a one-shot xLights/FSEQ lighting sequence;
- background music;
- one or more resting projection layers;
- a countdown until the next show;
- moving-head or other device state;
- announcements;
- an end-of-night variant that repeats without starting another show.

This document defines the operating-day commands, the repeated rest/show loop, transition timing, audio and projection behavior, power and thermal boundaries, persistence, recovery, configuration, and evidence required to implement it.

It does not add a calendar scheduler to ShowMesh. [ADR-038](../decisions/ADR-038-fpp-authorizes-night-sessions.md) is the authority rule.

## 2. Authority and time

Authority is split three ways:

1. **FPP owns calendar time.** Every dated or wall-clock action is an FPP schedule entry.
2. **Authored content owns duration.** The deployed resting FSEQ variant defines the inter-show rest length.
3. **ShowMesh owns relative choreography.** It advances an FPP-authorized session and schedules offsets around observed media boundaries.

ShowMesh configuration may contain durations that are intrinsic to command execution: fade duration, lead time, blackout hold, announcement delay, confirmation deadline, retry backoff, optional cooldown, and optional interlock timeout. It must not contain dates, weekdays, time zones, cron expressions, or a wall-clock show schedule.

Power control, environmental integration, and interlocks are optional capabilities. The night loop is valid without any of them. Readiness evaluates only requirements enabled by the active show and installation configuration.

## 3. Lifecycle vocabulary

Night-session state is installation-wide and distinct from [Program/Show Mode](../decisions/ADR-033-show-mode.md). Program/Show Mode changes operational footprint; it is not derived from playback. Night-session state describes the active presentation lifecycle.

```text
inactive/stopped -> preparing -> preshow -> transition-to-show -> live
                                  \-> resting-intershow -/

live -> transition-to-resting -> resting-intershow -> transition-to-show
                              \-> end-of-night-resting -> fading-out -> stopped
```

`blocked` and `degraded` are outcomes/evidence attached to a state, not alternate lifecycle states. `blackout` remains an emergency presentation state and does not mean equipment is powered down.

## 4. FPP-scheduled lifecycle commands

All commands are asynchronous, idempotent by invocation identity where available and by state where not, and visible in the audit/event surfaces.

### 4.1 `prepare-site`

Opens a new persisted preparation epoch and enters `preparing` when the prior session is `inactive` or `stopped`, then runs any configured show-day preparation actions. A deployment may use it for presentation power or a Home Assistant thermal mode; a deployment with neither still opens a valid epoch. ShowMesh does not directly cycle a heater, exhaust fan, or thermostat.

Every later preparation command attaches to this epoch. A duplicate within the same preparation or active session is an idempotent no-op. It is rejected during finalization or fade-out. The epoch becomes eligible for readiness and pre-show after every configured blocking preparation requirement has fresh post-epoch evidence. With no blocking preparation requirements, it is eligible immediately. A delayed command from the prior operating day cannot satisfy a new epoch using old evidence.

### 4.2 `run-readiness`

Starts readiness evaluation for the current preparation epoch: required nodes, exact asset variants, FPP state, projection bindings, audio outputs, and only the controlled devices, environmental signals, and interlocks enabled for this installation/show. It is rejected when no preparation epoch is open.

The result records the epoch, completion time, and evidence times. `start-night` requires a completed result from the same epoch within a configured maximum age and re-evaluates only blocking interlocks whose declared phase guards `start-night` or its immediately required actions. A shutdown, cooldown, or power-removal interlock is reported in readiness but cannot gate night startup. Readiness from a prior operating day is never adopted into a new epoch.

### 4.3 `start-preshow`

Enters the configured pre-show presentation. It may start resting projections, background music, countdowns, and the configured resting playlist. Pre-show is a distinct presentation profile but uses the same exit-transition machinery as inter-show resting.

`start-preshow` requires the current preparation epoch and attaches the presentation to it. A duplicate inside the same prepared or active session is an idempotent no-op; it does not manufacture a second epoch. A stale `start-preshow` after shutdown is rejected because no unconsumed preparation epoch exists.

### 4.4 `start-night`

Authorizes the night session and begins the first pre-show/resting-to-show transition. It is accepted only from a prepared epoch with fresh readiness from that same epoch and the expected pre-show/resting presentation observed. The scheduled command begins the transition; the show playlist begins after the configured fades, blackout, and optional announcement. If an operator wants the playlist itself to begin at an exact wall-clock time, the FPP command is scheduled earlier by the known transition duration.

State handling is closed:

| State | `start-night` behavior |
|---|---|
| prepared `preshow` | Consume the epoch and start a new night session. |
| `resting-intershow`, either transition, or `live` | Idempotent no-op returning the active session. |
| `end-of-night-resting` | Reject; finalization is monotonic. |
| `fading-out` or `stopped` | Reject; a new `prepare-site` epoch followed by readiness and pre-show is required. |
| `preparing` or `inactive` | Reject as not ready for night start; operator recovery completes readiness and invokes `start-preshow` first. |

This guard and the `prepare-site` epoch distinguish the next day's intentional start from delayed `start-preshow`/`start-night` commands after shutdown without adding a calendar field to ShowMesh.

### 4.5 `request-final-show`

Closes admission after one final complete show:

| Observed state | Required behavior |
|---|---|
| `live` | Mark the current playlist as final and let it finish. |
| `resting-intershow` | Finish the current resting FSEQ normally; the next show is final. |
| `preshow` | The first show is final. |
| transition to show | The show being entered is final. |
| transition to resting | Enter end-of-night resting when the transition completes. |
| end-of-night or later | Idempotent no-op with evidence. |

The command never starts a second show after the final playlist.

### 4.6 `fade-out-night`

Fades the active non-live presentation to stopped. Receipt closes admission immediately and cancels any armed next-show boundary. In `preshow`, it fades the active pre-show/resting elements without launching a show. During inter-show resting before another show is committed, it does the same. When received during live playback or an already-committed transition into a show, that show becomes final and the fade remains pending until observed completion. A late `request-final-show` cannot reopen admission. A separate explicit emergency action is required to interrupt live playback.

### 4.7 `power-down-presentation`

Closes the session after ShowMesh has observed that playback stopped and the fade completed. Receipt also closes admission and implies `fade-out-night` if it has not occurred. If presentation-power actions are configured, it performs their configured shutdown, confirmation, and cooldown steps. With no power actions, it records `not_configured` for that optional phase and reaches `stopped` without error. Any configured power action must remain scoped away from environmental protection.

### 4.8 `end-session` (operator recovery, not an FPP-invoked intent)

Abandons the current session, reaches `stopped`, and launches nothing. FPP does not invoke it;
it exists for an operator facing a session ShowMesh cannot safely resume.

A coordinator restart mid-transition leaves the session degraded, because ShowMesh cannot confirm
what is safe to resume and will not guess (§4.4, §11). Without a way out that state is permanent.
`end-session` is the way out: after it, the operator runs `prepare-site` and the ordinary sequence
with readiness evaluated fresh.

It deliberately does **not** clear the degraded flag and continue, and it cannot start a show,
resume an unconfirmed session, or skip readiness. The three admission-closing commands
(`request-final-show`, `fade-out-night`, `power-down-presentation`) remain accepted while
degraded, because a refusal for want of ShowMesh's own evidence is worse than the coordinator
being switched off ([ADR-024](../decisions/ADR-024-identity-authorization-and-audit.md)
decision 7).

See [ADR-041](../decisions/ADR-041-operator-recovery-is-not-a-calendar-intent.md) for why this
sits alongside §4's closed set rather than inside it.

## 5. Reference operating day

Times below document the reference installation's shape. They are examples configured in FPP and are not product defaults.

| Example time | FPP or external action | ShowMesh responsibility |
|---|---|---|
| 4:00 PM | `prepare-site` | Open the operating-day preparation epoch; optionally request a show-day thermal profile if the enclosure is not already maintained continuously. |
| 4:10 PM | Optional presentation power-on intent | Where configured, power LEDs, moving heads, render/audio equipment, and strike projectors after any configured blocking requirements pass. |
| 4:15 PM | `run-readiness` | Begin verification as devices become observable. |
| 4:30 PM | `start-preshow` | Start projections, background music, countdowns, and configured resting presentation. |
| 5:00 PM | `start-night` | Begin the first transition and then launch the first show playlist. |
| 10:00 PM | FPP schedule invokes `ShowMesh: Set Brightness Ceiling(targetPercent, fadeSeconds)` | Preserve the target and fade duration from the existing `fpp-brightness` entry; observe the interpolated ceiling and do not overwrite it with transition gain. |
| 11:00 PM | FPP schedule invokes `ShowMesh: Set Brightness Ceiling(targetPercent, fadeSeconds)` | Preserve the lower target and fade duration from the existing `fpp-brightness` entry; observe the interpolated ceiling and do not overwrite it with transition gain. |
| 11:30 PM | `request-final-show` | Finish the current show or allow the next normally timed show, then close admission. |
| After final show | Event-driven | Enter end-of-night resting; background music and projections return and the resting FSEQ repeats. |
| Christmas 12:00 AM; Halloween 1:00 AM | `fade-out-night` | Fade presentation elements to stopped. |
| Example five minutes later | Optional `power-down-presentation` | Where configured, safely shut down and remove presentation power after any required cooldown. |

The Christmas and Halloween fade times illustrate per-show FPP schedules. ShowMesh stores neither time.

## 6. Inter-show timing authority

### 6.1 Exact asset variant

The configuration references both an FPP-owned resting playlist and a logical resting timeline asset. For v1, the playlist contains exactly one FSEQ item and no FPP audio item; its item must resolve to the exact FSEQ variant deployed to that FPP target, using the asset identity rules in [ADR-028](../decisions/ADR-028-show-asset-store-and-identity.md). Filename alone is insufficient.

FPP owns creation and item ordering for the playlist. Track F references and validates it; it does not create an undocumented FPP object or invent a sequence-start primitive. A future authoring surface may manage the same FPP playlist through a separately specified integration.

ShowMesh extracts and records the duration from that artifact. A hand-entered duplicate `restDuration` is forbidden.

### 6.2 Playback anchor

Sending `start` does not establish time zero. The anchor is post-dispatch evidence from FPP that the intended resting item is playing, including its identity and position. For a confirmed start at position `p` with duration `D`, the expected content boundary is derived from the observation, not from request receipt.

ShowMesh continually compares the armed boundary with FPP's observed playlist, item index, elapsed time, remaining time, repeat state, pause state, and playback state. A restart, seek, pause, delayed start, or item mismatch moves or invalidates the boundary.

Ordinary low-rate observability polling is not a cue clock. The controller arms a local monotonic deadline from authoritative duration/position evidence and uses later observations to correct or invalidate it. Bench work must establish the event or poll cadence needed to keep errors inside the configured cue tolerance.

### 6.3 One-shot and repeat modes

- `resting-intershow` starts the configured one-item resting playlist with repeat disabled. Its FSEQ end drives the next show transition.
- `end-of-night-resting` starts the same or separately configured one-item resting playlist with repeat enabled. It has no show-transition deadline.

FPP and ShowMesh must never independently repeat the same inter-show item.

## 7. Transition choreography

Transitions are timelines of named logical actions, not raw MQTT topics, Resolume paths, or FPP protocol commands. Each cue declares its offset, duration where applicable, confirmation contract, failure policy, and fallback class.

### 7.1 Resting to show

The content boundary `E` is the end of the resting FSEQ. Cues may begin before or after it:

```text
E - lighting lead       begin lighting fade
E - projection lead     begin projection fade
E - audio lead          begin audio fade
E                       resting FSEQ ends; blackout barrier begins
E + announcement offset optional announcement
E + blackout duration   launch show playlist
```

This supports theater-style staging where lights and projections reach black first, music continues briefly in darkness, and audio then fades before an announcement or show start.

The show playlist starts only after all required pre-show cues have reached their declared barrier outcome. The policy for a failed or unconfirmed non-safety cue is explicit per cue; absence of evidence is never silently treated as success.

### 7.1.1 Show-commit boundary

A future show is **armed** while its boundary is known but no outward-facing enter-show cue has been dispatched. It becomes **committed** in one database transaction that writes `showCommitted=true` and a durable pending outbox record for the first outward-facing enter-show cue, normally the earliest lighting, projection, or audio fade. The outbox record pins the action revision and carries a stable cue invocation identity derived from session, cycle, and cue identity.

If `fade-out-night` wins the ordering before commit, the controller cancels the armed boundary and fades the current resting/preshow presentation without launching a show. If the first cue wins and commits the show, a concurrent or later fade request makes that show final and waits for it to complete. The controller never attempts to reverse a partially visible transition.

Dispatch happens from the durable outbox after commit. Crash recovery follows evidence and idempotency:

- if post-dispatch target evidence proves the cue took effect, record its outcome and do not send it again;
- if no effect is observed and the action supports the same stable idempotency identity end to end, retry that outbox record with the same identity;
- if the action is non-idempotent or structurally unconfirmable, mark the cue ambiguous, do not retry it automatically, and do not cross the show-launch barrier without operator recovery.

Configuration rejects a non-idempotent, unconfirmable action as the first outward-facing cue. A crash before the send therefore cannot leave the controller choosing between an unsafe duplicate and pretending a transition occurred.

### 7.2 Show to resting

The return transition begins only from observed evidence that the show playlist completed. Acceptance of FPP's graceful-stop command or entry into `stopping gracefully` is not completion evidence.

The default order is:

1. Observe the final show playlist end.
2. Hold post-show blackout for the configured duration.
3. Optionally play a thank-you announcement.
4. Start the next one-shot resting playlist, or the repeating end-of-night resting playlist after the final show.
5. Fade up resting lights, projections, audio, and other media on independently configured offsets and durations.
6. Limit background audio to its configured maximum gain.

The ordering of steps 2–5 is configurable through relative cues, but show completion remains the authoritative anchor.

### 7.3 Brightness composition

**Selected design, not yet implemented.** [RES-018](../research/RES-018-fpp-brightness-control.md) records the owner-approved two-value FPP component and the FPP 9/10 build plan. Stock FPP and the upstream `fpp-brightness` plugin do not expose this composition. Until the ShowMesh component passes its real-host acceptance matrix, readiness rejects a configuration that requires this seam.

FPP may change the installation brightness ceiling on its own schedule. ShowMesh transition fades are a multiplier, not a replacement:

```text
effective output = current FPP brightness ceiling * ShowMesh transition gain
```

A fade-up returns to the current ceiling, not a cached earlier ceiling. This requires a lighting action/provider that can both observe the scheduled ceiling and apply a separate transition multiplier. The current shipped FPP collector and command vocabulary do not yet provide that seam; Track F F0 must capture it and F4 must implement it before this behavior can be claimed.

If the ceiling cannot be observed or the control path exposes only one destructive absolute brightness value, the configuration is unsupported: readiness must say so and ShowMesh must not perform a fade that could overwrite the scheduled limit. The fallback is an authored FSEQ fade or a provider that supplies the missing compositional control, not pretending an unknown ceiling was preserved.

## 8. Audio

Resting audio is a ShowMesh `background` playback session. It uses node-local assets, loop behavior, gain, and the Audio Engine's fade machinery. Configuration includes source or playlist, loop policy, fade curve, per-transition offsets, maximum resting gain, and whether a source resumes or restarts where that distinction applies. A multi-item playlist follows `AUDIO-ENGINE.md` §3: it pins an ordered revision and exact item assets, advances each item once, and never guesses after a stale bookmark or missing next item.

Announcements are separate higher-priority sessions. A normal transition announcement is placed after the configured background fade or uses an explicitly configured duck/interrupt policy. Public-safety interruption of all playout is a separate future safety design; this document does not represent ShowMesh as an emergency-alert receiver.

The generic asset store remains codec-agnostic. Every output validates its own supported-format and mix capabilities without narrowing formats available to other outputs. The first synchronized-third-party compatibility corpus uses the formats FPP recognizes as audio, but that is an L0 owner assumption rather than evidence that a destination accepts them. Advance provisioning, absent-readiness behavior, and open integration questions are recorded in [RES-016](../research/RES-016-third-party-synchronized-audio-output.md).

## 9. Projections and other presentation systems

Resting projections, countdowns, pre-show text, blackout, and resting layers are explicit ShowMesh logical actions. Timecode remains the authority for timeline-driven show content. A transition uses stable named action bindings and the Resolume adapter's confirmation evidence; it never embeds REST paths or object identifiers.

Every additional presentation system joins the same cue model by providing named actions with honest confirmation and fallback metadata.

## 10. Optional power, interlocks, enclosure climate, and Home Assistant

None of this section is required to run a night session. Deployments opt into the actions and evidence they actually have. Missing optional integration is `not_configured`, not degraded or failed.

### 10.1 Configurable interlocks

An interlock is a named rule attached to exactly one lifecycle phase. Rule names are unique within a configuration revision. The v1 phase enum is `prepare-site`, `presentation-power-on`, `projector-strike`, `run-readiness`, `start-preshow`, `start-night`, `enter-resting`, `fade-out-night`, and `power-down-presentation`; unknown values are rejected rather than treated as observational. ShowMesh ships the rule mechanism, not universal device requirements or thresholds. Enabled rules declare:

- the phase it guards, such as preparation, projector strike, show start, fade-out, or power removal;
- its signal/evidence source and expected condition;
- freshness and evaluation timeout;
- posture: `observe` or `block`;
- operator-facing failure text;
- for `block` only, required `onUnavailable: block|allow` with no default;
- for `block` only, required `overridePolicy: none|authorized-operator` with no default.

The closed behavior matrix is:

| Posture | Condition false | Source unavailable |
|---|---|---|
| `observe` | Report failed observation; never withhold. | Report `unknown`; never withhold. |
| `block`, `onUnavailable: block` | Withhold only the declared phase/action. | Report `unknown` and withhold that phase/action. |
| `block`, `onUnavailable: allow` | Withhold only the declared phase/action. | Report `unknown` and allow that phase/action. |
| `disabled` | Do not evaluate. | Do not evaluate. |

A disabled entry contains only its name, phase, and `posture: disabled`; signal, condition, freshness, timeout, unavailable, and override fields are rejected. An observe rule may not set `onUnavailable` or `overridePolicy` because it never withholds. A block rule must set both. An override is accepted only when that rule declares `authorized-operator`, the caller has the required permission, and the override identifies the rule, phase, reason, and bounded invocation/session scope in the audit log. These validation rules make contradictory combinations invalid rather than implementation-defined.

Only rules for the phase currently being entered can withhold that phase. Other rules may be prefetched and displayed but have no control effect until their own phase. A deployment with no interlocks proceeds from ordinary readiness evidence. There is no built-in projector-temperature interlock, mandatory cooldown, or assumed Home Assistant installation.

### 10.2 Separate power domains when power control is configured

When ShowMesh controls power, presentation and environmental power are different groups:

```text
presentation:
  projectors, LED controllers, moving heads, audio/render equipment

environmental:
  enclosure heater, thermostat, temperature sensors,
  exhaust and circulation fans
```

Every configured power binding declares:

- `powerDomain: presentation|environmental|mixed|unknown`;
- `domainProvenance: provider|operator-declared`;
- the named logical action it invokes.

A provider may supply domain provenance only when it can authoritatively identify every target. Generic MQTT and Home Assistant service-call bindings are `operator-declared`: ShowMesh can validate the declaration but cannot infer or verify the physical wiring behind a topic, entity, relay, or downstream automation. Operator surfaces and audit evidence must preserve that distinction.

`power-down-presentation` accepts only bindings declared as `powerDomain: presentation`. It rejects `environmental`, `mixed`, and `unknown` bindings rather than guessing. A deployment may omit the command and all power bindings entirely.

Every presentation power-off binding also declares one removal policy, with no default:

- `immediate`: `immediateSafeAttestation` explicitly records that every target may safely lose power as soon as the action runs;
- `after-actions`: a non-empty ordered list of required shutdown actions, confirmations, delays, or evidence conditions must complete before removal.

An aggregate binding containing any target that is not safe for immediate removal must use `after-actions`. Prerequisite lists and delays are finite, non-negative, limited by versioned schema bounds, and may not invoke the same power-off binding directly or indirectly. Missing domain, provenance, policy, or prerequisites required by the selected policy makes the configuration invalid. This keeps cooldown behavior configurable without treating it as optional by accident after a device that needs it has been configured.

### 10.3 Optional Home Assistant thermal integration

In the reference installation, Home Assistant owns continuous freeze protection, thermostat cycling, exhaust control, and protection against over-cooling. In severe weather it may heat continuously for days before a show. While projectors run, it may need exhaust fans to remove their heat while maintaining a minimum safe enclosure temperature. This is an installation profile, not a ShowMesh requirement.

ShowMesh does not implement that thermostat. It may invoke a named Home Assistant mode such as `show-preheat`, `projectors-running`, or `standby`, and may observe temperature, mode, and fan/heater state as readiness evidence. Names are installation configuration, not product constants.

If an operator configures a blocking projector-strike temperature interlock, unsafe or stale evidence behaves according to that rule and may block automated strike. An observe-only rule reports the same evidence without blocking. With no rule, ShowMesh assumes no thermal interlock. No posture causes ShowMesh to take over thermostat control.

### 10.4 Optional safe shutdown

When an `after-actions` removal policy is configured, normal presentation shutdown may:

1. wait for live playback and the presentation fade to finish;
2. request proper projector shutdown;
3. observe shutdown where supported;
4. wait for configured or observed projector cooldown;
5. remove presentation power through named MQTT/Home Assistant actions;
6. request the normal environmental standby profile without disabling protection.

An ordinary `immediate` policy is valid only for targets explicitly attested safe for immediate removal. A separately configured manual `force-power-off` action may bypass ordinary prerequisites, but it has its own authorization and audit presentation and is never inferred from the ordinary policy. Deployments without ShowMesh power control expose no such action. Normal scheduled shutdown never silently escalates to it.

For the reference installation, the existing MQTT/Node-RED response-contract action is sufficient. A full Home Assistant control provider may replace the binding later without changing the lifecycle contract. A direct FPP/Home Assistant hard cutoff may exist as an installation-specific final fallback, scheduled beyond its configured maximum show and cooldown window.

## 11. Persistence, restart, and loss of coordination

The night-session record persists at minimum:

- session identity and pinned configuration revision;
- preparation epoch, readiness result identity, and evidence freshness;
- lifecycle state and state-entered time;
- final-show request and pending fade/shutdown intents;
- current resting asset and FPP playlist/item identity;
- last authoritative playback position and derived boundary;
- cues dispatched and their outcomes;
- durable cue outbox records, stable cue invocation identities, and pinned action revisions;
- current cycle number;
- armed-show identity and persisted `showCommitted` marker;
- last transition evidence and degradation reason.

The controller is event-driven and does not continuously reissue commands to force desired state.

On coordinator restart:

- if FPP is live, observe without disturbing playback, mark the show final if that intent was persisted, and resume only the post-show transition after observed completion;
- if the exact one-shot resting playlist/item is playing and its position is trustworthy, reconstruct the boundary and arm only future cues;
- if end-of-night repeat is observed and the persisted state agrees, restore observation without starting a show;
- if evidence is ambiguous, hold the current presentation, mark the session degraded, and require operator recovery rather than guessing or launching a playlist.

Pending cue outbox records are resolved before ordinary boundary reconstruction: observe first, retry only with the same end-to-end idempotency identity, and otherwise stop at an explicit ambiguous outcome. A pending show-launch barrier never becomes satisfied merely because the coordinator restarted.

Coordinator or broker loss never stops an already-running FPP playlist or already-playing node-local audio. It may prevent a later transition. The v1 FPP plugin provides no local night-loop fallback, and the system must say so plainly.

## 12. Conceptual configuration

Names are illustrative; the implementation track owns the versioned schema.

```yaml
nightSession:
  preshowProfile: christmas-preshow
  showPlaylist: christmas-show
  resting:
    playlist: christmas-resting
    timelineAsset: christmas-resting-fseq
    backgroundAudio:
      source: playlist
      items:
        - christmas-resting-track-1
        - christmas-resting-track-2
      repeat: playlist
      resume: true
      itemTransition: sequential
    endOfNightRepeat: true
    maxAudioGainDb: -10

  enterShow:
    lighting:
      startBeforeTimelineEnd: 20s
      fadeDuration: 15s
    projections:
      startBeforeTimelineEnd: 20s
      fadeDuration: 15s
    audio:
      startBeforeTimelineEnd: 3s
      fadeDuration: 3s
    blackoutAfterTimeline: 6s
    announcement: show-starts-now

  enterResting:
    blackoutAfterShow: 6s
    announcement: thank-you
    lightingFadeIn: 10s
    projectionFadeIn: 10s
    audioFadeIn: 8s

  # Entire block is omitted where ShowMesh does not manage site power/climate.
  siteControl:
    requestThermalProfile: home-assistant-show-preheat
    presentationPowerOn:
      action: site-presentation-on
      powerDomain: presentation
      domainProvenance: operator-declared
    presentationPowerOff:
      action: site-presentation-off
      powerDomain: presentation
      domainProvenance: operator-declared
      removalPolicy: after-actions
      prerequisites:
        - action: projectors-safe-off
          requireConfirmation: true
        - delay: 5m

  # Each rule is optional and independently observe-only, blocking, or disabled.
  interlocks:
    - name: projector-strike-temperature
      phase: projector-strike
      signal: enclosure-temperature
      condition: ">= configured-safe-minimum"
      freshness: 30s
      posture: block
      onUnavailable: block
      overridePolicy: authorized-operator
```

No field contains a wall-clock time. Omitting `siteControl` and `interlocks` is valid and disables no part of the rest/show loop.

## 13. Readiness and validation

Before `start-night`, ShowMesh must report at least:

- exact resting FSEQ variant present on the correct target;
- configured FPP resting playlist exists, contains exactly the expected FSEQ item, contains no FPP audio item, and can run one-shot or repeated as requested;
- parseable non-zero duration and cue offsets within the usable timeline;
- show playlist present and not unexpectedly busy;
- required audio and announcement assets local and hash-current;
- required audio assets probe successfully with usable duration and other metadata Track C needs;
- every configured audio output declares the background, announcement, playlist, mix/duck/interrupt, loop, gain, fade, seek, position, and requested sequential/gapless/crossfade item-transition capabilities this session requires;
- an optional synchronized third-party output with absent or stale provisioning evidence is a warning and does not block the local/FM path;
- a synchronized third-party output marked required has evidence covering every exact audio and announcement content hash in the pinned Night Session revision, including every background-playlist item: destination-reported states where they exist, or current operator attestations pinned to the immutable destination-configuration revision or fingerprint and each required hash where they do not. One verified item cannot satisfy a multi-item source, and an upload attempt or acknowledgement alone is never silently called ready;
- background output and configured maximum gain available;
- a fresh installed-path program-to-LTC offset measurement with method/provenance and the current threshold verdict, or `unknown` while RES-007 has not established a threshold;
- required projection actions resolve uniquely and are ready;
- required logical actions have valid confirmation/fallback declarations;
- configured observe-only interlocks report their current outcome without blocking;
- blocking interlocks for the phase being entered have fresh passing evidence, explicitly allow unavailable evidence, or have an explicit override permitted by that rule; other-phase failures remain visible but do not block;
- configured power bindings declare their domain and provenance, and presentation power-off bindings target only the presentation domain;
- every configured presentation power-off binding has an explicit valid `immediate` or `after-actions` removal policy;
- final-show and fade-out commands exist in the FPP-facing vocabulary;
- where scheduled brightness and ShowMesh fades affect the same output, an observed ceiling plus an independent transition-gain control exists; otherwise that fade configuration is rejected.

Readiness is evidence, not a refusal to continue by default. Only rules explicitly configured with `posture: block` may withhold their guarded action. Missing optional power, climate, or interlock configuration is `not_configured`, never failure.

## 14. Observability

The API, CLI, UI, audit log, and change stream expose:

- active session and pinned revision;
- lifecycle state and time in state;
- current cycle and whether it is final;
- resting asset duration, observed FPP position, and derived next boundary;
- next cue and its monotonic deadline;
- pending final-show, fade, or shutdown intent;
- per-cue dispatched/completed/confirmed outcome;
- current FPP brightness ceiling and transition gain where observable;
- active background and announcement sessions and effective gain;
- configured interlock outcomes, declared power domain and provenance, removal policy/progress, and presentation/environmental power state, or `not_configured`;
- recovery/degradation reason and required operator action.

Post-show evidence must distinguish playlist completion, blackout activation, resting FSEQ activation, projections restored, background audio restored, and end-of-night repeat state.

## 15. Acceptance scenarios

Implementation is not complete until observed end-to-end behavior covers:

1. a five-minute resting FSEQ whose lighting/projection fades begin before the asset ends and whose audio continues briefly in blackout;
2. show completion followed by blackout, thank-you announcement, and independently timed resting fade-up;
3. repeated cycles with no accumulated timing drift from a duplicated manual rest duration;
4. `request-final-show` during both live and resting states;
5. final show followed by repeating end-of-night resting and no next show;
6. Christmas and Halloween FPP schedules with different fade-out times and identical ShowMesh configuration shape;
7. a direct FPP brightness reduction preserved through later ShowMesh fade-down/fade-up;
8. a coordinator restart during live playback, during resting, and during a transition;
9. a missing/mismatched FSEQ, paused FPP, unexpected playlist, and stale position observation;
10. a missing `start-preshow`, duplicate `start-night`, stale start after shutdown, reordered final/fade commands, and a fade command received before a final-show request;
11. a crash before the first cue send, after the send but before outcome persistence, and while the show-launch barrier is pending;
12. a complete night with no power, climate, or interlock configuration;
13. the reference profile's blocking cold-enclosure rule, projector-generated heat requiring exhaust, and environmental protection surviving configured presentation power-off;
14. an unexpectedly late live show deferring ordinary configured fade and power-off;
15. generic third-party format/capability validation without restricting local-output asset formats, including acknowledgement with no readiness status, optional-output warning behavior, and a required-output policy satisfied by time-stamped audible checks covering every pinned asset on the listener device;
16. false and unavailable `observe` rules reporting without blocking, and `disabled` rules never evaluating;
17. unavailable `block` rules exercising both required `onUnavailable` choices, plus permitted and forbidden manual overrides;
18. a failing shutdown-only interlock remaining visible without gating `start-night`;
19. rejection of environmental, mixed, and unknown presentation power-off bindings, while preserving whether domain provenance is provider-supplied or operator-declared;
20. valid `immediate` and `after-actions` removal policies, plus rejection of missing policies, incomplete prerequisites, and aggregate immediate-off bindings containing a target that requires ordered shutdown.
