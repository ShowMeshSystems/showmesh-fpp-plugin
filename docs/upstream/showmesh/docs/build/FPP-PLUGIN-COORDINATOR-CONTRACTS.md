# FPP plugin coordinator contracts

> **ADR-048 amendment, 2026-08-30.** Sections 1 through 4 remain the normal
> coordinator-facing contract.  The plugin has no general execution authority
> under those sections.  [Track J](TRACK-J-fpp-fallback.md) adds a separately
> versioned signed fallback-program contract: it maps the existing deterministic
> entry key to an already authorized Cue and named node targets during confirmed
> coordinator loss.  Its schema is frozen by J1 before plugin or node work
> begins; it does not widen any existing observation route into a command route.

[RES-018](../research/RES-018-fpp-brightness-control.md) · [ADR-043](../decisions/ADR-043-show-scoped-cues-and-playlist-authority.md) · [ADR-024](../decisions/ADR-024-identity-authorization-and-audit.md) · [Track F](TRACK-F-resting-mode.md) · [Track H](TRACK-H-cues-and-playlists.md) · [SM-63 handoff](SM-63-FPP-PLUGIN-HANDOFF.md)

Status: frozen 2026-08-21, extended 2026-08-22, corrected 2026-08-23,
corrected 2026-08-26. This record fixes the wire contracts the ShowMesh FPP
plugin runtime shares with the coordinator: playlist-entry observation
ingestion, the coordinator-owned brightness transition gain, and playlist
definition publication. RES-018 decided the design; this file fixes the
exact bytes so the plugin and the coordinator can be built independently and
still meet.

**2026-09-08 amendment:** §1.2 gains an optional `playlistLoop`, and §1.8 is
new. FPP 10 never delivers the playlist `start` callback action to a plugin
(`Playlist::PlayImpl()` sets status to PLAYING before `Start()` reads it, so its
fresh-start test always evaluates to "playing"; verified against the pinned FPP
10.0 source at `370e62ed7e8c8318da6ee5b01312b8b75082d952`). Without `start`, a
playlist looping back into an entry could not be told from its first visit,
because a loop's second visit derives the identical entry key, so a repeating
playlist stopped re-firing its Cue. `playlistLoop` carries FPP's own pass
counter, which both majors already hand the plugin, and which the plugin
previously discarded. Per owner ruling (2026-09-08): the fleet is moving to FPP
10, so this is fixed rather than accepted as a limitation. Entry identity is
unchanged; see §1.8 for why the `start` term stays and why the upgrade order
matters.

**2026-08-26 correction:** sections 1.2 and 1.3 left the canonical spelling
of `section` implicit — described as "FPP playlist section" with no fixed
vocabulary. Two independently correct implementations each read that as a
different, defensible spelling: the plugin hashed FPP's own runtime section
string (`LeadIn`, `MainPlaylist`, `LeadOut`, `New`), the coordinator derived
from the playlist definition's JSON member names (`leadIn`, `mainPlaylist`,
`leadOut`). The shared fixtures could not catch it, because `entry-key.json`
supplies an already-canonical `section` and checks only derivation. Section
1.2 now names the canonical spelling explicitly and gives the mapping table
from FPP's runtime strings; section 1.3 states that `entryKey`'s `section`
input is that same canonical value. `test/fixtures/fpp/section-mapping.json`
pins the mapping itself. See [TRACK-H-CHAIN](../bench/TRACK-H-CHAIN.md) for
the failure this closes; the plugin fix that shipped first is SM-275, and
this correction is SM-278.

**2026-08-23 correction:** section 3.1 previously claimed the plugin "has no
HTTP server and deliberately registers no route" and cited ADR-013 for that
claim. That contradicted section 2.2, which has always required the plugin to
serve an inbound brightness route, and ADR-013 is about UDP 32320 MultiSync
port sharing, not HTTP routing; it says nothing that forbids a route. Per
owner ruling (2026-08-23): the plugin opens no listening socket of its own,
but it may register narrow, idempotent, evidence-returning routes on fppd's
own web server, and section 2.2's brightness route is specified to be
unauthenticated when it is built, matching the unauthenticated-by-default
posture SECURITY.md and RES-015 §7.4 already record for fppd's own web UI and
API. A bearer credential is deferred to a future season as a separate tracked
item. See sections 2.2, 2.3, and 3.1.

Nothing here has run against a real FPP host. The contract is verified by unit
tests and by the shared fixtures in
[`test/fixtures/fpp/`](../../test/fixtures/fpp/README.md), on both sides.

**Every numbered section below carries a build status on its own heading, and
that status is the only thing in this document a reader may treat as a claim
about what exists.** A frozen shape and a shipped behavior read identically in
prose, so prose here describes the CONTRACT and never implies an
implementation. A section that says what the plugin "does" is saying what this
contract requires of it, not reporting what is deployed. Keep the status lines
true when either side ships.

## 1. Playlist-entry observation ingestion

**Status: SHIPPED.** Both sides are built.

### 1.1 Endpoint and authorization

```text
POST /api/v1/integrations/fpp/playlist-entry-observations
```

Guarded by the `fpp:observe` scope. The installed plugin principal holds
`show:macro:run` and `fpp:observe`; the `scheduler` role is that principal and
its bundle grows by exactly this one scope. `fpp:command` and the human
operator scopes are not prerequisites, and `fpp:observe` is deliberately not in
the `operator` bundle: an operator credential must not be able to forge plugin
evidence.

The request authenticates as a bearer token, so the ADR-024 decision 6 CSRF
header requirement does not apply to it. Authentication and the scope check run
**before** the body is parsed.

Reads of the ingested state are open under `observation:read`, matching every
other FPP read surface:

```text
GET /api/v1/integrations/fpp/playlist-entry-observations
```

It returns the latest accepted observation for every known instance. It exists
so a client that sees the change-stream event has an authoritative state to
re-fetch, per ADR-020's non-resumable stream rule.

### 1.2 Request body, schema version 1

Content type `application/json`. The body is bounded at **16384 bytes**; a
larger body is refused with `413` before it is parsed. The complete playlist
definition never travels in this body, only its hash, so the bound is
generous for every legitimate observation.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `schemaVersion` | integer | yes | Currently `1`. Any other value is refused. |
| `instanceUuid` | string | yes | Persistent FPP UUID, from FPP's `SystemUUID` setting. |
| `playlistName` | string | when available | FPP playlist name. |
| `playlistHash` | string | when available | SHA-256, lowercase hex. See §1.3. |
| `section` | string | when available | Canonical section: the playlist definition's member name (`leadIn`, `mainPlaylist`, `leadOut`) for those three, or FPP's own runtime string unchanged for any other value. May be empty. See below. |
| `position` | integer | when available | Zero-based position within the section. |
| `entryKey` | string | when available | SHA-256, lowercase hex. See §1.3. |
| `sequenceFilename` | string | no | Absent when the entry has none. |
| `mediaFilename` | string | no | Absent when the entry has none. |
| `action` | string | yes | One of `start`, `playing`, `stop`, `query_next`, `unknown`. |
| `sequence` | integer | yes | Monotonic per-instance event sequence. See §1.5. |
| `observedAtMillis` | integer | yes | Plugin observation time, epoch milliseconds. |
| `coalescedSincePreviousAcknowledged` | integer | yes | Gap evidence, `0` when none. |
| `playlistLoop` | integer | no | FPP's own mainPlaylist pass counter for the running playlist, `0` on the first pass. Absent when the callback did not supply one. See §1.8. |
| `unavailable` | string | no | Absent, or one of the §1.4 reasons. |

`instanceUuid`, `sequence`, `observedAtMillis`, `action`, `schemaVersion`, and
`coalescedSincePreviousAcknowledged` are present on every observation,
available or not.

The identity fields divide into two groups, and the split is load bearing:

- **`playlistHash` and `entryKey` are derived identity.** They are required
  when `unavailable` is absent and **must be absent** when it is present.
  Neither can exist without the playlist definition, so a value in either
  field on an unavailable observation is a claim nothing computed. The
  coordinator refuses it rather than storing an entry key it never verified,
  because Track H consumes the stored observation and an unverified key there
  is an unverified key in the Cue binding.
- **`playlistName`, `section`, and `position` are corroborating evidence.**
  They are required when `unavailable` is absent, and are permitted but not
  required when it is present. `section` may legitimately be an empty string;
  `playlistName` may not.

"Permitted to be absent" never means "permitted to be arbitrary". Every field
present on the wire is validated whether or not `unavailable` is set.

**The canonical spelling of `section`.** The wire value of `section` is the
playlist definition's own section member name — `leadIn`, `mainPlaylist`, or
`leadOut` — for any of the three sections FPP's playlist callback has a
known runtime spelling for, and is FPP's own runtime string, passed through
**unchanged**, for any other value FPP might report. It is never FPP's
runtime spelling for those three known sections specifically: the plugin
maps it **before** placing it in this field or hashing it into `entryKey`
(§1.3), and only for a runtime string it does not recognize does the runtime
spelling itself reach the wire. This was previously left implicit, and the
two sides read it two different, individually defensible ways, which is the
failure [TRACK-H-CHAIN](../bench/TRACK-H-CHAIN.md) records. FPP 10.0's
runtime strings, and what each maps to on the wire:

| FPP runtime section string | Canonical wire value |
|---|---|
| `LeadIn` | `leadIn` |
| `MainPlaylist` | `mainPlaylist` |
| `LeadOut` | `leadOut` |

Any runtime section string outside this table — including FPP's `New`, which
names no member of the playlist definition — is passed through **unchanged**.
It is not mapped to a fourth canonical spelling and it does not become an
§1.4 `unavailable` reason: minting either would be a new wire value this
contract does not otherwise need. An entry key built from it will not match
any entry the coordinator parsed out of `leadIn`, `mainPlaylist`, or
`leadOut`, so the visible failure is the coordinator's existing
unknown-entry outcome, on the observation carrying that section, not a
protocol-level refusal.

### 1.3 The two hashes

These are not negotiable and are not reinterpreted by either side. The
reference implementation is `deriveEntryKey()` in the plugin repository's
`native/src/playlist_identity.cpp`; the coordinator's `pkg/fppidentity`
matches it byte for byte, proven by the shared fixtures. A disagreement is a
blocker raised against both, never a unilateral change on either side.

**`playlistHash`** is SHA-256 over the [RFC 8785](https://www.rfc-editor.org/rfc/rfc8785)
JSON Canonicalization Scheme serialization of the complete playlist definition
FPP returned. No runtime field is removed before hashing.

**`entryKey`** is SHA-256 over the RFC 8785 canonicalization of a JSON object
with exactly these five members. JCS sorts member names by their UTF-16 code
units, so the canonical member order is:

```json
{"instanceUuid":"...","playlistHash":"...","playlistName":"...","position":0,"section":"..."}
```

`position` is a JSON number, not a string. The key is an object rather than a
delimited string specifically so a playlist name or section containing a
separator character cannot collide with a different entry.

The `section` member is the same canonical value §1.2 fixes — the playlist
definition's member name, or an unrecognized runtime string passed through
unchanged — never FPP's runtime spelling for a section §1.2's table maps. An
implementation that hashes FPP's runtime string here instead produces an
`entryKey` that never matches the coordinator's, which is the failure
[TRACK-H-CHAIN](../bench/TRACK-H-CHAIN.md) records; `entry-key.json`'s cases
alone could not catch it, because they start from an already-canonical
`section` rather than from FPP's runtime spelling. `test/fixtures/fpp/`'s
`section-mapping.json` pins the mapping itself for exactly this reason.

The canonicalization rules both sides implement:

- Object member names sorted by UTF-16 code unit, duplicates rejected.
- No insignificant whitespace.
- ECMAScript `Number::toString` number formatting.
- Minimal string escaping: `"`, `\`, `\b`, `\f`, `\n`, `\r`, `\t`, and
  `\u00xx` for any other code point below `0x20`. Nothing else is escaped.
- UTF-8 output.
- Container nesting is bounded at 200. Only objects and arrays count toward
  the depth; a scalar does not.
- Invalid UTF-8 in a string or a member name is **refused**, not passed
  through. A definition containing it is `unsupported_definition_shape`.

The last two rules are stated because they are exactly where two independent
implementations drift without either one looking wrong. Sorting member names
by UTF-16 code unit is only a total order over well-formed UTF-8: map two
byte-distinct malformed names onto the same code unit and the sort has a tie
it cannot break, at which point the canonical bytes depend on which sort
algorithm each side happens to use. Refusing invalid UTF-8 removes the tie
instead of papering over it, and it fails in the visible direction: a refused
definition becomes an explicit unavailable observation, never a hash two sides
disagree about.

**Known open divergence, 2026-08-21.** The plugin's C++ currently counts
nesting depth per container while an earlier draft of the coordinator's Go
counted it per value, an off-by-one at the boundary, and the C++ does not yet
refuse invalid UTF-8. The coordinator now implements both rules as written
above. The plugin must adopt them before the two are byte-identical on
malformed input. Well-formed definitions, which is every definition FPP itself
produces, are unaffected and are covered by the shared fixtures.

**2026-09-01 correction to the depth divergence's stated cause.** The
paragraph above is wrong about *why* the two sides disagree at the depth
boundary. Both implementations count only containers; neither ever counted
per value. `pkg/fppidentity/canonical.go`'s depth check has counted only
containers since the file's own introduction (commit `73f3f07`, 2026-08-21,
"Freeze the FPP playlist-entry observation and brightness contracts", #38)
and has not been changed since: there is no earlier coordinator draft that
counted per value to find in this repository's history.

The real difference is **where** the bound is read. The coordinator's Go
parser (`parseArray`/`parseObject` in `pkg/fppidentity/canonical.go`) checks
`depth > maxDepth` immediately after incrementing, on the container's own
entry, before it looks at what the container holds. The plugin's C++ parser
(`native/src/json.cpp`) checks `depth_ > kMaxDepth` at the top of
`parseValue`, which runs once per element about to be parsed, not once per
container entered; `parseArray`/`parseObject` increment `depth_` themselves
without checking it. So a container's depth is only evaluated by the
`parseValue` call that parses its *first element*; a container with no
elements is never checked at all, at any depth.

As of this correction: the coordinator refuses 201 nested containers for
every document, whether the innermost container is empty or holds a value
(`test/fixtures/fpp/canonicalization.json`'s `depth-201-nested-arrays-empty-innermost`
and `depth-201-nested-arrays-scalar-innermost`). The plugin's C++, read
directly from `native/src/json.cpp` for this correction, accepts 201 nested
containers when the innermost container is empty and refuses when it is not,
matching the mechanism above. Invalid UTF-8 remains unrefused on the plugin
side; the coordinator refuses it, pinned by
`test/fixtures/fpp/canonicalization.json`'s `invalid-utf8-string-value` and
`invalid-utf8-member-name` cases, carried via that file's `inputHex` field
because a JSON string cannot hold a byte that is not valid UTF-8. Neither
divergence is closed by this correction or its fixtures alone: the plugin fix
for both is a separate, later change.

### 1.4 Unavailable observations

Identity never silently degrades to filename matching. When the plugin cannot
establish identity it sends an explicit unavailable observation, and the
coordinator stores and streams it rather than refusing it. The wire spellings
are:

| Wire value | Plugin `IdentityUnavailable` |
|---|---|
| `missing_instance_uuid` | `kMissingInstanceUuid` |
| `missing_playlist_name` | `kMissingPlaylistName` |
| `missing_definition` | `kMissingDefinition` |
| `unsupported_definition_shape` | `kUnsupportedDefinitionShape` |
| `negative_position` | `kNegativePosition` |
| `truncated_identity_field` | `kTruncatedIdentityField` |

The C++ enum's human strings are not wire values. Any other value in
`unavailable` is refused as an invalid parameter.

An unavailable observation carrying a `playlistHash` or an `entryKey` is
refused. See §1.2 for why those two fields, and not the corroborating ones.

An unavailable observation whose `instanceUuid` is itself missing cannot be
attributed to an instance and is refused: `missing_instance_uuid` is
reportable only when some other identity input failed, never as the reason a
body arrived with no instance at all.

### 1.5 The sequence must survive a plugin restart

**The per-instance `sequence` is required to be persistent and monotonic
across plugin and host restarts.** This is a requirement this contract places
on the plugin, and the plugin owns writing the file that satisfies it. The
plugin's `SequenceState` already refuses to move backwards in memory and
already exposes `restore()`; nothing currently calls it, so today's sequence
resets to `0` on every `fppd` start. Under the rule below, a restart would
then make every subsequent observation a refusal, so the follow-up plugin
issue must persist the sequence before the ingestion path can be exercised
end to end.

What the coordinator does with a genuine regression:

- It refuses the observation with `409`, audits it, and leaves the stored
  latest observation untouched. It does not accept a lower sequence, and it
  does not silently re-anchor: silently accepting a regression is
  indistinguishable from accepting a replayed or forged observation.
- The refusal names the last accepted sequence for that instance, so an
  operator reading it can tell a restart apart from a genuine reorder.
- The stored per-instance sequence is cleared only by an explicit,
  authenticated operator action. That action does not exist yet and is
  deliberately out of scope here; it is specified in
  [TRACK-H-H2-SPEC](TRACK-H-H2-SPEC.md) section 5.1. The store method exists
  and no route reaches it, so today the only way to clear a row is direct
  database access on the coordinator host.

**That recovery route is a prerequisite for the plugin's sending half, not a
nice-to-have.** Nothing posts to this endpoint yet, so nothing can be wedged
today. The moment a plugin does post, a single observation carrying a wildly
high sequence, from a misconfigured host or a compromised credential, refuses
every later legitimate observation for that instance permanently. There is
also no binding between the authenticated principal and the `instanceUuid` it
reports for: `fpp:observe` authorizes reporting for any instance, because
ADR-024 decision 4 delivers action scoping and states plainly that target
scoping is not implemented. Both facts are recorded here so the follow-up work
inherits them rather than rediscovering them.

### 1.6 Ingestion behavior

In order:

1. Authenticate; refuse `401` when no credential resolves.
2. Check `fpp:observe`; refuse `403` naming the scope.
3. Bound the body at 16384 bytes; refuse `413` on overflow.
4. Decode, and canonicalize the raw body. Refuse `400` on malformed JSON,
   unknown fields, trailing content after the object, or a duplicate member
   name. A duplicate member name matters here and not merely as pedantry: a
   permissive decoder keeps the last `sequence` while a reader of the same
   bytes sees the first.
5. Refuse `400` when `schemaVersion` is not `1`.
6. Refuse `400` when `instanceUuid` is absent or empty.
7. Refuse `400` when `action` is outside the fixed vocabulary, when
   `unavailable` is outside the §1.4 vocabulary, when `position` is negative,
   when a hash is present and is not 64 lowercase hex characters, or when
   `coalescedSincePreviousAcknowledged` or `sequence` is negative. Refuse
   `400` when `unavailable` is absent and any of `playlistName`,
   `playlistHash`, `position`, or `entryKey` is missing, and when
   `unavailable` is present and either `playlistHash` or `entryKey` is
   present.
8. When `unavailable` is absent, re-derive the entry key from
   `instanceUuid`, `playlistName`, `playlistHash`, `section`, and `position`
   and refuse `400` when it disagrees with the submitted `entryKey`.
9. Compare `sequence` against the last accepted sequence for that instance:
   - greater: accept.
   - equal and the canonical body is identical: **idempotent replay**, `200`,
     nothing stored, no change-stream update.
   - equal and the body differs: refuse `409`.
   - lower: refuse `409` (§1.5).
10. Store the complete observation as the latest state for that instance. The
    change stream renders the stored latest observation on its own pass and
    emits `fppPlaylistEntry.changed` when it differs from what it last sent,
    so several observations accepted between two passes collapse into one
    frame. That is current-state convergence, not a lost event: ADR-020 makes
    the stream non-resumable, and a client re-fetches the authoritative state
    rather than reconstructing it from frames.

Accepted observations are not written to the audit log; a per-entry audit
entry would flood it during an ordinary show. Every refusal from step 5
onward **is** audited, under the action `fpp.observe_playlist_entry`, with the
refusal reason. Steps 1 through 3 keep the coordinator's existing generic auth
and size-refusal behavior, which means a `401` or a `403` here is recorded
exactly as it is on every other write route and gets no ingestion-specific
audit entry of its own.

**Ingestion grants no execution authority.** Accepting plugin evidence says
only that FPP reported an entry. Track H applies Show, Playlist, Cue, and
active-show authorization after ingestion, and an accepted observation for a
non-active show must never activate anything.

### 1.7 Refusal vocabulary

| Case | Status | Problem type |
|---|---|---|
| No credential | 401 | `unauthorized` |
| Missing `fpp:observe` | 403 | `forbidden` |
| Body over 16384 bytes | 413 | `payload-too-large` |
| Malformed body, unknown field | 400 | `invalid-parameter` |
| Unsupported `schemaVersion` | 400 | `unsupported-observation-schema-version` |
| Missing `instanceUuid` | 400 | `invalid-parameter` |
| Invalid enum, hash, or position | 400 | `invalid-parameter` |
| Identity field missing with no `unavailable` | 400 | `invalid-parameter` |
| Derived identity present with `unavailable` | 400 | `invalid-parameter` |
| Derived entry key mismatch | 400 | `observation-entry-key-mismatch` |
| Reused sequence, different body | 409 | `conflict` |
| Sequence regression | 409 | `conflict` |

### 1.8 Entry occurrence and `playlistLoop`

An entry OCCURRENCE is one visit to one playlist entry. Repeat ticks inside a
visit belong to the same occurrence; a later visit to the same entry is a new
one. Track H derives its Cue activation identity from the occurrence, so two
ticks inside one visit dedup to a single dispatch while a second visit
dispatches again.

The entry key alone cannot separate those two cases. A playlist looping back
into an entry derives the identical key on its second visit, because the key is
a function of the entry's identity and not of when it was reached.

`playlistLoop` is what separates them. It carries FPP's own mainPlaylist pass
counter for the running playlist, verbatim, from the same callback that
supplies every other field in §1.2. It is `0` on the first pass and increments
once per completed pass. It is corroborating evidence and never identity: it is
not an input to either hash in §1.3, so an entry's key is unchanged by it.

The coordinator begins a new occurrence when the action is `start`, OR the
entry key differs from the last accepted one, OR `playlistLoop` differs from
the last accepted one. Any of the three is sufficient.

Three properties of that rule are load bearing:

- **Absent compares equal to absent.** A plugin that sends no `playlistLoop`
  behaves exactly as it did before this field existed, so no deployed plugin
  changes behavior when a coordinator that understands the field is installed
  in front of it.
- **`0` is a value, not an absence.** A plugin reporting the first pass and a
  plugin reporting nothing must not compare equal, which is why the field is
  omitted rather than sent as zero when the callback did not supply it.
- **The `start` term stays.** FPP 9 fires the playlist `start` action and the
  first term alone is sufficient there. FPP 10 never fires it
  (`Playlist::PlayImpl()` flips status before `Start()` reads it), so on that
  major the third term is what provides the property. Removing a correct term
  because another one now covers the common case would break the major the
  fleet currently runs.

**Upgrade order is not free: the coordinator goes first.** §1.6 step 4 refuses a
body carrying an unknown field, and that refusal rejects the whole observation
rather than ignoring the member. So a plugin that sends `playlistLoop` to a
coordinator that predates this section has every observation refused with `400`,
and a coordinator receiving no observations activates no Cues at all. Upgrading
the coordinator first is safe in both directions, because the field is optional
and an older plugin simply never sends it.

## 2. Brightness transition gain

**Status: DESIGNED, NOT BUILT.** Neither side serves this. See 2.4.

### 2.1 The composition

RES-018 §1, restated so implementers do not have to re-derive it:

```text
effective output = round(ceiling * transition_gain / 100)
```

`ceiling` is 0–100 and is written by FPP's own schedule and operator command
path, through the plugin's registered `ShowMesh: Set Brightness Ceiling` FPP
Action. It is already implemented in the plugin.

`transition_gain` is 0–100 and is the coordinator's alone. It is **not**
reachable from any FPP Action, MQTT setter, or relative brighten/dim command.
Adding any second writer to it defeats the seam.

### 2.2 The coordinator-facing write

The night-session controller writes the transition gain by POSTing to the
resident plugin component, on the FPP host, at the plugin's own HTTP path:

```text
POST /api/plugin/showmesh/brightness/transition-gain
```

The plugin opens no listening socket of its own to serve this. The route
registers on fppd's own web server: on FPP 10 through Plugin API 6's
`registerPluginApi`, on FPP 9 through the libhttpserver adapter. See section
3.1 for the general rule this instance of a plugin-served route follows.

**This route is unauthenticated.** It accepts any caller reachable on the
show LAN. The accepted posture it matches is FPP's own: SECURITY.md and
RES-015 §7.4 record that fppd's web UI and API are unauthenticated by
default, and list that among the FPP exposures ShowMesh designs around rather
than files as its own defect. SECURITY.md's separate acceptance of cleartext
commands is a transport rule about what crosses the show LAN in the clear; it
does not by itself say a route may go unauthenticated, so it is not the
citation for this decision. A deferred bearer credential would refuse a
casual or accidental LAN caller, but the plugin would have to hold the
expected secret on the FPP host to check one, and RES-015 §7.4 records that
any ShowMesh credential placed on an FPP host must be treated as readable by
anyone who can reach that host's web UI, not only by someone with a shell on
it. See section 2.3 for what this season leaves unenforced.

Body:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `schemaVersion` | integer | yes | Currently `1`. |
| `targetPercent` | integer | yes | 0–100 inclusive. Out of range is rejected, never clamped. |
| `fadeSeconds` | integer | yes | 0–86400 inclusive. `0` applies immediately. |
| `requestId` | string | yes | Caller-minted idempotency key; a repeat of the same id is a no-op. |

These are exactly `BrightnessEngine::setGain(targetPercent, fadeSeconds, now)`'s
inputs plus a version and an idempotency key. The plugin rejects out-of-range
input rather than clamping it, so a mistyped value is visible instead of
silently rounded into range.

The response reports the applied state so the caller has evidence rather than
an HTTP 200: `{"schemaVersion":1,"applied":true,"gainStart":100,"gainTarget":75,"fadeSeconds":30,"ceiling":60,"effectiveOutput":45}`.

### 2.3 What this contract forbids

- No absolute-brightness write. A ShowMesh write that could restore an old
  ceiling is forbidden by Track F and by RESTING-MODE §7.3.
- No relative brighten/dim. A relative adjustment applied twice is a
  different value; MultiSync carries full state for the same reason.
- No FPP Action, MQTT topic, or command binding for the gain.
- A ceiling change during a gain fade takes effect immediately, and a later
  gain of 100 reveals the current ceiling, never a cached earlier one.

**Accepted limitation:** section 2.2's route is unauthenticated, so this
single-writer rule is a contract the night-session controller alone is
expected to honor, not one the route can enforce against another caller on
the show LAN this season. Any host on that LAN can POST a competing value.
This is an accepted risk for this season, not an oversight: a bearer
credential that would let the route refuse other callers is deferred to a
future season as a separate tracked item, per owner ruling (2026-08-23). The
rule stays written here as the intended contract; only its enforceability is
what this season gives up.

### 2.4 What remains unbuilt

The plugin does not yet serve this path; `BrightnessEngine::setGain()` has no
caller, so the gain is pinned at 100 on any real host and the compositional
seam cannot be exercised end to end. The coordinator does not yet call it
either: Track F's readiness still rejects any cue that requires compositional
brightness (`nightCheckNoUnbuiltBrightnessComposition`), and that check stays
in place until the plugin serves this contract and RES-018 §8's decisive
mid-fade case is observed on a real host.

This section is the frozen shape that unblocks those two implementations. It
is not a claim that either exists.

## 3. Playlist definition publication

**Status: SHIPPED**, apart from the coordinator-triggered republish, which has
no agreed shape yet.

Frozen 2026-08-22 for Track H seam H2. Section 1 gives the coordinator a
playlist hash and an entry key. Neither says what the playlist contains, so
neither can be authored against: an operator binding a ShowMesh Cue to FPP
entry 3 needs to see that entry 3 is `Thriller.fseq` before the show, not
discover it when FPP plays it.

This section fixes how the definition behind a hash reaches the coordinator.
**The plugin posts it.** The same principal, the same credential, and the same
`fpp:observe` scope as section 1.

### 3.1 Why the plugin sends it rather than the coordinator fetching it

The hash in section 1.3 is SHA-256 over the RFC 8785 canonicalization of the
definition the plugin read. Today that read is a local file: the resident
worker opens `FPP_DIR_PLAYLIST/<name>.json` and hashes those bytes. It is not
FPP's REST API, and it is not the `Json::Value` FPP hands the playlist
callback, which the plugin deliberately mines for three bounded fields and
otherwise discards.

If the coordinator fetched the definition from FPP's REST API instead, it
would be canonicalizing a different read of a different representation. FPP's
`GET /api/playlist/{name}` re-serializes through FPP's own JSON layer, and
whether it adds, drops, or recomputes a member relative to the stored file is
not measured anywhere in either repository. Canonicalization removes
formatting differences; it does not remove a field FPP's API injects.

Every binding in Track H is keyed on the hash. A hash the coordinator computed
from a second source is a hash no observation will ever match, and the failure
would arrive on show night looking like a permanent unexplained mismatch
rather than like the wrong import path it actually was. So the rule is: **the
bytes the plugin hashed are the bytes the coordinator imports.**

The plugin publishes rather than serving a read for one further reason, and it
is narrower than it once looked. The plugin opens no listening socket of its
own; the general rule is that it **may** register narrow, idempotent,
evidence-returning inbound routes on fppd's own web server — on FPP 10
through Plugin API 6's `registerPluginApi`, on FPP 9 through the
libhttpserver adapter. That is the mechanism section 2.2's brightness route is
DESIGNED to use, and which nothing in the plugin uses yet (see 2.4). It
never opens a second listener, never proxies FPP, and never serves a value
the coordinator has not verified. That general permission does not, by
itself, favor a read route for the definition: the plugin already needs an
outbound client for section 1's observations, and one more outbound POST is a
small addition to work already required, while a definition-read route would
still leave the coordinator fetching through a representation the plugin does
not control between hash and read. Push keeps the bytes the plugin hashed and
the bytes the coordinator imports identical without adding that risk; it is
not something the plugin is forced into by an inability to serve routes at
all.

### 3.2 The route

```text
POST /api/v1/integrations/fpp/playlist-definitions
```

Guarded by `fpp:observe`. Authentication and the scope check run before the
body is parsed, as in section 1.1. The body is bounded at **1048576 bytes**
and a larger body is refused with `413` before parsing. This bound is two
orders of magnitude above section 1.2's, because unlike an observation this
body does carry the complete definition.

### 3.3 Request body, schema version 1

```json
{
  "schemaVersion": 1,
  "instanceUuid": "M4-7840e12f81da4191c0d00fbb6a889314",
  "playlistName": "Halloween Main",
  "playlistHash": "<64 lowercase hex>",
  "definition": { },
  "capturedAtMillis": 1755900000000
}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `schemaVersion` | integer | yes | Currently `1`. Any other value is refused. |
| `instanceUuid` | string | yes | The same persistent FPP UUID section 1.2 reports, from FPP's `SystemUUID` setting. |
| `playlistName` | string | yes | FPP playlist name. |
| `playlistHash` | string | yes | SHA-256 over the canonicalization of `definition`, section 1.3. Lowercase hex. |
| `definition` | object | yes | The complete playlist definition, as a parsed JSON value. No member removed. |
| `capturedAtMillis` | integer | yes | When the plugin read this definition, epoch milliseconds. |

`definition` is the JSON value itself, not a string holding JSON. The
coordinator canonicalizes what it received and refuses the request when the
result does not hash to the declared `playlistHash`.

That check is the load-bearing one. It makes the transport irrelevant, since a
proxy that reformats the body still canonicalizes to the same bytes, and it
makes the store self-verifying: every definition is filed under a hash the
coordinator computed itself. A caller cannot install a definition under
someone else's hash, so the worst a forged post can do is add an entry nothing
references.

### 3.4 Ingestion behavior

In order:

1. Authenticate; refuse `401` when no credential resolves.
2. Check `fpp:observe`; refuse `403` naming the scope.
3. Bound the body at 1048576 bytes; refuse `413` on overflow.
4. Decode strictly. Refuse `400` on malformed JSON, an unknown field, trailing
   content after the object, or a duplicate member name, for section 1.6's
   reasons.
5. Refuse `400` when `schemaVersion` is not `1`.
6. Refuse `400` when `instanceUuid` or `playlistName` is absent or empty, when
   `playlistHash` is not 64 lowercase hex characters, when `definition` is
   absent or is not an object, or when `capturedAtMillis` is negative.
7. Canonicalize `definition` and refuse `400` with
   `definition-hash-mismatch` when its SHA-256 disagrees with `playlistHash`.
   A definition the coordinator's own canonicalizer refuses, for invalid UTF-8
   or excessive nesting, fails here too and is the same refusal: the
   coordinator never stores a definition it could not canonicalize.
8. Store under the key `(instanceUuid, playlistHash)`. A repeat of a key
   already held is idempotent `200` and stores nothing, because the key is the
   content. `playlistName` and `capturedAtMillis` on a repeat are ignored
   rather than overwriting the stored ones: the first report of a given
   content is the one with provenance.

There is no sequence and no ordering. Content addressing removes the need for
one, which also removes section 1.5's wedging hazard from this route entirely:
a definition carrying a wildly wrong value cannot refuse later legitimate
posts, because there is no counter for it to poison.

A store that actually inserted is audited, under the action
`fpp.publish_playlist_definition`. Unlike an accepted observation, this
happens once per playlist revision rather than once per entry, so it does not
flood, and it gives an operator a dated record that the FPP playlist changed.
An idempotent repeat is not audited. Every refusal from step 5 onward is
audited with its reason.

### 3.5 Refusal vocabulary

| Case | Status | Problem type |
|---|---|---|
| No credential | 401 | `unauthorized` |
| Missing `fpp:observe` | 403 | `forbidden` |
| Body over 1048576 bytes | 413 | `payload-too-large` |
| Malformed body, unknown field, duplicate member | 400 | `invalid-parameter` |
| Unsupported `schemaVersion` | 400 | `unsupported-definition-schema-version` |
| Missing or malformed identity field | 400 | `invalid-parameter` |
| Definition does not hash to `playlistHash` | 400 | `definition-hash-mismatch` |

### 3.6 Reading definitions back

```text
GET /api/v1/integrations/fpp/playlist-definitions
GET /api/v1/integrations/fpp/playlist-definitions/{instanceUuid}/{playlistHash}
```

Both under `observation:read`, matching every other FPP read surface. The list
returns metadata only (instance, playlist name, hash, captured and received
times, entry count, and whether a stored `show.playlist` references it); the
second returns the stored definition. The list exists so an operator can
choose a playlist to import without downloading every definition on the host.

### 3.7 When the plugin posts

The plugin posts a definition whenever it holds one whose hash it has not
already posted successfully, on all three of these occasions:

1. **When it resolves an entry identity.** The definition behind that hash
   must reach the coordinator before, or alongside, the first observation
   citing it. An observation is still accepted when the definition has not
   arrived, per section 1 unchanged; Track H holds that binding as having no
   definition rather than activating it.
2. **At worker start, for every playlist definition on the host.** The
   coordinator otherwise holds nothing until FPP plays something, and
   authoring happens in the afternoon with FPP idle. The plugin already reads
   this directory; enumerating it is the same read.
3. **On a bounded re-scan, no more often than every 60 seconds.** An operator
   who edits a playlist and does not play it would otherwise leave the
   coordinator holding the previous revision until the next plugin restart.
   Skipping a file whose size and modification time are unchanged keeps this
   cheap; no filesystem watch is required.

The plugin tracks which hashes it has posted successfully in memory only. A
restart re-posts, and the coordinator answers idempotently, so nothing is lost
by not persisting that set. This is deliberately weaker than section 1.5's
requirement on the observation sequence, and it is safe for the same reason
the route needs no sequence: the key is the content.

Retries use the same bounded backoff and the same visible local status as
section 1's observation delivery.

### 3.8 What the plugin had to add

None of this existed when this section was frozen on 2026-08-22. All of it
shipped afterwards; verified against the plugin runtime on 2026-09-08 and
listed here with where it landed, so the next reader does not re-derive it:

- Retain the canonical definition past the hash call.
  `resolveEntryIdentity()` computes `IdentityResolution::canonicalDefinition`,
  which now reaches `publishDefinition()` rather than being discarded.
- The outbound HTTP client section 1 already requires, carrying this second
  route. `CoordinatorClient::publishDefinition()` posts to `kDefinitionPath`.
- Enumeration of the playlist directory at start, and the bounded re-scan.
  The start-up sweep and `kDefinitionRescanIntervalMillis` (60 s) in the
  runtime's worker.
- The posted-hash set section 3.7 describes is `heldDefinitions_`, keyed on
  instance UUID and playlist hash, in memory only, so a restart re-posts.

Still true, and still a constraint rather than a task: no inbound HTTP route,
no second listener, and no change to the callback thread's bounded
copy-and-return.

### 3.9 What this section does not do

- It does not let the coordinator write anything to FPP or to the plugin.
- It does not give the coordinator a second source of entry identity. The
  entry key still comes from section 1.3, derived from the same five fields.
- It does not make a definition an observation. It carries no sequence, is not
  ordered against anything, and grants no execution authority. Track H applies
  Show, Playlist, Cue, and active-show authorization to a binding built from
  it, exactly as section 1.6 requires for an observation.

## 4. Shared fixtures

**Status: SHIPPED.** The files exist and both sides consume them.

`test/fixtures/fpp/` holds plain JSON data files, consumable by any language.
They are deliberately not a Go package and not a shared module: the plugin
repository is Apache-2.0 with no Go module dependency on the coordinator and a
C++ core that links no third-party library, so a fixture it cannot copy as
data is a fixture it cannot use.

See [`test/fixtures/fpp/README.md`](../../test/fixtures/fpp/README.md) for the
file format and the case list. The coordinator's own tests consume the same
files, so a fixture that drifts from the implementation fails on this side
before the plugin ever sees it.
