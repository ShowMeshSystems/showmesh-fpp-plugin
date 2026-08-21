# Step 9 specification: show macros and the FPP plugin

[Build plan](BUILD-PLAN.md) · [Build log](BUILD-LOG.md) · [Lessons](LESSONS.md)

Written 2026-08-14 by the orchestrating session. [BUILD-PLAN's Step 9 section](BUILD-PLAN.md#step-9-show-macros-and-the-fpp-plugin) states the goal, the deliverables and the bounding records; this document is the implementation specification builders work from, and it is where the decisions that section leaves open are made rather than discovered.

**Read this whole document before writing code.** Step 8's retrospective established that a defect introduced by a specification is invisible to a reviewer who trusts the specification, so the specification is itself under review in this step. If something here is wrong, say so rather than implementing it.

**Revised 2026-08-14, same day, after that review returned sixteen findings, all confirmed against the code.** Every one is folded in here. The corrections are written as corrections, naming what the first version said and why it was wrong, because several of them are instances of failure modes this project has recorded before and a silent replacement teaches nothing. The largest are §2.2 (an `unconfirmed` step no longer aborts the run), §2.5 (the audit exemption is per step, not per run), §7.2 (the `RetainAsPublished` justification was backwards), and §8.3 (the discharge of ADR-024 decision 7 did not hold in decision 7's own scenario). Two new decisions, §2.9 and §2.10, close questions the first version left for a builder to invent. The review also recommended cutting the external MQTT step entirely; **the owner declined that cut and it stays in Step 9**, so its three findings are fixed here rather than deferred. See §13.

## 1. What this step is

ShowMesh can currently invoke eight FPP primitives, one at a time, each confirmed on evidence post-dating its own dispatch. This step makes a sequence of them a first-class, versioned, persisted, observable thing an operator can name and an FPP schedule can fire.

Three obligations attach to it by name and are not optional:

- **[ADR-004](../decisions/ADR-004-layered-commands-and-fallback.md)'s middle third.** Primitives exist; macros are here; the reduced local fallback is deliberately cut from day-0 and every macro must say so.
- **[ADR-024](../decisions/ADR-024-identity-authorization-and-audit.md) decision 7's fallback trigger**, outstanding since Step 7 and attached to the first consumer of `show:macro:run`, which is this step.
- **[RES-015](../research/RES-015-fpp-plugin-distribution-model.md)'s conclusion** that FPP's native command mechanism can detect neither an authorization refusal nor a transport failure, so ShowMesh-authored code on an FPP host is required on two independent grounds.

## 2. Decisions taken before the build starts

Recorded here so each is a decision rather than an artefact of whichever branch happened to run. Six of them become [ADR-031](../decisions/ADR-031-macro-execution-model.md), because they are durable constraints rather than implementation detail. §2.9 and §2.10 are step-scoped rules rather than durable constraints and stay here.

### 2.1 A macro run is asynchronous

`POST` returns a run identifier and the run's initial state. It never returns a completed result.

This is forced by a lesson this project has already paid for. Step 7 shipped a server that held an unconfirmed response for 20 seconds while the CLI gave up at 10 and the browser at 15, which deleted an outcome from existence and reported a transport failure for a successful conversation. A macro is a sequence of steps each of which may take its own confirmation deadline, so a six-step macro can legitimately run for minutes. **A synchronous macro run recreates that defect at a timescale where it is guaranteed rather than possible.**

Run state is read back with `GET`, and changes are announced on the change stream. The submitting client learns the outcome by watching, not by waiting.

### 2.2 A failed step aborts the remaining sequence. An unconfirmed step does not

> **SUPERSEDED IN PART, 2026-08-14, by [ADR-035](../decisions/ADR-035-a-run-always-runs-every-step.md), after the acceptance run.** The `onFailure` default is now `continue`: a run always runs every step, and a failed step marks the run `completed: false` and names itself rather than suppressing the sequence. `abort` remains available as an explicit per-step choice. Everything below about `failed` and `unconfirmed` being *different facts on independent axes* stands unchanged and is why the change was one line rather than a rewrite: only the failure axis's default direction moved.

Owner's decision, 2026-08-14, revised the same day after the specification review. **The first version of this decision collapsed `failed` and `unconfirmed` into one abort trigger, and that was wrong.** It is recorded here rather than quietly replaced, because the reasoning is the correction:

`failed` means something answered and it was not what the operator declared. `unconfirmed` means evidence was expected and did not arrive inside the deadline. The first is a statement about the show; the second is a statement about ShowMesh's own evidence pipeline. §6.4 defines a five-value vocabulary that separates them precisely, and the original §2.2 threw that separation away at the only decision that consumes it.

Treating `unconfirmed` as failure is absence of evidence read as evidence of absence, which this project has now decided correctly in four other subsystems, and it points the degradation the wrong way. This architecture degrades toward the show continuing. An abort-on-unconfirmed rule degrades toward the show not starting, and §6.3's own arithmetic arms it: a second same-instance step resolves at roughly 15 s against a 20 s deadline, so a slow poll at 17:00 aborts a working show start and leaves the display dark, with the cause being that ShowMesh could not watch rather than that anything failed.

**So there are two policy axes, not one:**

| Field | Default | Values | Meaning |
|---|---|---|---|
| `onFailure` | `abort` | `abort`, `continue` | What a `failed` step does to the remainder. |
| `onUnconfirmed` | `continue` | `continue`, `abort` | What an `unconfirmed` step does to the remainder. |

Both are **optional with a stated default**, and these two are the only keys in this step's payloads where an absent key carries meaning. Every other absent key is an error (§5.4). Both defaults are the safe direction for their own axis: failure stops, a monitoring gap does not.

An `unconfirmed` step that does not abort still sets the run's `confirmed: false` with a reason naming the step, per §2.3. The operator is told; the show is not stopped to tell them.

`onFailure: null`, `onFailure: ""`, and the same two for `onUnconfirmed`, are **errors**, not defaults. The decoder must distinguish an absent key from a present null.

**No automatic compensation.** ADR-004's consequences list says macro execution must be "compensatable"; this step does not build compensation and does not pretend to. An operator recovering from a half-run uses the primitives directly, which is what they have today. Recorded as out of scope in §12 rather than left as an implied capability.

### 2.3 `completed` and `confirmed` are separate facts

Owner's decision, 2026-08-14. A finished run reports two booleans and never collapses them:

- **`completed`** — every step dispatched and none aborted the run.
- **`confirmed`** — every step produced post-dispatch evidence that its effect occurred.

They differ constantly and legitimately. A macro whose MQTT step declares no expected response is structurally unconfirmable ([ADR-029](../decisions/ADR-029-logical-actions-and-integration-bindings.md) decision 4) and will report `completed: true, confirmed: false` every single time it runs correctly. A run that aborted at step 2 reports `completed: false` and whatever `confirmed` had earned by then.

Whenever either is false the run carries a `reason` naming the step and the cause. **The UI must make the difference legible rather than decorative** — this is the field most likely to be rendered as one green tick, and one green tick is exactly the outcome ADR-029 decision 4 says trains an operator to stop reading.

### 2.4 A run interrupted by a coordinator restart is never resumed

On startup, any run left `running` is finished with `completed: false` and a reason saying the coordinator restarted mid-run. It is not resumed and its remaining steps are not dispatched.

Auto-resuming would mean a coordinator that restarts at 03:00 dispatching the second half of a show-start macro at 03:00. The show has moved on and ShowMesh does not know how. This mirrors `ReconcileStrandedFPPCommands`, which already sweeps unresolved commands at startup and resolves rather than retries them.

### 2.5 The audit exemption is per step, and it is declared on the action

> **SUPERSEDED, 2026-08-14, by [ADR-035](../decisions/ADR-035-a-run-always-runs-every-step.md) decision 1, after the acceptance run measured what this section actually did.** A macro run now never withholds a command because the audit store is unwritable, whatever a step's safety class: every step dispatches, and attribution is downgraded to `attributionDegraded` on the step and on the run rather than the command being refused. There is no submission-time gate and no per-step refusal left.
>
> **This section held two incompatible rules and shipped the wrong one**, which is why it is kept rather than deleted. Its own earlier paragraph specifies the per-step rule ("the stop still runs and the start is refused"); its later paragraph specifies an all-steps-exempt submission gate. The implementation followed the later one, so `[stopPlaylist, startPlaylist]` was refused **entirely** with `503` and the stop did not run. ADR-024's own lesson, made a fourth time: an argument that degradation is safe, made against the wrong failure direction. Read the section for the reasoning about safety classes, which is still correct outside a run; do not read it for what the executor does.

Revised 2026-08-14 after the specification review. **The first version made a run exempt if any of its steps was, and that reinstated the exact defect Step 8's review closed**, where the exemption was applied to all eight primitives rather than the two stops so `startPlaylist` would have dispatched unaccountably when the audit store failed. At macro level the same widening is worse: add a `stopPlaylist` step to any macro and every start in it becomes unattributable on a full disk. A stop step becomes a laundering mechanism.

The supporting argument was also made against the wrong failure direction, which is the error [ADR-024](../decisions/ADR-024-identity-authorization-and-audit.md) itself was written to correct. It claimed the alternative "produces a half-run, which is worse for an operator trying to stop a show". But an operator trying to stop a show runs a stop, not a `[stop, start]` macro. Under a per-step rule the stop still runs and the start is refused, so the run aborts under §2.2 with a stated reason. **The feared half-run is the case where the safety-critical half already succeeded.**

**So: the exemption applies per step.** ADR-024 decision 11's closed list of three actions, blackout, stop and power-off, is applied to the step's own action and to nothing else. A refused non-exempt step is `failed` and aborts the run under §2.2 with a reason saying the audit store is unwritable. An exempt step dispatches with degraded attribution, recorded on that step as `attributionDegraded: true` and raised onto the run.

**The second half of this, and it inverts decision 11 if left alone.** Safety class is currently a property of the eight FPP primitives only. An external MQTT action has none, and day-0 projector power is MQTT. Under a naive reading a **power-off macro built from MQTT steps fails closed with the audit store down**, and power-off is one of the three actions decision 11 names by hand as never refusable. The rule would refuse exactly what the ADR exempts.

So a `show.action` carries a required `safetyClass` (§5.3) drawn from a closed enum that matches decision 11's list exactly and adds no members: `none`, `blackout`, `stop`, `powerOff`. For an `fpp` action it must agree with the primitive's own registered class and is rejected at write time if it does not, so the two cannot drift. For an `mqtt` action the operator declares it, which is the only place the information exists.

**The posture is evaluated at submission, not mid-run.** §2.1 makes the run asynchronous, so once a `202` is out there is no HTTP response left to carry a `503`. The coordinator checks audit writability when the run is submitted, and a run whose steps are not all exempt is refused **at that point** with `503`, which is a real response a client can act on. A `503` after submission is unreachable and must not be specified.

An audit store that becomes unwritable **mid-run**, which is the ordinary case of a disk filling during a six-step run, is not a submission-time condition and needs its own answer rather than a builder's invention. It resolves the current step under the per-step rule above, and the run carries `reason` naming the audit store. Both likely inventions are wrong: aborting silently hides it, and proceeding anyway is the widening this section just removed.

### 2.6 An overlapping run of the same macro is refused

A run submitted while another run of the same macro is `running` is refused with `409` naming the in-flight run's identifier.

This follows Step 8's `ifBusy: refuse` precedent, and RES-015 §7.3 supplies the reason it is not theoretical: FPP carries no invocation identity and no retry, so the duplication ShowMesh must defend against comes from the operator, from overlapping schedule entries, and from the MultiSync command fan-out. A double-fired 17:00 schedule must not start the show twice.

Idempotency keys are still required and still work; this is the guard for two *different* keys arriving close together.

### 2.7 Every macro step invokes a logical action, with no exceptions for FPP

[ADR-029](../decisions/ADR-029-logical-actions-and-integration-bindings.md) decision 1 is applied without carve-outs. A macro step names an action; the action holds the binding. An FPP primitive is not exempt just because it is ShowMesh's own vocabulary: `startPlaylist` against instance `fpp-main` with playlist `Halloween Main` is a binding that would otherwise be copied into every macro that starts the show, which is the exact maintenance failure ADR-029 exists to prevent.

**A macro definition therefore contains no instance identifier, no playlist name, no topic, no payload and no primitive name.** It contains action references, an optional per-step `onFailure`, and its own metadata. If a builder finds themselves adding a `params` override on a macro step, that is protocol leaking back into the macro and it is wrong.

### 2.8 The raw protocol escape hatch is not built

ADR-029 decision 3 supports a raw escape hatch and requires it to be deliberately inconvenient. This step does not build it. The action vocabulary covers day-0, and an inconvenient surface with no consumer is a field no code computes, which [ADR-020](../decisions/ADR-020-control-api-shape-and-change-stream.md) forbids. Recorded in §12, not silently skipped.

### 2.9 `show:macro:run` authorizes the run. Step scopes are authorized at authoring time

The specification review found this unstated, and both of the answers a builder would reach for are wrong.

ADR-024 decision 4 gives `scheduler` exactly one scope, `show:macro:run`, and makes `fpp:command` and `device:power` separate scopes it does not hold. So if the executor checked per-step scopes at run time, `scheduler` could not run any macro containing an FPP step, which is every day-0 macro, and §8's whole plugin discharge would be untestable. If it silently did not check, `show:macro:run` would be an umbrella over the entire write vocabulary and the audit trail would lie, attributing `commands` rows to a principal that provably does not hold `fpp:command`.

**The rule, stated so nobody has to guess it:**

- **`show:macro:run` authorizes the run.** It is the only scope checked at submission.
- **The steps were authorized when the macro was written.** Composing an action into a macro requires `config:write`, which is admin-only, so the decision that this macro may fire these primitives was made by an administrator at authoring time and is pinned as a revision.
- **The audit entry records both**: the run's issuer, and the macro revision that authorized the composition. Neither alone answers "who caused this dispatch".
- Each step's `commands` row records the issuing principal **and** the run id, so an investigator reaching a command row can always get back to the authorizing revision.

This is the good answer and it is not the obvious one, which is why it is written down rather than left to the executor.

### 2.10 An external MQTT action names its broker

The coordinator has **two** MQTT configurations and the specification review found the step pointed at the wrong one. `SHOWMESH_MQTT_BROKER` is the ShowMesh control-plane broker in the Compose bundle, which is what `BrokerManager` connects to. `SHOWMESH_FPP_MQTT_BROKER_URL` with its own credentials is the operator's live home-automation broker, added in Step 5 for FPP MQTT ingestion. **Projector control lives on the second one.**

Left implicit, an operator authors `projectors-on`, the coordinator publishes to its own Mosquitto, Node-RED never sees it, the projectors stay off, the waiter listens on the wrong broker and times out, and every symptom points at Node-RED when none of them is Node-RED.

**So an `mqtt` action's target names a broker by identifier** (§5.3), from a closed set the deployment declares. There is no default: an action with no broker is rejected at write time, because a defaulted broker is the same silent-wrong-target failure with an extra step.

**The ADR-008 dimension, stated rather than assumed.** Publishing show-affecting commands onto a broker outside the ShowMesh control plane is a real departure and gets its one sentence: the external broker is an integration target, never a control-plane participant, and nothing in ShowMesh's own liveness, inventory or command path may depend on it. It carries integration traffic only, exactly as an HTTP integration target would.

**And the standing rule is unchanged by this.** §14 forbids any MQTT publish against the operator's live broker during this step's development and acceptance. Every test in §11 uses a bench broker. Shipping the capability to publish to a declared broker and pointing it at the live one are two different acts, and this step does only the first.

## 3. Corrections to the incoming brief

Two things the session was handed are not quite right, and building on them unexamined would cost time.

**"No schema migration needed" is true of macro definitions and false of macro runs.** RES-008's re-survey correctly established that `config_objects` and `config_revisions` are keyed `(kind, id)` over an opaque JSON payload with no `CHECK` constraint on `kind`, so a new *definition* kind is Go code only. A macro **run** is not configuration: it is execution history with per-step outcomes, timestamps, and links to command rows. It needs tables, and therefore **schema v7**. Verified against the v6 DDL directly, not inferred.

**"The generic mechanism has been exercised once and proven against nothing" understates it.** There is no generic mechanism. `fpp.endpoints` is hardcoded end to end: no kind registry, no interface, one literal route per kind, kind-specific wire types, and the absent/null/present decode rule written inline against the literal key `"endpoints"`. A second kind is a hand-written parallel implementation. Builders must not go looking for the registry to register with; it does not exist, and this step does not build one either — two kinds is not enough evidence to design an abstraction, and inventing one here would be the third consumer's problem solved by guessing.

## 4. The seam that has to be cut first

**Nothing beneath the FPP command HTTP handler is callable.** `handleFPPCommand` is an unexported method on an unexported type, and every piece a macro executor needs — the primitive registry, the evidence resolver, the confirmation loop, the safety-class audit fallback, the idempotency-first ordering — is package-private inside `internal/coordinator/api`.

Two options exist and only one is right. Calling the coordinator's own HTTP API from inside the coordinator would duplicate authentication, CSRF and HTTP framing for a caller that is already in-process and already authorized, and would put the macro executor's correctness at the mercy of its own network stack.

**So: extract the dispatch, confirm and audit core into one exported in-process entry point, and reduce the HTTP handler to a wire adapter over it.**

The hard requirement on that refactor is that it changes no behaviour. Specifically it must preserve, because each of these is a Step 7 or Step 8 review finding that cost real time to find:

- **Idempotency lookup before any guard runs**, so a legitimate replay is not answered with a spurious `409`.
- **The three-way replay rule**: same key with same action, target and canonical params replays; different action or target is `409`; same action and target with different canonical params is a *different* `409`.
- **The `CollectedAt` fence** on every confirmation read, with `notBefore` set to the dispatch attempt, and no confirmation path reading the first matching row instead of going through `ResolveObservations`.
- **The safety-class audit rule**, exempt versus not-exempt, with the exemption applying to the two stops and nothing else.
- **`context.WithoutCancel` detachment**, so an abandoned client cannot abort an in-flight dispatch or its bookkeeping.

**The proof that the refactor is behaviour-preserving is that Step 8's existing tests pass unchanged**, including `make test-integration-fpp`. A builder who finds themselves editing an existing Step 8 test to make it pass has broken something and must stop and report it rather than adjust the test.

## 5. Configuration objects

### 5.1 Two new kinds, and the boundary with Track E

This step ships `show.action` and `show.macro`. [Track E](TRACK-E-show-authoring-and-assets.md)'s E1 lists Show, Surface, Action, Macro and the active-show pointer as its own deliverables; Track E has not started and Step 9 needs actions and macros now, so **Step 9 owns those two and Track E owns the rest.** BUILD-PLAN and Track E are updated to say so, so this is a division rather than a collision.

### 5.2 The show reference

[ADR-027](../decisions/ADR-027-show-and-surface-model.md) decision 2 makes a Show a namespace and requires surfaces, actions and macros to carry a show reference. Both new kinds carry a **required** `show` field, a non-empty identifier string, from the first revision.

It is required now and validated only for format, not for existence, because the Show object is Track E's and does not exist yet. **Referential validation is explicitly deferred**, which is the readiness concern ADR-029's consequences already name. The field is required rather than optional because adding a required identity field to a versioned configuration object later invalidates every existing revision, and defaulting one is the "absent is not empty" defect this project has now shipped three times.

### 5.3 `show.action` payload

The object id is the action identifier used by macro steps. One object per action.

```json
{
  "show": "halloween-2026",
  "label": "Projectors on",
  "description": "",
  "safetyClass": "none" | "blackout" | "stop" | "powerOff",
  "target": { "integration": "fpp" | "mqtt", "...": "integration-specific" }
}
```

`safetyClass` is **required** and drives §2.5's per-step audit exemption. The enum matches ADR-024 decision 11's three named actions exactly and adds no members. It is required rather than defaulted because a defaulted `none` on a power-off action silently makes the one thing decision 11 says is never refusable into something refusable.

**`integration: "fpp"`:**

```json
"target": {
  "integration": "fpp",
  "instanceId": "fpp-main",
  "primitive": "startPlaylist",
  "params": { "playlist": "Halloween Main", "ifBusy": "refuse" }
}
```

`primitive` must be one of the eight wire actions Step 8 registered, resolved through the existing registry rather than a second copy of the list. `params` is validated by the primitive's own `ValidateParams`, so an action authored with a bad playlist type fails at write time rather than at 17:00.

`safetyClass` **must agree with the primitive's own registered class** and the write is rejected if it does not. The two cannot be allowed to drift, and the registry is the authority.

`instanceId` **is validated at write time against the configured `fpp.endpoints`** and the write is rejected if the instance is not configured. See §5.6 for what that does and does not buy.

**`integration: "mqtt"`:**

```json
"target": {
  "integration": "mqtt",
  "broker": "home-automation",
  "publish": { "topic": "home/projectors/set", "payload": "ON", "qos": 1, "retain": false },
  "expect": {
    "kind": "none" | "boolean" | "number" | "text" | "match",
    "topic": "home/projectors/state",
    "value": "on",
    "deadlineSeconds": 30
  }
}
```

`broker` is **required, has no default, and is validated at write time** against the brokers the deployment declares, per §2.10. `safetyClass` on an MQTT action is operator-declared, because the coordinator has no way to know that `home/projectors/set` is a power-off.

**Absent, null and explicitly empty are three different things on every field of this payload too, with two stated exceptions, added 2026-08-14 during wave 2.**

The first is `description`, on both kinds. It is optional, and an absent key and an explicit `""` mean the same thing: no description. That is not the Step 7 erased-label defect wearing a new hat, and the difference is worth stating because the two look identical. **`declare` was a patch and this is a full replace.** A `PUT` writes a whole new immutable revision from the body it was given, so a body with no `description` is the operator saying this revision has none, not the operator declining to mention a field they expect to survive. The erased-label defect was a partial update treating an absent field as an instruction to clear an existing value, and there is no existing value to carry forward here.

The second is `publish.retain`, which is optional and absent means `false`; a present `null` is an error. §5.4's exception list names `onFailure` and `onUnconfirmed` and is scoped to the macro payload, so this is a third defaulted key in the step rather than a violation of that rule, and it is written down here rather than left as a builder's judgement. The reason it is safe to default and the wiped-endpoint and erased-label defects were not: **those two defaulted an absent key to a destructive value**, while `retain: false` is the non-sticky choice, so the operator who says nothing gets the publish that leaves nothing behind on someone else's broker. `qos` carries no default and is required, because there is no equivalent argument for which of 0, 1 and 2 the silent operator meant.

This is the one place a topic legitimately appears in configuration, and it is in the **action**, never the macro. ADR-029 decision 3 sanctions it as an explicitly advanced operation and BUILD-PLAN names it as the general integration point rather than a projector feature. Build it as the general one.

### 5.4 `show.macro` payload

```json
{
  "show": "halloween-2026",
  "label": "Begin set",
  "description": "",
  "steps": [
    {
      "id": "projectors",
      "action": "projectors-on",
      "localFallback": { "class": "coordinator-required", "reason": "..." }
    },
    {
      "id": "start",
      "action": "start-main-show",
      "onFailure": "continue",
      "localFallback": { "class": "none", "reason": "..." }
    }
  ]
}
```

**`localFallback` is per step, not per macro.** Revised 2026-08-14 after the specification review, which found the macro-level single-value field dropped two enum members that ADR-016 and RES-008 §10 both require by name.

`class` is a **closed validated enum** with three members, and an unlabelled step is rejected at write time:

- **`none`** — nothing runs locally if the coordinator refuses or is unreachable.
- **`coordinator-required`** — this step is executed by the coordinator and is unreachable exactly when the coordinator is. ADR-016 requires this label on any step touching a coordinator-hosted provider, and CLAUDE.md carries it as standing constraint 17. **Every external MQTT step in this design is one**, because the publish originates at the coordinator.
- **`silence`** — ADR-019's recorded exception, where the reduced local behaviour is deliberately nothing rather than a handover.

`reason` is required and must be non-empty on every class, including `none`.

**`reduced` is not an accepted value and must be rejected with a message saying no delivery path exists.** ADR-004 requires every critical macro to define a reduced local fallback; RES-008's re-survey established that the agent holds no cached fallback subset and ADR-008's v1 topic set carries no configuration-distribution topic, so a `reduced` fallback could be authored and could never reach a node. Under ADR-020 that is a field no code computes, and offering the value would let an operator believe they had configured a safety net that does not exist. **The limitation is made visible where an operator would look**: the API rejects it with a stated reason, the UI states it at the point of authoring, and each macro's own definition carries the reason in its own words.

The label is per step and not per macro for a second reason ADR-016 states directly: the remedy is deployment-dependent, so the same macro legitimately carries different labels in two installations. A macro-level field makes that inexpressible from revision 1, and adding an identity or label field later invalidates every existing revision, which is the same argument §5.2 correctly uses to make `show` required.

**Validation at write time, because a reference that resolves at 17:00 is a reference nobody checked:**

- `steps` must be non-empty and is capped at **32 steps**. The cap exists so §9's follow mode has a finite worst case; see §9.
- Step `id` must be unique within the macro and stable, because it is what a run's step records and the operator's own reading of a failure both key on.
- **`step.action` must resolve to an existing `show.action` object** and the write is rejected if it does not. A macro referencing a deleted action must fail at authoring, not at showtime.

**Absent, null and empty are three different things on every field here.** The `PUT` decode must distinguish them explicitly, per the rule Step 7 shipped a defect against twice in one step. The only exceptions are `onFailure` and `onUnconfirmed`, whose absences mean `abort` and `continue` respectively by §2.2, and both exceptions must be spelled out in the decoder rather than falling out of Go's zero value. A present `null` on either is an error.

### 5.5 Surface and revisions

**Four routes per kind, not three.** `fpp.endpoints` is a singleton and needs no list route; both of these kinds are collections, and the specification review found that §9's `showmeshctl macro list` and the UI's macro list had **no endpoint to call**. A client cannot enumerate object ids under an opaque `(kind, id)` store.

- `GET /api/v1/config/{kind}` — list. Returns object ids with label, show, and current revision number. Not the full payloads.
- `GET /api/v1/config/{kind}/{id}`
- `PUT /api/v1/config/{kind}/{id}`
- `GET /api/v1/config/{kind}/{id}/revisions`

Writes require `config:write` and are audited in the same transaction as the revision write, using `Identity.AuditedWrite` with the next revision number computed inside the closure.

**Reads do not copy `fpp.endpoints`' posture, and the review was right that copying it breaks the UI.** `config:write` is admin-only; `show:macro:run` is in the operator action scopes. Copying the posture gives an `operator` a role that can run a macro and cannot list one, so §9's macro list renders empty or `403` for the exact role the actual operator signs in as. `fpp.endpoints` is an admin-only deployment object; a macro is a run-time object with a non-admin consumer, and that difference is the whole point.

**So: reading `show.macro` and `show.action` requires `show:macro:run` or `config:write`.** A principal who may run a macro may read the macros they may run and the actions those macros compose. Writing still requires `config:write`.

### 5.6 What referential validation buys, and what it does not

§5.3 and §5.4 validate `instanceId`, `step.action` and `broker` at write time. RES-008 §10 item 6 is the reason, and it states the standard: "A macro referencing a node or device that no longer exists must fail at activation, not at 17:00. It must be answered because answering it wrongly is invisible until showtime."

> **CLOSED, 2026-08-14, by [ADR-036](../decisions/ADR-036-dispatch-configuration-applies-without-a-restart.md).** The paragraph below was written as a known gap and was measured as one during the acceptance run: the removal returned `200`, and the in-flight run's next step dispatched to the removed endpoint and confirmed, because the endpoint list was captured at process start. `fpp.endpoints` is now resolved per dispatch and the collector set is reconciled while the process runs, so the behaviour this section describes is what actually happens. Verified live: step 2 of an in-flight run resolved `failed` with `no FPP instance with id "bench-fpp" is configured`, with no restart.

**Write-time validation does not close the in-flight case and this specification does not pretend it does.** An operator can `PUT` `fpp.endpoints` removing `fpp-main` while a run sits at step 2 of 5. §6.1 pins the macro and action revisions; `fpp.endpoints` is a different configuration object with no pin. So step 3 dispatches against an instance that no longer exists.

> **SUPERSEDED IN PART, 2026-08-14, by [ADR-035](../decisions/ADR-035-a-run-always-runs-every-step.md).** The paragraph below ends by stating the old `onFailure` default and endorsing it. Under ADR-035 the default is `continue`: the step resolves `failed` exactly as described, and the later steps still dispatch, with the run reporting `completed: false` and naming this step. An author who wants a fleet-shape change to stop the sequence declares `onFailure: abort` on the step, which is the explicit per-step choice ADR-035 preserved. The `failed`-not-`skipped` distinction is unchanged.

**That resolves `failed`**, with a reason naming the instance and saying it was removed during the run. It is not `skipped`: something was declared, it is gone, and the operator needs to know the difference between a step that was deliberately passed over and a step whose target evaporated. Under §2.2's default that aborts the run, which is correct here, because continuing a show-start sequence against a fleet that changed shape mid-run is the case where stopping loudly is right.

`show` references stay unvalidated for existence, per §5.2, because the Show object is Track E's and does not exist yet.

## 6. The macro executor

New package, `internal/coordinator/macro`. It owns the run, not the wire.

### 6.1 Storage, schema v7

Two tables.

`macro_runs`: id, macro object id, **macro revision pinned at submission**, show, trigger (`api` | `plugin` | `cli` | `ui`), issuer principal id and name, idempotency key (unique), created_at, finished_at, state (`running` | `finished`), completed, confirmed, reason, attribution_degraded.

`macro_run_steps`: run id, step index, step id, action object id, **action revision pinned at submission**, integration, safety class, local fallback class, state, dispatched_at, resolved_at, outcome, outcome_state, outcome_reason, attribution_degraded, and the `commands.id` of the dispatched command for FPP steps.

**Revisions are pinned at submission and the whole run executes against them.** An operator editing a macro at 16:58 must not change what the 17:00 run does halfway through.

**Three things the review found missing from v7, all of which are cheaper now than later:**

**Retention, on both tables.** ADR-024 decision 11 established the principle for `audit_log`: an unbounded table that gates commands is a scheduled outage, and retention was required before the first write endpoint shipped. `macro_run_steps` grows at steps-per-run and the FPP plugin fires on every schedule entry, so both tables get a bound in the same migration that creates them, alongside the existing sweeps in `store/retention.go`. Not a follow-up.

**The `commands.id` reference is dangling by design and must be read as one.** `commands` is pruned by retention while `config_revisions` and `node_declarations` are not, so a run older than the command retention window points at a row that no longer exists. This is fine, and it is not fine to leave unstated: the run view renders the step's own recorded outcome with the command detail marked **not retained**, with a reason. It never renders blank and it never renders as though the step had no command. Absent evidence is stated, never omitted.

**Desired state and the command envelope's revision.** RES-008 §10 item 4 puts "what revision identifier travels with the invocation" on the settle-first list and it has not been settled anywhere. §4 requires the refactor to preserve behaviour, and the FPP primitives carry a `DesiredState` field, so **desired state will be written per step as an accident of the refactor** with `commands.requested_revision` left at its empty default. That is the pin in §6.1 going to the trouble of recording a revision that no investigator can reach from a command row.

**So: a step's dispatch writes desired state exactly as a single command does, and `requested_revision` carries the pinned macro revision**, formatted so a macro-issued command is distinguishable from an operator-issued one. The command journal must be able to answer "which macro revision caused this dispatch", because a configuration change at 16:58 is precisely what pinning exists for.

### 6.2 Step idempotency, and the run's own key

A step's idempotency key is derived deterministically from the run id and the step index. This makes reconciliation and any future retry safe by construction: a step that already dispatched cannot dispatch twice, because the key is already in `commands`.

**The run's key needs its own replay rule and the review was right that it had none.** §4 spends five bullets insisting the command-level three-way replay rule be preserved exactly, and then the new surface, which is where a double-fired schedule actually lands, had only a `UNIQUE` constraint. A unique-constraint violation is not a specified behaviour; it is whatever a builder maps it to.

The rule mirrors the command rule rather than inventing a second vocabulary:

- Same key, same macro id, same pinned macro revision: **replays**, returning the existing run and its current state, not a new run.
- Same key, different macro id: **`409`**, a distinct problem type.
- Same key, same macro, different pinned revision: **`409`**, its own problem type, because the macro was edited between the two submissions and the caller asked for two different things under one key.

Idempotency lookup runs **before** §2.6's overlap guard, so a legitimate replay of an in-flight run returns that run rather than a spurious `409` naming itself.

### 6.3 The nudge, which will bite

Step 8's post-dispatch poll nudge is rate limited to one accepted nudge per collector instance per two seconds, the limiter returns a bool that the command path deliberately ignores, and a refused nudge is invisible on the wire. Its own doc comment names this step as the reason the limit exists.

**The arithmetic, corrected 2026-08-14.** The first version of this section overstated the failure in one direction and understated the risk in the other, and both errors matter.

`lastNudgeAt` is updated only on an *accepted* nudge. So: step 1 nudges and confirms in about 0.55 s. Step 2 dispatches at about 0.6 s, inside the 2 s window, is refused, and waits for the ordinary 15 s tick, resolving at roughly 15.0 to 15.6 s. That matches Step 8's measured 15.010 s and 15.023 s, and it is **inside** the 20 s deadline with about 4.5 s of margin. Step 3 dispatches at about 15.6 s, well outside the window, so it **is** nudged and resolves in 0.55 s. The pattern alternates fast and slow.

**A four-step macro against one host is therefore about 32 s, not "about two seconds or about a minute", and it does not normally report `unconfirmed` at all.**

Getting that wrong cut both ways. Overstating the failure makes acceptance criterion 1 pass by luck rather than by the fix, which is exactly what that criterion exists to prevent. Understating the risk hides the real one: 15.6 s against a 20 s deadline is a 4.5 s margin, and it will not survive a slower host, a longer poll cadence, or a third collector source. Under the original §2.2 that margin was a margin on an **abort** rule. §2.2 has since been corrected so a blown margin no longer stops the show, which is the real fix; the executor still must not manufacture starvation it can avoid.

**The mechanism is not the builder's choice, and the two options the first version offered as equivalent are not equivalent.** Waiting for the limiter window before dispatching means the executor deliberately delays a show-affecting dispatch, potentially a stop, potentially a blackout, by up to 2 s per step so that its own telemetry arrives sooner. **That is monitoring impairing control**, which inverts the rule the limiter's own doc comment cites as its reason for existing.

**So: dispatch immediately, and adapt the waiting.** The limiter grows a reservation that tells the caller when a nudge will be accepted, and the executor uses that to schedule its confirmation read. Dispatch timing is never altered by telemetry considerations.

One caveat the verifying agent surfaced and a builder must answer: `Runner.Nudge` returns `false` for three distinct causes, unknown collector id, rate limit, and a channel-full coalesce, and **only the rate-limit branch is time-bounded**. A reservation API that treats all three alike will hand out a deadline it cannot honour. Distinguish them or return no reservation for the two that are not time-bounded.

**Correction, 2026-08-14, after wave 2 built against this section. The second half of it prescribes a mechanism the seam it targets has no room for, and the reservation wave 1 shipped for it has no consumer.**

`FPPCommandDispatcher.Dispatch` performs the whole cycle inside one blocking call: it nudges the collector once, then polls the observation store on its own interval until either fresh evidence resolves the command or the confirmation deadline elapses, and it returns only once that is over. The HTTP handler needs nothing else and neither does the executor. **There is therefore no second, executor-owned confirmation read for a reservation to schedule**, and re-invoking `Dispatch` under the same idempotency key replays the existing row rather than re-polling anything.

`NextNudgeAt` is consequently reachable from `collector.Runner` through the coordinator's wiring and into `FPPCommandDispatcher`, and **called by no production code at either end**. Its tests pass against fakes on both sides. That is this project's own recorded shape, a capability that compiles, is tested, and cannot be reached by anything, and it is recorded here rather than quietly left in place.

**What is actually true after wave 2**, and it is narrower than this section assumed:

- **Dispatch is never delayed**, which is the part that mattered and the part acceptance criterion 18 measures. The executor calls `Dispatch` first and consults nothing beforehand.
- **The starvation this section describes is real and unmitigated.** A step dispatched inside the limiter's 2 s window has its nudge refused and waits for the collector's ordinary tick, which this section's own arithmetic puts at roughly 15 s against a 20 s deadline.
- **The consequence is latency, not failure.** §2.2 as corrected means a blown margin marks the run `confirmed: false` with a reason and never stops the show. This is the second time that correction has turned out to be the thing carrying the design.

**The fix, if the margin proves too thin against real hardware, is one layer lower than this section looked.** `confirmFPPCommand` already ticks on its own poll interval, so re-nudging on each tick would let the second step's nudge land as soon as the limiter reopens, roughly 2 s rather than 15, and it would improve the single-command HTTP path by the same amount.

**Owner's decision, 2026-08-14: measure before changing anything.** The fix is not taken now, and `NextNudgeAt` is not deleted now either. Acceptance criterion 1 already requires per-step confirmation latency to be measured against the bench and reported as a number, so the evidence that decides this arrives in wave 3 at no extra cost. Changing shipped Step 8 behaviour ahead of that measurement would be the thing this project keeps writing down that it will not do: acting on a plausible reading of a system rather than on what the system did. **If criterion 1's numbers show the margin holding, this section closes as a recorded risk that did not materialize, and `NextNudgeAt` is then a deletion rather than a fix.**

### 6.4 Step execution

Steps run in order, one at a time. For each step: resolve the action at its pinned revision, dispatch through the §4 seam (FPP) or the §7 waiter (MQTT), record the outcome, then apply §2.2's **two** policy axes.

Step outcome vocabulary: `confirmed`, `unconfirmed`, `unconfirmable`, `failed`, `skipped`. `unconfirmable` is structural, meaning the step declared no expected response, and is distinct from `unconfirmed`, which means evidence was expected and did not arrive. Collapsing the two would make an honest "nothing can confirm this" indistinguishable from a real failure, which is the distinction ADR-029 decision 4 exists to preserve. Steps after an abort are `skipped`, which is not a failure.

How each outcome maps onto the run:

| Step outcome | Run `completed` | Run `confirmed` | Abort? |
|---|---|---|---|
| `confirmed` | unaffected | unaffected | no |
| `unconfirmable` | unaffected | set false, with reason | no |
| `unconfirmed` | unaffected | set false, with reason | only if `onUnconfirmed: abort` |
| `failed` | set false if it aborts | set false, with reason | yes unless `onFailure: continue` |
| `skipped` | already false | unaffected | n/a |

### 6.5 Reconciliation

Called at startup alongside `ReconcileStrandedFPPCommands`, before the server listens. Per §2.4.

### 6.6 The run surface, specified here rather than left to a builder

The specification review's sharpest process finding: for a step whose entire point is this surface, the run endpoints were the least specified part of the document, with the paths, submit status code, wire types and event kind all delegated to whichever builder took wave 2. They are settled here.

```
POST   /api/v1/macros/{id}/runs      -> 202, the run object, scope show:macro:run
GET    /api/v1/macro-runs            -> list, most recent first, filterable by macro id and state
GET    /api/v1/macro-runs/{runId}    -> one run with its steps
```

Reads on runs require `show:macro:run` or `config:write`, matching §5.5.

`202` on submit, not `200` and not `201`: the run is accepted and not complete, which is the honest status and the one §2.1 forces. The body is the full run object in its initial state, so a client that never watches still holds the run id and the pinned revisions.

**The change stream gains one additive event kind for run state transitions**, carrying the run id, macro id, state, the two booleans, and the reason. Step-level detail is fetched, not streamed, so a run with 32 steps does not put 32 events on a stream every client receives.

**In-flight runs must appear in `/api/v1/snapshot`, and ADR-020 decision 3 makes this fatal to omit.** The stream emits no `id:`, sequence numbers are per-connection, and every interruption forces an authoritative snapshot re-fetch. If in-flight runs are not in the snapshot then **a client reconnecting at 17:00:30 during a run started at 17:00:00 sees no run at all** and has no way to learn one exists. It then presses Run, receives §2.6's `409` naming a run it cannot display, and the operator concludes the system is stuck. The snapshot carries in-flight runs and a bounded window of recently finished ones.

## 7. The external MQTT command step

### 7.1 What exists

The coordinator cannot publish. `BrokerManager` wraps a full paho client and exposes no publish method. Subscriptions are a fixed slice passed at construction and re-sent whole on every reconnect; there is no runtime subscribe. Delivery is one process-wide callback with the single slot already claimed by `inventory.HandleMessage`. `internal/agent/mqtt.go`'s `Publish(ctx, topic, qos, retain, payload)` is the closest existing shape to copy.

**Four things beyond "all three need work", each of which the review found unaddressed and each of which is a design rather than a detail:**

**There is no runtime *unsubscribe* either**, and §7.2's ordered rules never mentioned tearing a subscription down. Without one, subscriptions accumulate for the process lifetime. `subscribeAll` re-sends the whole set on every reconnect, so the set must become mutable and reconnect-safe, which the current shape is not. A waiter subscribes on entry and unsubscribes on exit, including on deadline expiry and on run abort, and the reconnect path must send the set as it stands at reconnect time rather than the construction-time slice.

**Multiplexing the single callback slot is the thing most likely to be done wrong under time pressure.** One process-wide callback must serve `inventory.HandleMessage` and N concurrent waiters. Route by topic to a registry of interested parties; inventory keeps its existing delivery unchanged and must not be able to lose a message because a waiter is registered.

**Two concurrent runs waiting on the same response topic have no correlation**, because `expect` names a topic and a value, not an instance. Two runs of two macros both waiting on `home/projectors/state` will both see the same delivery. That is **correct and must be stated as correct**: the external system reported a state, and both waiters legitimately observed it. What must not happen is one waiter consuming the delivery and starving the other. Delivery is fan-out to every registered waiter on that topic, each applying its own contract and its own deadline independently.

**Per §2.10 the waiter operates against a named broker**, so the publish and the subscribe must both resolve through the same broker identifier from the action. A waiter that subscribes on one broker and publishes on another is the silent failure §2.10 exists to prevent, and it is worth an explicit test rather than an explicit comment.

### 7.2 The trap, and it is the sharpest thing in this step

**A retained message must never confirm a step.**

Home Assistant and Node-RED retain state topics as a matter of course. A step that publishes `ON` and waits on `home/projectors/state` will, on subscribing, immediately receive last night's retained `on` — and confirm, in milliseconds, having observed nothing at all.

That is Step 7's 179-microsecond defect in a fourth subsystem, and unlike the previous three it is not merely possible but close to certain, because retaining state topics is the correct thing for a home-automation broker to do.

So the waiter must, in this order:

1. **Subscribe first**, before publishing, or a fast responder's answer is lost.
2. **Discard every delivery whose RETAIN flag is set.** `broker.Message` already carries `Retained` for exactly this class of reason.
3. **Start the deadline at the publish**, not at the subscribe.
4. Accept only a live delivery arriving after the publish.

**Rule 2's justification, corrected 2026-08-14, and the correction matters more than the rule.** The first version said the existing subscription options "deliberately preserve the flag by setting `RetainAsPublished: false`", which is backwards, and a builder following the stated reason breaks every MQTT step.

`RetainAsPublished: false` does not preserve the flag. Per MQTT 5 §3.3.1.3, with RAP off the broker **forces RETAIN=0** on forwarded live messages regardless of how the publisher set it, so RETAIN=1 arrives only from the retained store at subscribe time. **RAP=false preserves the distinction, not the flag**, and the distinction is the entire mechanism rule 2 depends on. The code's own comment at `internal/coordinator/broker/broker.go` says this correctly.

So the rule reads: **RAP stays `false` so that RETAIN=1 means, and only means, a replay from the broker's retained store.** The client is `eclipse/paho.golang` on MQTT 5, so RAP is a real settable option and this is not theoretical. A builder who reasons that RAP=`true` "preserves the flag better" makes every live answer from a responder that retains its state topic arrive with RETAIN=1, rule 2 discards all of them, and every projector step times out. §11 gains an acceptance criterion for the mirror case, because criterion 4 alone does not catch it.

### 7.3 The response contract

- **`none`** — no subscription, no waiter. The step resolves `unconfirmable` with a reason saying the action declares no expected response. Never `confirmed`, never a silent success.
- **`boolean`** — payload must parse as a JSON boolean. `true` confirms. `false` is a *negative answer* and fails the step with a reason saying the external system reported failure, which is different from silence and must read differently.
- **`number`** — payload must parse as a JSON number. Confirms on receipt; an optional `value` requires equality.
- **`text`** — payload must be valid UTF-8. Confirms on receipt; the payload is recorded, bounded.
- **`match`** — payload must equal `value` exactly. Anything else is a negative answer, not silence.

Deadline expiry is `unconfirmed` with a reason naming the topic and the deadline. A malformed payload under a typed contract is `failed`, not `unconfirmed`: something answered and it was not what the operator declared.

`deadlineSeconds` is operator-authored, so it is bounded rather than left unbounded, and the review found the first version said it "needs a stated maximum" and then did not state one, leaving an undefined term in §9's derivation. **The maximum is 120 seconds**, validated at write time. A projector that has not answered in two minutes is not going to, and a longer wait is a step that holds a run open past any useful operator response.

### 7.4 The exemption, recorded

Step 8's rule is that a command ships only if its effect is observable through a signal the collector already collects. This step type is exempt: the operator supplies the observation. BUILD-PLAN records the exemption and this specification repeats it so it is not later read as an oversight.

## 8. ADR-024 decision 7, discharged

Decision 7 has three clauses and each gets a named implementation. **A discharge that satisfies two of them is not a discharge.**

### 8.1 The macro definition specifies behaviour for a refusal

§5.4's required per-step `localFallback`, now a three-member closed enum rather than the single value the first version shipped.

**The review found a hole here and it has to be said plainly: the plugin cannot read the definition, and it never will be able to.** The plugin holds a `scheduler` credential, `scheduler` holds `show:macro:run` and nothing else, and §5.5 puts macro reads behind `show:macro:run` precisely so that a runner can read what it runs. So the plugin **can** read the macro it fires, and that is a deliberate consequence of the §5.5 scope fix rather than a happy accident.

What the plugin does with it on a refusal is bounded, because on a `401` or `403` there is no authenticated read either. **So the plugin's refusal behaviour has two parts:**

- **It caches the macro's step labels and fallback classes on its last successful authenticated fetch**, so at 17:00 on a refusal it can state what the definition said should happen locally rather than asserting a hardcoded constant.
- **With no cache it says so**, naming the macro and stating that its local policy is unknown. It does not substitute a default and present it as the definition's answer. An unknown policy stated as unknown is the honest failure; a hardcoded "nothing runs locally" presented as the operator's own choice is the field-with-no-consumer shape ADR-020 forbids, arriving from the other side.

For day-0 every step states `none` or `coordinator-required` with a reason in the operator's own words, so the practical answer is that nothing runs locally. The point is that it is written in the definition and read from it, rather than being a constant that happens to match.

### 8.2 The plugin treats `401` and `403` as coordinator-unavailable-to-this-caller

The plugin classifies every attempt into four classes, and they must be distinguishable in its own records, not collapsed into success and failure:

| Class | Trigger | Meaning |
|---|---|---|
| `ok` | `2xx` | The run was accepted. |
| `refused` | `401`, `403` | A healthy coordinator refused *this caller*. Fires the local policy, which per §8.1 is "nothing runs locally", recorded and reported. |
| `rejected` | other `4xx` | The coordinator answered about the *request* — unknown macro, already running. Not a credential problem and must not be reported as one. |
| `unreachable` | transport failure, or `5xx` | The coordinator could not serve. The genuine ADR-004 outage condition. The status, where there is one, is recorded. |

`rejected` exists because folding a `404 unknown macro` into `refused` would send the operator to rotate a credential over a typo, which is decision 7's own stated harm — a credential fault presenting as a network fault sends the operator to the wrong place at the worst time — running in the other direction.

### 8.3 It is distinguishable in evidence

Three paths, revised 2026-08-14. The first version had two and the review showed that neither works in the scenario ADR-024 decision 7 was actually written around.

**The argument that was wrong, recorded because it is the interesting part.** The first version said a refusal leaves a coordinator-side mark while "a network fault produces no record at all", and that **"that asymmetry is itself the distinction."** It is not. That is inferring a fault from absence of evidence, and it fails because silence has at least six causes the operator cannot tell apart at 17:00: network fault, FPP host down, plugin not installed, plugin crashed, schedule entry deleted or mis-timed, or the operator watching the wrong screen. The section's own next sentence set the correct standard, that the surface must present the mark rather than requiring the operator to infer it from absence, and then supplied a mark for the refusal and no mark for the outage.

**And the buffered path is circular in exactly decision 7's scenario.** ADR-024's case is an operator rotating the `scheduler` token in November and missing the FPP host. The plugin buffers its `refused` record and attaches it to "the next successful authenticated call". With a stale credential **there is never a next successful call.** The record that would tell the operator their credential is wrong is trapped behind the credential being wrong, and it flushes only after they have already diagnosed and fixed the thing it exists to diagnose.

So:

**1. Plugin side, local and immediate. This is the one that carries the discharge.** At 17:00 the FPP host is the only thing that knows what happened, so it must say so where it stands, without needing the coordinator: a status file the FPP UI can show, and a log line, written on every attempt, carrying the §8.2 class, the HTTP status where there is one, the macro id, and the timestamp. **A `403` and a closed port must produce visibly different local records**, and neither depends on any subsequent successful call. This is what makes clause 3 true.

**2. Plugin side, buffered, per ADR-004's own consequences**, which require local fallback to "report that degradation when connectivity returns". The plugin buffers degraded outcomes and attaches them to the next successful authenticated call, in an additive `priorFailures` array on the run request body. The buffer is bounded in count and age, drops oldest first, and **records the fact that it dropped**, because a silently truncated failure history is a smaller version of the same lie. This is now a convenience that consolidates history centrally, not the mechanism the discharge rests on.

**The flush rule needs stating, because the review flagged it as unspecified and a builder will get it wrong.** The buffer flushes on `2xx` only. A `409` under §2.6 and a `404` for an unknown macro are both `rejected` under §8.2, and a `404` handler that discards the body before anything reads `priorFailures` would drop the buffer's contents while clearing it. On any non-`2xx` the buffer is retained unflushed.

**3. Coordinator side, and it is nearly free.** A refusal at the macro-run endpoint is a successful conversation with a healthy coordinator, so the coordinator is up and can record it. It is audited and surfaced as an operator-visible event.

**That coordinator-side write is bounded, because unbounded it is an eviction primitive.** LESSONS records the shape: an unbounded write on a failure path evicts the evidence it exists to preserve. Events are under retention, so an FPP host with a bad credential firing every few minutes for a week would flush the event history with its own complaints. Refusal events from one host for one macro are **coalesced with a count and a first-seen and last-seen time**, rather than appended one per attempt. The same bound applies to the coordinator's acceptance of `priorFailures`.

**No new MQTT topic and no second broker credential.** Both were considered. The first version declined a topic on the grounds that "the next-successful-call mechanism already covers" this reporting path, and that premise was exactly what the review showed to be false for the refusal case, so the argument is replaced rather than kept: path 1 covers it locally and immediately without needing a broker at all, and a topic would add a second credential on a host RES-015 §7.4 shows cannot keep a secret. **If path 1 proves insufficient in practice, an MQTT topic is the right next move and deserves a real argument at that point**, not a recycled one.

## 9. Clients

**`showmeshctl`** gains macro subcommands: list, show, run, and read a run, all against the routes §5.5 and §6.6 now specify. Unchanged rules: it may not import a coordinator package, the import-graph test enforces it, and its own client timeout floor must be derived independently and reconciled with the server's by an integration test rather than by a shared constant.

**Follow mode takes an idle timeout, not a total one, and the first version had this backwards.** It said the follow timeout "must exceed the longest a run can legitimately take", which was unsatisfiable as written: `steps` had no cap and §7.3 stated no maximum, so the longest legitimate run was unbounded and no finite timeout satisfied the requirement. §5.4 now caps steps at 32 and §7.3 caps `deadlineSeconds` at 120, so a bound exists, but a **total** timeout is still the wrong shape. It reintroduces exactly the coupling §2.1 removed, which is a client waiting on a long server-side operation, and that is Step 7's defect wearing a new hat.

So follow mode times out on **silence**, not on duration: no run event and no successful poll within the idle window. On idle timeout it exits **cleanly**, printing the run id and that the run is still in progress, and it never reports anything that reads as a transport failure. A run that legitimately takes six minutes is followed for six minutes as long as it keeps saying something.

**The Operator UI** gains a macro list, a run control gated on `show:macro:run` through the existing `ScopedButton`, and a run view. A control the principal may not use renders disabled with a stated reason and never hidden; a stale scope list renders `unknown` and never permissive. §2.3's two booleans must be visually distinct — a run that completed without confirmation must not look like a run that confirmed.

**`api/openapi.yaml`** grows additively, stays conformance-tested in both directions, and the UI types are regenerated with the CI diff check green.

## 10. The FPP plugin

Per RES-015, and the record is dense with traps that have already cost someone else time. Builders must read RES-015 §5 and §7 in full. The non-negotiables:

- **The Step 9 macro path is a script command registered through `commands/descriptions.json`**, not a C++ macro implementation. **Superseded in scope 2026-08-18:** the complete FPP package will also contain a separate C++ brightness component. The Go macro helper remains a forked process; the planned C++ component will use isolated FPP 9 and FPP 10 adapters and be compiled locally against installed headers. See [RES-018](../research/RES-018-fpp-brightness-control.md).
- **A prebuilt static Go binary fetched by `scripts/fpp_install.sh` and SHA-256 verified against a hash committed in `fpp-showmesh/artifacts.lock.json`**, because FPP hosts carry no Go toolchain and the dominant build-on-device model is closed to us by construction. A checksum fetched from the same mutable release location is not the trust anchor. Verification also clears the registry's `unverified-package-install` finding, which would block a first listing.
- **The credential is read from a mode-0600 file the plugin owns, and is never a command argument.** FPP publishes every command execution with its arguments in cleartext to MQTT `command/run`, and the fleet publishes to the operator's live home-automation broker. A token in an argument or a URL query string is broadcast on every invocation. RES-015 is explicit that FPP's own exposure is upstream's problem and out of scope, while **ShowMesh putting a secret somewhere FPP will expose it is ShowMesh's defect.** The macro id is not a secret and may be an argument.
- **Never select an artifact with `uname -m`** — a Pi boots a 64-bit kernel under a 32-bit userspace. Use the ELF class of an FPP binary and the aarch64 dynamic-linker probe, and confirm they agree, which is the pattern two unrelated plugins converged on independently.
- Read `FPPDIR` from the environment first, then strip a positional `FPPDIR=` prefix, then fall back to `/opt/fpp`; the earlier environment-stripping claim was corrected in RES-015 §5.1. The working directory is the plugin *parent*, not the plugin directory; scripts are committed mode 0755 or they are silently skipped; a cheaply guarded `preStart.sh` repair check is a no-op in the common case; `fpp_uninstall.sh` is mandatory; `fpp_upgrade.sh` is additive; no `curl | bash`; no `systemctl restart fppd`; call `http://localhost/api/...` rather than fppd's port; `pluginInfo.json` is strict JSON with entries for FPP 9.4–9.x and FPP 10.x. **The former “no Makefile” rule applied to the prebuilt Go-only package and is superseded for the locally compiled C++ brightness component.**
- `eventCallback` is dead code called by nothing. If a callback is ever needed it is `playlistCallback`, not `MultiSyncPlugin`, which sees nothing on a host that is not a configured MultiSync master.
- The execve environment carries no `PATH`. Absolute paths only.
- **CI must build `GOARCH=arm GOARM=7`**, which it does not today, or the plugin cannot reach the BeagleBone Black in the deployed fleet.
- **The release-artifact pipeline is a deliverable and did not appear in the first version's build order.** `showmesh-fpp-plugin` must produce the prebuilt Go binaries and C++ source bundle; `fpp-showmesh` must commit their exact filenames and hashes before installation. A bench or first real-host install uses local/private candidate assets because public publication remains gated on that real-host run. The original same-release binary/checksum wording is superseded by RES-018's committed-lock contract.
- **The plugin writes a local status record on every attempt**, per §8.3 path 1. This is not optional and it is not a log line only; it is what discharges decision 7's third clause.

**Bench only, and the limits are stated rather than discovered.** Developed against `bench/fpp-multisync`'s containerized `fppd` by the owner's decision. That licenses the plugin mechanism, the callback boundary, refusal semantics and decision 7's behaviour. It does not license the on-host install path, filesystem permissions, packaging or cross-version compatibility. **RES-015 stays at L1 and this step does not raise it**, which is a stated deferral carried into whichever step first installs on real hardware.

**Accepted risk, recorded and not to be re-litigated.** The plugin credential holds `fpp:command` in full on a host that cannot keep a secret. Target scoping was declined by the owner in Step 8. Single operator, owned hardware, isolated network.

## 11. Acceptance criteria

Every one is proved against a running coordinator and the bench `fppd`, not against the test suite. Step 8's worst defect — a `nextPlaylistItem` that reported confirmed while doing nothing — was found by running it and by no test.

1. A macro of at least four steps against one bench FPP host runs end to end, and **per-step confirmation latency is measured and reported**. No step reports `unconfirmed` for want of a poll. This is §6.3 and it is the criterion most likely to pass by luck, so the number is the evidence, not the green tick.
2. **REVISED 2026-08-14 (ADR-035).** A step that fails does **not** abort the remainder: every later step still dispatches, nothing reads `skipped`, and the run reports `completed: false` naming the failed step. A step declaring `onFailure: abort` explicitly still aborts, and the steps after it then read `skipped` and not `failed`, which is the distinction the original criterion existed to protect.
3. A step declaring `onFailure: continue` does not abort, and the run reports `confirmed: false` with a reason. `completed` is `false` too, because a step failed; those two conditions were the same flag while a failure always aborted, and ADR-035 decision 3 separates them.
4. **A macro with an MQTT step whose response topic carries a retained message does not confirm off it.** Set the retained value to the expected one first, so that a broken implementation confirms instantly and a correct one waits. Measure the time from publish to resolution; a confirmation faster than the responder could have answered is the defect.
5. An MQTT step with `expect.kind: none` reports `unconfirmable` with a reason, on every run, and never `confirmed`.
6. A second run of a macro already running is refused `409` naming the in-flight run.
7. A run interrupted by a coordinator restart is finished `completed: false` on the next start, its remaining steps never dispatch, and no command row is left unresolved.
8. **The plugin distinguishes all four classes in §8.2 against real responses**: a `403` from the real coordinator by using a token without `show:macro:run`, a `404` for an unknown macro, and a transport failure by pointing it at a closed port. **A `403` and a closed port must produce visibly different records in the plugin's own local status record**, read directly on the host with the coordinator unreachable, so the evidence does not depend on any later successful call.
9. A degraded outcome buffered by the plugin arrives on the next successful call and appears as an operator-visible event. A `404` and a `409` do **not** flush the buffer, and the buffered entries survive to the next `2xx`.
10. `reduced` is rejected as a `localFallback` class with a message stating no delivery path exists. `coordinator-required` and `silence` are accepted, and a step with no `localFallback` is rejected.
11. **REVISED 2026-08-14 (ADR-035 decision 1). A run never withholds a command for an audit failure.** With `audit_log` genuinely unwritable, a macro of `[stopPlaylist, startPlaylist]` runs **both** steps, each carrying `attributionDegraded: true`, raised onto the run; nothing is refused and no `503` is returned at submission or mid-run. Proved against a real unwritable `audit_log` (a SQLite `RAISE(ABORT)` trigger on a live database), as the Step 7 fix was. The safety class is deliberately still mixed in the fixture, because a run that passed only when every step was exempt is exactly the behaviour being removed.
12. Step 8's existing tests and `make test-integration-fpp` pass unchanged across the §4 refactor.
13. The collector's read-only posture is unchanged: the request census over a full macro run shows no unexpected non-GET against FPP beyond the primitives' own `POST /api/command`.
14. **An `unconfirmed` step does not abort by default.** Force it by stopping the collector for the duration of one step. The run continues, dispatches its remaining steps, and reports `completed: true, confirmed: false` naming the step. With `onUnconfirmed: abort` on that step, the same scenario aborts. This is §2.2's correction and it is the single most consequential change the review produced.
15. **The RAP mirror case.** A responder publishing its live answer **with retain=true** still confirms the step. Criterion 4 alone passes under a broken `RetainAsPublished: true`, so this criterion is what actually pins §7.2 rule 2.
16. **Snapshot completeness.** A client that connects for the first time during an in-flight run sees that run in `/api/v1/snapshot`, with no prior stream history. Verified by starting a run, then starting a fresh client.
17. **Run-level idempotency.** The same key replayed returns the same run and does not start a second one; the same key against a different macro is `409`; the same key against an edited macro is a distinguishable `409`. A replay of an in-flight run returns that run rather than the overlap `409` from criterion 6.
18. **The nudge reservation does not delay dispatch.** Measure the interval between the executor deciding to dispatch a step and the command leaving, across a four-step same-host macro. It does not grow with the limiter window. Per §6.3, the confirmation read adapts and the dispatch does not.
19. **Broker routing.** An MQTT action naming broker A publishes on A and waits on A, proved with two bench brokers where B would also have accepted the publish. An action with no `broker` is rejected at write time.
20. **Write-time referential validation, and live endpoint resolution (ADR-036).** An action naming an unconfigured `instanceId`, a macro naming a nonexistent action, and an action naming an undeclared broker are each rejected at `PUT`. Separately, removing an endpoint mid-run resolves the affected step `failed` with a reason naming the instance, per §5.6, **with no restart**. The addition direction is part of this criterion and not optional: an endpoint added through the API must become both dispatchable and polled without a restart, because live resolution alone would make it dispatchable while nothing polled it and every command against it would fail to confirm.
21. **An `operator`-role principal can list, read and run a macro** through both clients. This is the §5.5 scope fix and it is the criterion that catches the UI rendering an empty list for the role the actual operator signs in as.
22. **The run retention bound is exercised**: `macro_runs` and `macro_run_steps` are pruned, and a run whose `commands` row has been pruned renders with the command detail marked not retained rather than blank.

## 12. Out of scope, stated rather than omitted

- **Compensation and undo** (§2.2).
- **Reduced local fallback delivery** — the ADR-025 agent cache with its pinned verifying key, and an ADR-008 topic to carry it. Cut from day-0 by the owner; revisit after Halloween.
- **The raw protocol escape hatch** (§2.8).
- **The Show object, the active-show pointer, surfaces, and referential validation of `show` references** — Track E.
- **Step types for Resolume, audio and render nodes** — Tracks B, C and D. The vocabulary grows as those tracks land, which is BUILD-PLAN's stated handling of the dependency that would otherwise make this step block on everything.
- **On-host plugin install, packaging, permissions and cross-version compatibility.** RES-015 stays L1 (§10).
- **Target-scoped authorization**, declined by the owner in Step 8 rather than deferred. ADR-024's supersession trigger fires in this step and is deliberately not taken.
- **A generic configuration-kind registry** (§3).

## 13. Build order

Waves, with disjoint files inside each. Step 7 fanned out three "parallel" seams that all touched one migration file and one route table; this ordering exists to not repeat that.

**Wave 1**, three parallel builders, no shared files. Schema v7 and the run store, with retention, in `internal/coordinator/store`. The §4 dispatch seam refactor in `internal/coordinator/api/fppcommand_*`. MQTT publish, runtime subscribe and unsubscribe, callback multiplexing, and the response waiter in `internal/coordinator/broker`.

**Wave 2**, the macro layer. Config kinds in `internal/coordinator/config` and the executor in `internal/coordinator/macro` in parallel, both avoiding the route table. Then **one builder alone** owns the API surface: the routes §5.5 and §6.6 specify, `v1` wire types, `api/openapi.yaml`, the snapshot addition, and the change stream's new event kind.

**Wave 3**, three parallel builders: `showmeshctl`, `ui/`, and the plugin with its build targets and its release-artifact pipeline.

**Scope, decided 2026-08-14 after the review recommended a cut.** The review's recommended cut was the whole external MQTT step, on the grounds that it is the only thing forcing a new broker subsystem, that it carries three of the sixteen findings, and that day-0 loses nothing because FPP fires those MQTT commands directly today. **The owner declined the cut and kept it in Step 9**, under schedule pressure toward a mid-September day-0: deferring it collides with work already written, and the subsystem is wanted rather than tolerated. The three findings it carried are therefore **fixed in this document rather than deferred with it**, at §2.10, §7.1 and §7.2. Builders should know the cut was considered and declined on the record, so that "we could just drop this" is a decision already made rather than one to remake at 2am.

**The second recommended cut was also declined, 2026-08-14, after wave 2 landed.** The reviewer recommended dropping the `show.action` UI authoring surface from wave 3, leaving the Operator UI with list, run and run view, on the grounds that [ADR-030](../decisions/ADR-030-operator-ui-is-the-authoring-surface.md) already makes `showmeshctl` the required authoring path. **The owner kept it.** So wave 3's UI ships authoring for both kinds, and the schedule pressure the reviewer was answering is real and now lands somewhere else. That makes the paragraph below binding rather than advisory: with wave 3 the larger of the two options, criteria 8 and 9 are the thing that must not absorb the difference.

**What gets protected instead.** The review's other observation stands: acceptance criteria 8 and 9, the plugin's decision-7 behaviour, are last in the build order, are the only cross-compiled artifact, need a release pipeline that does not exist, and are the obligation that has **already slipped from Step 7 and Step 8**. A plugin that installs, fires a macro and gets a `200` will look complete while discharging nothing. **If anything in this step is trimmed under time pressure, it is not criteria 8 and 9.**

**The copy guards are extended in the wave that adds the files they must cover.** Both guard tests currently walk hardcoded file lists that do not glob, so a new file is unchecked by default. **Invert them**: walk the directory and carry an explicit exemption list, so the default for a new operator-facing file is checked. This step adds a great deal of operator-facing text and the current shape would silently not check any of it.

**Exempt at the string or function level, never the file level.** The Go guard's pattern matches on `docs/`, `.md`, `ADR-\d+`, `RES-\d{3}` and `section \d`, which will trip on internal log and error strings once it walks a whole directory. A file-level exemption to quiet that also removes coverage of every genuinely operator-facing string in the same file, which turns the inversion into a net loss exactly where the text density is highest.

## 14. Standing rules that apply here

- No write, no command, no restart, no settings change and no MQTT publish against the deployed fleet or the operator's live broker. Everything is the bench `fppd` and a bench broker.
- Unit tests never raise a research record above L1.
- Never write a doc comment, log line or document claiming verification that has not happened. Step 8 shipped two comments claiming tests that did not exist, both of them the fix for an earlier defect.
- A test's name is a claim: before trusting one, break the behaviour it names and confirm it fails.
- Operator-facing strings carry no repo path, `.md` reference, ADR number or section citation. What the operator reads and what a maintainer needs to know are different documents.
