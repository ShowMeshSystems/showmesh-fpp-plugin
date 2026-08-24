# FPP plugin coordinator contracts

[RES-018](../research/RES-018-fpp-brightness-control.md) · [ADR-043](../decisions/ADR-043-show-scoped-cues-and-playlist-authority.md) · [ADR-024](../decisions/ADR-024-identity-authorization-and-audit.md) · [Track F](TRACK-F-resting-mode.md) · [Track H](TRACK-H-cues-and-playlists.md) · [SM-63 handoff](SM-63-FPP-PLUGIN-HANDOFF.md)

Status: frozen 2026-08-21, extended 2026-08-22, corrected 2026-08-23. This
record fixes the wire contracts the ShowMesh FPP plugin runtime shares with
the coordinator: playlist-entry observation ingestion, the coordinator-owned
brightness transition gain, and playlist definition publication.
RES-018 decided the design; this file fixes the exact bytes so the plugin and
the coordinator can be built independently and still meet.

**2026-08-23 correction:** section 3.1 previously claimed the plugin "has no
HTTP server and deliberately registers no route" and cited ADR-013 for that
claim. That contradicted section 2.2, which has always required the plugin to
serve an inbound brightness route, and ADR-013 is about UDP 32320 MultiSync
port sharing, not HTTP routing; it says nothing that forbids a route. Per
owner ruling (2026-08-23): the plugin opens no listening socket of its own,
but it may register narrow, idempotent, evidence-returning routes on fppd's
own web server, and section 2.2's brightness route ships unauthenticated this
season, matching SECURITY.md's accepted cleartext-command posture on the
isolated show LAN. A bearer credential is deferred to a future season as a
separate tracked item. See sections 2.2, 2.3, and 3.1.

Nothing here has run against a real FPP host. The contract is verified by unit
tests and by the shared fixtures in
[`test/fixtures/fpp/`](../../test/fixtures/fpp/README.md), on both sides.

## 1. Playlist-entry observation ingestion

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
| `section` | string | when available | FPP playlist section. May be empty. |
| `position` | integer | when available | Zero-based position within the section. |
| `entryKey` | string | when available | SHA-256, lowercase hex. See §1.3. |
| `sequenceFilename` | string | no | Absent when the entry has none. |
| `mediaFilename` | string | no | Absent when the entry has none. |
| `action` | string | yes | One of `start`, `playing`, `stop`, `query_next`, `unknown`. |
| `sequence` | integer | yes | Monotonic per-instance event sequence. See §1.5. |
| `observedAtMillis` | integer | yes | Plugin observation time, epoch milliseconds. |
| `coalescedSincePreviousAcknowledged` | integer | yes | Gap evidence, `0` when none. |
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

## 2. Brightness transition gain

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
show LAN, matching the posture SECURITY.md already records for cleartext
commands on that isolated network: fppd's own web server is unauthenticated
by default. A deferred bearer credential would refuse a casual or accidental
LAN caller, but not anyone with shell access on the FPP host, which RES-015
already records as the accepted limit for the plugin's existing outbound
credential; see section 2.3 for what this season leaves unenforced.

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
libhttpserver adapter — as section 2.2's brightness route already does. It
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

### 3.8 What the plugin must add

Stated plainly, because as of 2026-08-22 none of it exists:

- Retain the canonical definition past the hash call.
  `resolveEntryIdentity()` already computes `IdentityResolution::canonicalDefinition`
  and the runtime discards it.
- The outbound HTTP client section 1 already requires, carrying this second
  route.
- Enumeration of the playlist directory at start, and the bounded re-scan.
- No inbound HTTP route, no second listener, and no change to the callback
  thread's bounded copy-and-return.

### 3.9 What this section does not do

- It does not let the coordinator write anything to FPP or to the plugin.
- It does not give the coordinator a second source of entry identity. The
  entry key still comes from section 1.3, derived from the same five fields.
- It does not make a definition an observation. It carries no sequence, is not
  ordered against anything, and grants no execution authority. Track H applies
  Show, Playlist, Cue, and active-show authorization to a binding built from
  it, exactly as section 1.6 requires for an observation.

## 4. Shared fixtures

`test/fixtures/fpp/` holds plain JSON data files, consumable by any language.
They are deliberately not a Go package and not a shared module: the plugin
repository is Apache-2.0 with no Go module dependency on the coordinator and a
C++ core that links no third-party library, so a fixture it cannot copy as
data is a fixture it cannot use.

See [`test/fixtures/fpp/README.md`](../../test/fixtures/fpp/README.md) for the
file format and the case list. The coordinator's own tests consume the same
files, so a fixture that drifts from the implementation fails on this side
before the plugin ever sees it.
