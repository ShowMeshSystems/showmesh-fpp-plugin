# ADR-024: Identity, Authorization, and Audit for the Write Surface

Status: Accepted  
Date: 2026-08-11

Supersedes [ADR-021](ADR-021-read-api-authentication-posture.md), carrying
forward three of its rules restated in decision 3, and supersedes decision 4 of
[ADR-022](ADR-022-operator-ui-serves-the-api-same-origin.md).

## Context

[ADR-021](ADR-021-read-api-authentication-posture.md) rule 5 makes a superseding
ADR, deciding a real authentication and authorization mechanism, a precondition
for the first write operation. Its supersession section says what that record
must cover: authenticated identities, authorization by target and action per
ARCHITECTURE §10.4, audit attribution, the MQTT control plane's own
authorization, and a session model for the browser. ADR-021 states plainly that
one shared secret is not an identity, that it does not satisfy §10.4, and that
it expects to be superseded.

That bar is now the project's critical path rather than a piece of hygiene.
Steps 0 through 5 are complete, and every operator-facing thing they built
displays state rather than changing it. ARCHITECTURE §12 Phase 1 is dominated by
write work: native FPP lifecycle commands, resting and background audio with
deterministic fades, stable projection through Resolume, readiness checks, and
blackout. Two of its six items are documentation rather than code, and the rest
cannot begin. Nothing of consequence starts until this record lands.

Five constraints are inherited rather than chosen:

- **[ADR-022](ADR-022-operator-ui-serves-the-api-same-origin.md) decision 2** is
  load-bearing and outlives the shared-secret posture that motivated it. The UI
  container forwards credentials and never holds, injects, mints, validates, or
  refreshes one. Decision 3 of that record supplies the test that keeps it
  observable: removing the proxy and pointing a client straight at the
  coordinator must change nothing except the origin.
- **ADR-022 decision 4**, the shared secret in `sessionStorage`, was built
  deliberately small enough to delete. Deleting its semantics is expected. Its
  input affordance is kept, for the reason in decision 5.
- **[ADR-014](ADR-014-operator-ui-is-an-api-client.md)**: the API is a public
  versioned contract. Whatever lands must be usable by `showmeshctl` and by
  `curl`, not only by a browser.
- **[ADR-020](ADR-020-control-api-shape-and-change-stream.md)**: within a major
  version the contract is additive-only. Whether authentication forces `v2` is a
  decision this record must make explicitly rather than assume.
- **[ADR-008](ADR-008-mqtt-control-plane.md)** and ARCHITECTURE §2.4: a running
  show must survive coordinator loss and broker loss. Authorization must not
  become something a show depends on at runtime.

The deployment reality matters as much as the documents. The show VLAN is the
actual security boundary today. The bundled Mosquitto sets `allow_anonymous
true`, and the deferred item recorded when the control-plane skeleton landed
still stands: any client with publish rights can create unbounded node rows by
publishing on arbitrary syntactically valid node IDs. ADR-021's own consequences
judge the broker the larger exposure of the two, because publish rights change
coordinator state while the read API only discloses it, and `SECURITY.md` says
the same to anyone who reads it first. Separately, the deployed FPP fleet
publishes to the operator's home-automation broker authenticating as a
shared home-automation account that also holds publish rights, on a broker where
`falcon/player/<host>/command/run` is a topic FPP acts on. That is not a
hypothetical about shared broker credentials; it is the reference installation.

### The constraint that shaped this record most

The operator runs this show outdoors, at night, in the cold, from a phone.

Any mechanism that can demand a credential at that moment is wrong regardless of
how well it reads on paper. This ruled out more of the design space than any
security argument did: it eliminated short idle timeouts, sessions with refresh
cycles, per-tab credential storage, and per-principal lockout. It is recorded
here, first, so that a future contributor tightening one of those knobs knows
what they are trading away rather than discovering it during a set.

It also creates one tension this record has to resolve rather than inherit.
Every conventional defence against credential guessing is a lockout, and a
lockout is an operator lockout at showtime that an attacker can trigger
deliberately. Decision 8 resolves it.

## Decision

### 0. What this record does not decide

It does not define any write endpoint, does not decide how FPP triggers a show
macro, and does not decide node enrollment automation. It decides who a
principal is, what a principal may do, how a credential is presented and stored,
who may speak on the control-plane broker, and what is recorded when someone
acts.

### 1. Principals are coordinator-local, in two credential forms

A **principal** has an id, a display name, a kind (`human` or `machine`), a
role, a creation time, and a disabled flag. It lives in the coordinator's SQLite
store.

**Kind does not restrict credential form, and role is independent of kind.** A
human may mint an API token to use `showmeshctl` from a terminal, and a machine
principal may hold any role the operator grants it. This is stated explicitly
because the tidy-looking symmetry, humans get sessions and machines get tokens,
would make humans browser-only: an operator at a terminal would have to borrow a
machine principal's token, at which point the audit log records `scheduler`
rather than a person and this record's central property evaporates for exactly
the humans it was written for. ADR-014's usable-with-no-UI requirement is not
satisfied by a CLI that can only act as a robot.

Both credential forms resolve to the same principal and the same scope set.
Every authorization decision and every audit entry is written against a
principal, never against a credential form.

No external identity provider is required, and none may become required for
operator access. An identity-provider outage must never be able to lock an
operator out of a running show. OIDC and reverse-proxy forward-authentication
are extension points for a future record, deliberately not built here.

**Credential handling is constrained by ADR-012's pure-Go requirement, and the
parameters are a decision rather than an implementation detail.** Passwords are
hashed with argon2id via `golang.org/x/crypto/argon2`, which is CGo-free and
cross-compiles cleanly to `linux/arm64`, and is a new dependency rather than one
already present. Its cost parameters are fixed here at **64 MiB memory, time
cost 2, parallelism 1**, because memory cost on a Pi-class coordinator running
the broker and the UI alongside it is a capacity decision that determines what
decision 8 has to bound. API tokens are high-entropy random values of at least
128 bits, stored as SHA-256 digests; they are not passwords and a slow KDF buys
nothing against a value with no dictionary behind it. Every token carries a
fixed identifiable prefix, is displayed exactly once at creation, and the prefix
is load-bearing rather than cosmetic: see the URL rule below.

**Machine tokens do not expire by default.** An explicit expiry may be set, but
the default is none, and the control is revocation. A token that expires
mid-season would make FPP's scheduling authority
([ADR-001](ADR-001-fpp-is-authoritative.md)) depend on ShowMesh's clock and on
the operator's rotation discipline, which is the runtime dependency ADR-008 and
ARCHITECTURE §2.4 exist to prevent.

**A credential is never carried in a URL, and this is enforced rather than
asked for.** Authentication never reads a query parameter under any
circumstance, and the coordinator rejects with `400` and audits any request
whose query string contains the token prefix. FPP invokes its own commands over
GET, so a credential in a query string lands in access logs, in browser history,
and in `Referer` headers.

The first pressure on this rule will not come from FPP, and naming the real one
matters: OBSERVABILITY's preview wall uses elements that cannot set headers, so
a thumbnail or preview stream URL will want a query-parameter token. For a
browser the session cookie already solves it, which is an uncredited point in
favour of decision 5. For a non-browser consumer it is unsolved, and it must be
solved by a mechanism other than a credential in a URL.

### 2. Writes always require authentication. Reads keep ADR-021's posture. Neither forces v2

**Every write endpoint requires an authenticated principal.** There is no
opt-out, no default-open mode, and no configuration setting that disables it.

**Read endpoints keep ADR-021's posture:** open by default, closable by
configuration, with the startup warning naming the exposure when they are open.

Reads staying open is an operational safety decision and not inertia. A
credential problem must never cost the operator visibility of the show.
Whatever else fails, the phone still renders health, and the failure is scoped
to "you cannot act", a state an operator can understand and route around, rather
than a blank screen indistinguishable from the coordinator being down. The cost
is stated honestly in the consequences: OBSERVABILITY §13 stays only
conditionally met.

Requiring authentication on writes is **additive under ADR-020**, and the reason
must be stated precisely rather than approximately. It is not that v1 clients
have always presented a credential; with the token unset, which is the default,
every v1 read client works today with none. It is that **no v1 client has ever
called a write endpoint, because v1 has none.** Requiring a credential on
surface that does not yet exist breaks nothing. **There is no `/api/v2`.**

`SHOWMESH_API_TOKEN`, the ADR-021 shared secret, is **retired, and its removal
must fail loudly rather than fail open.** If the variable is still set when a
coordinator carrying this record starts, the coordinator **refuses to start**
and names the migration in the error.

That is deliberately the harshest of the three available behaviours, and the
other two are worse. Ignoring the variable means an operator who deliberately
closed their read API has it silently reopened by a container tag change, which
is a security control failing open on upgrade and is exactly the switch ADR-021
existed to give them. Honouring it contradicts the retirement and keeps a shared
secret alive indefinitely. A refusal to start is deterministic, visible in the
first seconds, fixed by editing one line of `.env`, and is the one outcome that
cannot quietly reduce security. It is also the change most likely to surprise
an upgrading operator, so `deploy/` must document it before the release that
carries it.

Version negotiation continues to run *before* authentication, preserving the
Step 3 decision, so version skew stays diagnosable without credentials. That is
restated here because it is easy to break while adding an authentication
middleware, and its failure mode is an operator unable to tell a version
mismatch from a bad password.

### 3. What this record carries forward from ADR-021

ADR-021 held three rules that this record's own decisions do not restate and
that would become unowned if it were superseded without them. They are carried
forward unchanged, and are listed rather than assumed because a superseded
record is not a place a future implementer will look.

1. **`/healthz`, `/readyz`, and `/version` are never authenticated**, under any
   read-closure setting. They disclose nothing operational and the container
   healthcheck depends on them. The failure mode of getting this wrong is not
   subtle: wrapping the health routes in the new middleware turns "the operator
   closed the read API" into a failing healthcheck, a restarting coordinator,
   and a restart loop during a show.
2. **Every credential is redacted by the configuration type's `LogValue`,
   enforced by code rather than by a doc comment, and never appears in an error
   body, a log line, or a problem detail.** This now covers more than one
   secret: passwords, password hashes, session identifiers, token values, token
   digests, the bootstrap code, and broker credentials.
3. **CORS is configured by explicit allow-list and defaults to emitting no CORS
   headers at all.** This matters more under this record than it did under
   ADR-021, not less. The existing middleware already emits
   `Access-Control-Allow-Credentials: true` for an allow-listed origin, which is
   close to inert while the credential is a hand-typed bearer token and is not
   inert once the credential is an automatically attached cookie. So, stated
   explicitly because it spans two mechanisms that must interlock: **a
   configured CORS origin does not exempt a cookie-authenticated write from the
   same-origin requirement in decision 6.**

### 4. Authorization is action-scoped, and roles are named scope bundles

Authorization is expressed as scopes of the form `<resource>:<action>`. The read
surface is gated by `node:read`, `fpp:read`, `observation:read`, and
`event:read`, one per resource the v1 read API actually serves, rather than by a
single scope whose name is narrower than the surface it controls. The write
surface adds `show:macro:run`, `device:power`, `fpp:command`, `config:write`,
`principal:write`, and `audit:read`. **Amended 2026-08-15:** Track D's seam D-3
adds `resolume:action`, one scope for the whole Resolume action vocabulary, on
the same reasoning as `fpp:command`.

Roles are named bundles of scopes. None of them is restricted to a principal
kind:

| Role | Holds |
|---|---|
| `viewer` | every read scope, and nothing else |
| `operator` | the read scopes plus the show, device, FPP, and Resolume action scopes |
| `admin` | everything, including `principal:write` and `audit:read` |
| `scheduler` | `show:macro:run` and nothing else |

Enforcement is a single check at the coordinator's API boundary: does this
principal hold the scope this action requires. Never at the UI, never at the
proxy.

**Target scoping is deliberately not implemented and deliberately expressible.**
ARCHITECTURE §10.4 requires authorization by target *and* action. This record
delivers action, and fixes the shape of the check so that a target predicate
later narrows an existing grant rather than replacing the model. §10.4 is
therefore **partially satisfied**, and that is recorded as partial rather than
allowed to look like compliance, in exactly the way ADR-021 refused to let its
own shared secret look like compliance. The reason for deferring targets is
ADR-021's own and it has not stopped being true: there is one operator and no
consumer to design a target taxonomy against, and a guessed authorization model
is expensive to retract.

### 5. The browser session is long-lived, device-scoped, and slides on any use

This is where the cold-phone constraint does the most work, and every clause
below exists because of it.

The coordinator mints a session at `POST /api/v1/session` and returns an
**HttpOnly cookie**. The UI container forwards it. ADR-022 rule 2 is unbroken:
the coordinator mints and validates, the proxy only passes through, and it must
not rewrite the cookie's path or domain.

**A cookie, not `sessionStorage`.** ADR-022 decision 4's semantics are deleted,
as they were built to be. `sessionStorage` dies with the tab, which means
re-entering a secret on a phone, outdoors, at night. A persistent cookie
survives tab close, browser restart, and three hours in a coat pocket.

**A session expires only after 90 consecutive days without use, and "use" means
any request that carries the cookie, including a read.** There is no absolute
lifetime. Both halves are stated because "90 days, sliding" is ambiguous in a
way that produces the exact failure this decision exists to prevent. Under an
absolute reading, a session minted at setup in late September dies around
Christmas week. Under a sliding reading that only counts *authenticated* use,
an operator who watches the dashboard nightly for three months and issues no
writes never slides it, because reads are open and never enter the
authentication path, so the session expires from apparent disuse and they
discover it the first night they need to act. Sliding on cookie presence rather
than on authentication closes both.

**Sessions carry a per-principal generation counter.** A password change, a role
change, or an administrative revocation of all sessions increments it, and every
session below the current generation is invalid. Without this, a password change
would leave stolen sessions alive.

**A database restore is an operator step, not an automatic one, and this
paragraph was corrected after the implementation proved the original claim
false.** The first version of this decision said a restore also increments the
counter. It cannot: the counter lives in the database, so restoring a
pre-revocation backup rolls the counter back along with everything else.
Reproduced end to end during Step 6, where a session revoked by a password
change authenticated again as `admin` after the data directory was restored from
a copy taken before the revocation.

Auto-detecting a restore from inside the restored database is structurally
impossible for the same reason, and this record does not pretend otherwise. The
mechanism is instead an explicit host-level subcommand,
`showmesh-coordinator invalidate-all-sessions`, which bumps every principal's
generation, and the restore runbook in `deploy/` must call it. That makes the
guarantee real and operator-visible rather than asserted and absent.

**Sessions are device-scoped and individually revocable**, each carrying an
operator-supplied device label and listed in the UI. The label is a **mnemonic,
not a binding**: a stolen cookie is not a new session row, it is the same
session presenting the same label, so revocation handles a lost phone, which you
know about, and does not handle a copied cookie, which you do not. The
generation counter above is the blunt instrument that does.

**Revocation must reach an open change stream.** Authentication is evaluated
once at request start in every conventional middleware, and `/api/v1/stream` is
a request that stays open indefinitely, so revoking a session would otherwise
have no effect on the connection the lost phone is holding. The coordinator
therefore revalidates the credential of an open stream periodically and on
generation change, and **closes the connection** when it becomes invalid. It
does not emit a new event kind to announce it: ADR-023 recorded that a client
ignoring an unrecognised event kind silently stops updating while its connection
stays green, and that failure is worse here than a disconnect. Closing the
connection puts the client on ADR-020's existing path, a reconnect and an
authoritative snapshot re-fetch, where a `401` surfaces as an explicit
authentication state.

**Being signed out is a persistent state, never a modal at the moment of use.**
This covers three cases and not one: a session that has become invalid, a
session approaching expiry, and **a device that has never authenticated at
all**, which is a new phone, cleared cookies, a private tab, or an installed
progressive web app with its own cookie jar. The UI learns which on load from
`GET /api/v1/session`, so the banner costs nothing, and without the third case
the operator's first authentication demand still arrives at the instant they
press a button.

**The bearer-paste affordance stays in the UI as break-glass.** ADR-022
decision 4 is deleted for its shared-secret semantics, not for its input box.
Without it, every failure of the principal store, a forgotten password, a
corrupt database, a bad restore, has no phone-reachable workaround and requires
a laptop and host access, outdoors, at night. Keeping the field lets a machine
token written on a card in the operator's wallet act from a phone when the
session path is broken.

**The `Secure` attribute is an explicit setting, off by default, and never
inferred.** ShowMesh terminates no TLS (ADR-022 decision 5), so setting `Secure`
unconditionally would break the default bundle outright. It is not derived from
`X-Forwarded-Proto`, because trusting a forwarded header would make the proxy
part of the trust decision, which is what ADR-022 rule 2 exists to prevent.

Two consequences of that follow and must be documented rather than discovered.
With no TLS in front, the session cookie is readable on the wire, and the show
VLAN remains the actual boundary. And **cookies are scoped by host, not by
origin**: port is ignored, and with `Secure` off, scheme is too. The bundle
publishes the coordinator and the UI on different ports of the same address, so
the cookie reaches both, and it would reach any other HTTP service sharing that
address, which on a homelab host is a real possibility rather than a
hypothetical. The standard fix is the `__Host-` cookie prefix, which requires
`Secure` and is therefore unavailable by default. So: where a deployment puts
TLS in front, it must set `Secure` and the cookie must carry the `__Host-`
prefix; where it does not, the host-scoping is a known limitation and the
coordinator should not share a host address with unrelated services.

### 6. Cross-site request forgery, HTTP methods, and the bearer exemption

ADR-022 decision 4 rejected cookies partly because they attach automatically.
That exposure is real now that writes exist, and closing it takes three rules
rather than one.

**No endpoint that changes state may be reachable by `GET` or `HEAD`.**
`SameSite=Lax` deliberately permits the cookie on cross-site top-level `GET`
navigation, so without this rule the entire cookie CSRF story reduces to a
question nobody has answered: is any write reachable by GET? In a project whose
own Step 5 lesson is that **GET-only is not read-only**, because FPP invokes
commands over GET, leaving that open would be negligent. If a mechanism for
triggering a macro cannot issue a method other than GET, it must not be an HTTP
endpoint.

**A write authenticated by cookie requires `Sec-Fetch-Site: same-origin`, and
is rejected when the header is absent.** There is no `Origin` fallback, and that
omission is deliberate. Comparing `Origin` to "myself" requires the coordinator
to know its own browser-facing origin, whose only sources are `Host` and the
`X-Forwarded-*` family, and decision 5 forbids trusting the latter. The
plausible deployment makes this concrete: ADR-022 decision 5 invites a TLS proxy
in front of the UI container, nginx rewrites `Host` to the upstream name unless
configured otherwise, and the browser's `Origin` would then never match what the
coordinator sees. Every write would return `403` while every read worked, the
symptom being "the buttons do nothing", and the fix a contributor would reach
for is trusting `X-Forwarded-Host`, which is the boundary violation this record
spends a paragraph preventing. A rule that creates the pressure to break itself
is the wrong rule.

Rejecting on absence is fail-closed and costs almost nothing, because a
non-browser client uses a bearer token and is exempt. It does cost something
real and it is named here: `Sec-Fetch-Site` requires Safari 16.4 or later, so a
sufficiently old iOS device cannot use cookie-authenticated writes at all and
must use the bearer-paste path from decision 5.

**AMENDED 2026-08-14: there is now an `Origin` fallback, and the two
paragraphs above were wrong in one fact and right in the other.**

The cost estimate was wrong, and the error was not small. `Sec-Fetch-*` is
not merely a browser-version feature: **Chrome sends no `Sec-Fetch-*` header
at all to an origin it does not consider potentially trustworthy**, which
means every plain-HTTP address that is not `localhost`. Measured on one
machine, one browser, one page, the same `POST`, with only the origin
differing:

```
http://192.168.x.x:18099  ->  Origin present, no Sec-Fetch-* at all
http://localhost:18099    ->  Origin present, Sec-Fetch-Site: same-origin
```

ShowMesh terminates no TLS (ADR-022), so the practical effect was that the
browser session path worked on `localhost` and nowhere else. The operator
reaches the Operator UI from a phone on the show LAN, which is the whole
point of the responsive layout, and the refusal they got told them to
update Safari while they were running current Chrome. This was found by
signing in during Step 9's acceptance run, not by any test.

**So: `Sec-Fetch-Site` decides when the browser sent it, and `Origin`'s
host decides when it did not. A request carrying neither is still
refused.** Both headers are set by the browser and neither is settable by
page script, so a cross-site attacker can forge neither; the fallback
restores deployments Chrome's own rule had silently excluded rather than
loosening the check for the ones it already covered.

**The reasoning against the fallback was right about the mechanism, and
that part is kept.** Comparing `Origin` needs the coordinator's own
browser-facing origin, and the only honest source for it is `Host`. The
prohibition on trusting `X-Forwarded-*` is unchanged and is what the
comparison deliberately does not use. The predicted proxy failure was also
real, and arrived within minutes of the fallback landing: `ui/nginx.conf`
forwarded `Host $host`, which strips the port, so a browser on
`http://<ip>:18081` reached a coordinator that believed its host was
`<ip>`, and every sign-in was refused over a port mismatch. **The fix was
to stop the proxy rewriting `Host` (`$http_host`), not to start trusting a
forwarded header** — which is also what ADR-022 decision 3 asks for
independently, since removing the proxy must change nothing except the
origin.

Two further details, both chosen deliberately: the comparison is **host
only, never scheme**, because a cross-site origin differs in host by
definition while requiring scheme equality would break the TLS-terminating
proxy this record itself invites; and `Origin: null`, which a sandboxed
iframe or a `file://` document sends, carries no host and therefore never
matches.

**Login and bootstrap carry the same requirement, decided 2026-08-12 and
amended in.** This record originally left `POST /api/v1/session` uncovered,
which the section below on what implementation proved wrong records as an
omission rather than a judgement: `SameSite=Lax` governs whether a cookie is
*sent*, not whether one is *set*, so a cross-site form post to the login
endpoint makes the victim's browser hold, and the audit log attribute, the
attacker's principal. `POST /api/v1/bootstrap` has the same shape and creates
the first administrator. Both now require `Sec-Fetch-Site: same-origin` and are
rejected when the header is absent, which is the identical predicate the rule
above applies to every other write.

The point of adopting the identical predicate rather than a weaker one is that
there is **one** cross-site rule in this system rather than two. A
deny-known-bad variant, rejecting the header when it is present and wrong while
permitting its absence, protects the same browsers in practice and costs a
second rule that reads almost the same as the first. This project's recurring
defect is a near-duplicate that diverges quietly, and a security predicate is
the worst place to keep one.

The cost is real and is named here rather than discovered: a `curl` login must
pass the header explicitly, and a browser that sends no `Sec-Fetch-Site`, which
means Safari before 16.4, cannot log in at all. That population is already
barred from cookie-authenticated writes by the paragraph above, so the cookie
being refused could not have performed one; its path is decision 5's
bearer-paste break-glass, unchanged. A machine client authenticates with a
token and never posts to this endpoint at all.

Both endpoints shipped in v1 with Step 6, so requiring a header they did not
require yesterday is a **compatibility break inside a major version**, which
[ADR-020](ADR-020-control-api-shape-and-change-stream.md) decision 8 otherwise
forbids. It is recorded there as that record's one exception, with the rule
that keeps it from becoming a habit: a break inside a major version is
permitted only to close a security defect, and only when it is recorded at the
time it is taken.

What this does **not** address is the exposure decision 8 already governs:
login remains unauthenticated by construction, and a same-origin page can still
submit it. Cross-site forgery and login flooding are different problems with
different mechanisms, and this clause closes only the first.

**The bearer exemption is keyed on the credential that actually authenticated
the request, never on the presence of a header.** A bearer write carries no CSRF
exposure because nothing attaches an `Authorization` header automatically. But
an implementation that reads "if an `Authorization` header is present, skip the
CSRF check" is exploitable: URL userinfo makes a browser attach
`Authorization: Basic ...` to a top-level navigation, `SameSite=Lax` sends the
cookie alongside it, and a middleware that falls through to cookie
authentication has skipped the check on a cookie-authenticated write. So a
malformed or non-Bearer `Authorization` header is a `401` and never a fallthrough
to cookie authentication.

### 7. An authorization refusal is not coordinator loss, and must be a defined fallback trigger

This is the correction of an argument that is easy to make and wrong, and it is
recorded as its own decision because the wrong version is intuitive enough to be
reintroduced.

A coordinator outage is a **transport** failure: connection refused, timeout,
DNS failure. That is what an ADR-004 reduced local fallback detects and fires
on. A `401` or `403` is a **successful conversation with a healthy coordinator
that returns a refusal**, and nothing in ADR-004, ADR-016, ARCHITECTURE §8.2, or
the FPP plugin treats that as a fallback condition. So identity failure is not
equivalent to coordinator loss; left unaddressed it is strictly worse.

> **Corrected 2026-08-12, and the correction is load-bearing for anyone
> implementing this decision.** The sentence above beginning "That is what an
> ADR-004 reduced local fallback detects" is **false** for FPP's native `URL`
> command, which is the mechanism ARCHITECTURE §8.2 describes for FPP invoking
> a macro. FPP reads no HTTP status on any version, and on the deployed `9.4`
> and `9.5.3` it does not detect a transport failure either, so it reports
> success for a refused connection just as it does for a `403`. The conclusion
> below is unaffected. What changes is the scope of the obligation: a ShowMesh
> plugin on the FPP host must detect **both** the refusal and the transport
> failure, because FPP supplies neither. See "What implementation and research
> proved this record got wrong" and
> [RES-015](../research/RES-015-fpp-plugin-distribution-model.md) §7.2.

The scenario is ordinary rather than exotic. The operator rotates the
`scheduler` machine token in November and misses the FPP host. At 17:00 FPP
fires its native command to run `Begin Set`. The coordinator is up, healthy, and
green on the dashboard, and returns `403 missing scope show:macro:run`. The FPP
plugin sees a reachable coordinator and does not enter reduced local fallback,
because it has no reason to. The macro's coordinator-required steps do not run,
and its locally executable steps do not run either, which is worse than the
genuine outage would have been.

Therefore: **an authenticated refusal from the coordinator is a defined fallback
trigger.** A macro definition must specify behaviour for it exactly as ADR-016
requires coordinator-required steps to be labelled, and the FPP plugin and node
policy must treat `401` and `403` as "the coordinator is unavailable to this
caller" for the purpose of executing reduced local steps. The condition must
also be distinguishable in evidence and loudly surfaced, because a credential
fault presenting as a network fault sends the operator to the wrong place at the
worst time.

### 8. Login cost is bounded, and a principal is never locked out

`POST /api/v1/session` is unauthenticated by construction, and decision 1 makes
each attempt cost 64 MiB of memory-hard work and, under decision 11, a durable
audit row. Unbounded, that is a denial of the write surface available to anyone
on the VLAN with no credential, on an appliance also running the broker, the UI,
and the collectors. Reads being open by default means no reconnaissance is
needed to find the endpoint.

The standard mitigation is a per-principal lockout after N failures, and this
record's governing constraint forbids it: a lockout is an operator lockout at
showtime, and an attacker who knows the operator's principal name can trigger it
deliberately. That tension is real and is resolved rather than left implicit.

**Password verification is bounded by a concurrency limit and a per-source
increasing delay, never by a lockout.** A bounded number of argon2 verifications
may run at once, and further attempts queue rather than allocate. Repeated
failures from one source raise that source's response latency. Neither mechanism
can ever put a principal into a state where the correct password is refused, so
neither can strand the operator, and both make sustained guessing arbitrarily
expensive. A queued attempt that would exceed the bound is rejected with a
retry-after rather than being held indefinitely.

### 9. Bootstrap and recovery are host-level, never network-level

On first run with no principals, the coordinator generates a bootstrap code and
writes it **only to a file in the data volume**, with restrictive permissions.
It is **not written to the startup log.** OBSERVABILITY §13 requires logs to
avoid embedding secrets, a code sufficient to create the first administrator is
a secret, and the Compose bundle configures no log rotation, so a logged code
persists in the container's JSON log and in any log shipper indefinitely.
Reading a container log also requires the Docker socket, which is
root-equivalent, so the log adds reach without adding convenience over the file.

The code is **single-use and invalidated on first successful admin creation, at
which point the file is deleted**, and it carries an expiry. "One-time" is
otherwise ambiguous between generated-once and usable-once, and only the second
is safe. There is deliberately no setup mode that stands open to the network
until somebody claims it, because that is a window in which anyone on the VLAN
becomes administrator; a code that never expires in a file that is never deleted
is the same window, quieter.

**An unclaimed bootstrap state is loud and persistent**: a UI banner and a
repeated warning log, not one line in the first ten lines of a startup log. The
case that makes this necessary is a volume loss, a `docker compose down -v`, or
a move to a fresh host. The coordinator returns to zero principals and
regenerates a code, the audit log is gone, and **because reads stay open the
dashboard still renders and nothing looks wrong.** Without a loud signal the
operator has none.

Lockout recovery is a coordinator subcommand run against the data volume on the
host, requiring filesystem access, which is equivalent to owning the deployment.
It is **not reachable over the API at any scope**.

**What lockout costs is bounded by decision 7 and by RES-009, not by an
assurance.** ARCHITECTURE §2.4 requires a running show to continue through
coordinator loss, FPP retains scheduling authority under ADR-001, reads stay
open so the display remains observable, decision 7 makes a refusal fire local
fallback, and decision 5 keeps a break-glass credential path on the phone. That
is the argument. It is an argument from requirements, not from measurement:
[RES-009](../research/RES-009-failure-mode-testing.md) is unresearched, no step
so far has claimed to verify anything about a running show, and this record does
not claim it either.

### 10. The MQTT control plane drops anonymous access

The bundled Mosquitto sets `allow_anonymous false`. Each agent holds its own
credential with an ACL permitting it to publish only beneath its own
`showmesh/nodes/<node-id>/…` prefix and to subscribe only to its own `cmd`
topic. The coordinator holds a separate credential, and no client other than the
coordinator may publish to any `cmd` topic. This closes the deferred item from
the control-plane skeleton in which any client with publish rights could create
unbounded node rows on arbitrary node IDs.

The stronger reason for the per-node ACL is worth stating, because the
unbounded-rows item is the weaker one: **a node is a Pi in a weatherproof box in
a front yard, physically reachable by anyone walking past.** The ACL is what
bounds what a stolen one can do.

**Three ACL mechanics are decided here rather than left to whoever writes the
file**, because two of them fail silently and one of them is self-granting:

- The pattern substitution is **`%u`, the username, never `%c`, the client id.**
  A client chooses its own client id, so a `%c` pattern grants whatever the
  client claims.
- **`pattern` rules apply to every user regardless of preceding `user` lines**,
  so the coordinator's broader grants must be per-`user` rules, and a stray
  global `topic` line grants it to everyone.
- The `%u` pattern is only safe because a broker username equals a node id and
  node ids are validated against a character class excluding `+`, `#`, and `/`.
  That mitigation exists today by luck of an earlier decision, and is cited here
  because a hand-created broker username is not bound by that validator.

**FPP is a first-class client of this broker, not an exception to it.** ADR-008
recommends one broker carrying both FPP telemetry and ShowMesh traffic, so an
operator following it puts FPP on the bundled broker. The ACL therefore includes
an FPP publisher role limited to FPP's own status topics, with no access to any
`showmesh/` topic. Without it, `allow_anonymous false` would silently cut off
FPP's MQTT output on upgrade, and decision 10's own reopening condition below
would be met by the topology ADR-008 recommends.

**Broker credentials are generated per deployment at first run, never shipped in
`deploy/`.** A credential set authored into the bundle would be identical in
every installation, making the ACL decorative, which is the failure ADR-021
named by name when it rejected a mandatory shared secret with no distribution
mechanism. The generated mapping is a secret: excluded from YAML export bundles
under ADR-009, and covered by decision 3's redaction rule.

**The bundle's own broker healthcheck breaks on this change, and it is named
because the record claims to know what it breaks.** The shipped healthcheck runs
`mosquitto_sub` against `$SYS/#` with no credentials, so it fails twice over
once anonymous access is off: no credential, and `$SYS` needs an explicit grant
the per-node pattern will not supply. It must gain a dedicated healthcheck
principal with a read grant on `$SYS`, and the credential must not be passed on
a command line where `docker inspect` exposes it.

**Two limits on what "revocable" means here**, both of which change operator
expectations. Mosquitto re-reads its password and ACL files on `SIGHUP` but does
**not** re-authenticate already-connected clients, so revoking a compromised
agent credential mid-season takes effect only when that agent reconnects, or on
a broker restart that drops the whole fleet and flips it to `unknown` at once.
And hand-provisioned per-node credentials on controllers mounted in a yard
require a physical visit to rotate, so in practice they will not be rotated
until enrollment automation lands: they are permanent credentials with a
revocation story, not rotating ones.

**Agents must distinguish rejection from unreachability, and rejection must not
be fatal.** Before this change, "the broker will not take my connection" was
always transient. `allow_anonymous false` creates a permanent, self-inflicted
rejection that presents identically. An agent receiving CONNACK reason code
`0x87 Not authorized` continues on its ADR-009 cached fallback subset rather
than treating the condition as fatal, and surfaces it as evidence distinct from
an unreachable broker. The same applies to the coordinator: CLAUDE.md's standing
constraint that it starts and stays up with no broker reachable now extends to a
broker that **rejects** it. An ACL denial is quieter still, since Mosquitto
accepts the connection and discards the publish, so agents must surface v5
authorization reason codes distinctly. The failure this prevents is an operator
at 21:00 seeing "node offline", which looks exactly like a dead switch port, and
spending the set chasing the wrong fault.

**Residual risk, recorded rather than solved.** With ACLs alone, broker trust is
the boundary: anyone holding the coordinator's broker credential can forge a
command to any node, and an agent cannot tell. Message-level command
authentication would close it and is not decided here, because key distribution
and rotation deserve their own evidence. The condition that must reopen this is
**any client on the ShowMesh control-plane broker that is not the coordinator, a
ShowMesh agent, an FPP publisher under the role above, or the healthcheck
principal.**

**The FPP MQTT collector's credential is outside this boundary, and the existing
mitigation addresses the wrong half of the problem.** The connection is
subscribe-only by construction: no publish method on the interface, no Last
Will, and explicit per-suffix subscription filters rather than a host-scoped
`#`. Every one of those is a property of ShowMesh's code, and none of them binds
anyone who **reads the credential** from `.env`, from `docker inspect`, or from
a backup of the data volume. That credential is a shared home-automation account
with publish rights, on a broker where a topic drives the display, so the real
exposure is that reading the coordinator's environment yields command authority
over every FPP host in the house. The code-level construction is good and does
not touch it.

What does touch it is a broker-side control ShowMesh cannot enforce and must
therefore require in documentation: **a dedicated account on the foreign broker
with a read-only ACL scoped to FPP's status topics, used by nothing else.** It
must never be the operator's general homelab account, which is what it is today,
and it must never be reused as the ShowMesh control-plane credential. The two
brokers must never be merged; merging them would look like a tidy-up and would
import the home-automation broker's entire publish surface into the show's
control plane.

### 11. Every write is audited, with a named safety class that is never refused for want of attribution

ARCHITECTURE §8.1 already requires a command to carry an issuer. This record
makes that issuer a principal id, and makes the resulting entry durable and
readable.

The audit log is **append-only**, and an entry records the timestamp, the
principal id and display name, the credential form and which specific session or
token was used, the client address where the deployment declares a trusted
source for it, the action, the target, the parameters with secrets redacted, and
the idempotency key, which ARCHITECTURE §8.1 requires on every command rather
than on some of them. Authentication failures are audited, and so is every
principal, token, and session mutation. An audit log that records only success
cannot show an attempt.

**Outcome is a second entry, not a mutable field.** ADR-003 is explicit that a
command is not successful because it was sent, so the outcome is not known when
the dispatch entry is written. Dispatch and outcome are separate append-only
entries correlated by command id, and an outcome that never arrives, because the
coordinator restarted between dispatch and confirmation, carries an explicit
state and reason from ADR-020's vocabulary rather than a null that renders as
blank. **An idempotent replay writes its own entry marked as a replay that
dispatched nothing**, because the replay is precisely the case an investigator
wants to see: it means the operator did not get their response.

**Client address is an attribution problem the proxy creates.** Behind the UI
container, every request arrives from the proxy, and `ui/nginx.conf` forbids the
coordinator trusting `X-Forwarded-For` for a rate limit, an audit record, or an
authorization decision. So an audit entry records **who** and, by default,
cannot record **from where**. A deployment may declare a trusted proxy address
to recover it, which makes the proxy an attribution source without making it a
security boundary, and the distinction must be kept: a declared-trusted address
may fill an audit field and may never grant a scope.

It is exposed at `/api/v1/audit`, requires `audit:read`, and is **not** carried
on the change stream. **The audit log is not the event history**, and they must
not be merged: the event history records what the system observed, the audit log
records who asked, and collapsing them makes "what happened" and "who is
accountable" the same query with different failure modes.

**Audit retention is bounded before the first write endpoint ships.** It cannot
be deferred to [RES-013](../research/RES-013-telemetry-storage-and-alerting.md)
with the rest of the retention design, because the rule below makes an
audit-write failure gate commands, and an unbounded table that gates commands is
a scheduled outage. Free disk space must also be an observed signal with an
alert, not a surprise.

**A write that cannot be attributed does not proceed, except within a named
safety class.** For a coordinator-local state change the audit entry is written
in the same transaction, so the two succeed or fail together; for a command
dispatched to an agent the dispatch entry is written before dispatch. If that
write fails, the command is refused.

**Blackout, stop, and power-off are exempt.** They proceed with a degraded
attribution record written to stderr and flagged as incomplete, and are never
refused for want of an audit write.

The exemption exists because the unqualified rule inverts this architecture's
safety direction, and that is worth stating plainly rather than burying. ADR-008,
ARCHITECTURE §2.4, and ADR-004's local fallback all degrade toward *the show
continuing*. An unqualified audit gate degrades toward *the operator being
unable to stop it*, and ARCHITECTURE §12 Phase 1, which this record exists to
unblock, contains blackout. The reasons an operator reaches for blackout are a
tripped eFuse, an overheating projector, a noise complaint at 22:00, or somebody
at the door, and "the show survives it" is the wrong success criterion for every
one of them. The trigger is not hypothetical either: ARCHITECTURE §11 names disk
exhaustion as a required failure case, the Compose bundle configures no log
rotation for either container, Mosquitto logs to an unrotated volume, and
SQLite does not return freed pages to the filesystem without a vacuum. On a full
disk the unqualified rule refuses every command including blackout while the
dashboard renders green.

Fail-closed is kept where it is right, which is `config:write` and
`principal:write`. Changing who may act, with no record of who changed it, is
the case the audit log exists for.

**AMENDED 2026-08-26, owner ruling: audit-store unavailability never blocks
an action, on any request path, not only the three-member safety class
above.** The ruling, in the owner's own words: "Audit log database becoming
unavailable SHOULD NOT STOP ANY ACTIONS, rather than stopping it should be
LOUD in the UI and non-audit logs about it. Audit logging is NOT a show
stopping issue, hopefully its loud enough in logs and UI that the operators
know to fix it before the next show, but again it should NOT stop the show
or any actions from running. If the audit log being down currently blocks
actions, that must be corrected."

This exemption is scoped to the `ErrAuditWrite` class specifically: an
audit_log append that itself failed. A `BeginTx`-class failure (the
store's one connection could not even open a transaction) or an
audit-params marshal failure still returns a 500 on all five paths below,
because neither is the sentinel each path's fallback recognizes. This is
defensible rather than an oversight: `InsertCommand` shares the identical
SQLite file, so nothing could have been durably recorded either way, and
a fallback for a failure this narrow amendment's own mechanism cannot
even detect is not the same claim as the `ErrAuditWrite` fallback makes.

Five request paths still failed closed on the pre-dispatch write, contrary
to that rule: `POST /api/v1/actions/{id}/invocations` (direct show.action
invoke) for an action whose stored `safetyClass` was `"none"`, the audio
session command dispatch behind `POST /api/v1/nodes/{nodeId}/audio/sessions/{sessionId}/*`
for an action outside this subsystem's own blackout-equivalent set
(`audio.session.stop`, `audio.session.clear`, `audio.output.mute`),
`POST /api/v1/fpp/{instanceId}/commands` for a primitive outside decision
11's own named safety class, `POST /api/v1/resolume/actions` for an action
other than `blackout`/`clearLayer`, and the four night-session
admission-opening commands (`prepare-site`, `run-readiness`,
`start-preshow`, `start-night`) behind `POST /api/v1/night/session`. This
amendment removes the refusal on all five. Every command dispatch on every
one of those paths now proceeds regardless of `safetyClass`, using the
identical fallback the safety-class exemption above already established:
the pre-dispatch write is redone through a plain, non-transactional
insert (or, for the two night-session commands whose own decision runs
inside the failed transaction, a non-transactional redo of that
transaction's already-computed persist step alone), the command
dispatches normally, and the outcome is the ordinary one dispatch would
have produced with a healthy audit store. The distinction this record's
own safety class used to carry (exempt vs. refused) survives only as a
distinction in *why* attribution is reported degraded, not in *whether*
the command runs. This covers every request path ADR-024 decision 11
itself governs; it does not touch `config:write` or `principal:write`,
which decision 11 already keeps fail-closed for an unrelated reason (see
below).

This is a widening of the exemption, not a second mechanism next to it.
The same stderr line and the same wire flag (`attributionDegraded`) that
already report a safety-class exemption's degraded attribution now report
every other command's degraded attribution too, with a reason string
naming which of the three justifications applies (the named safety class,
a macro run's own decision under ADR-035, or this amendment) so an
investigator can still tell them apart in the record.

**A per-action `attributionDegraded` flag answers a narrower question than
the one this amendment exists to make loud.** It says "was this one action
unaudited," which only an operator who has just issued a command can read.
It does not say "is the audit store down right now," which is what an
operator needs to learn *before* deciding whether to trust the log later,
without having to act first to find out. So the coordinator also carries a
standing, coordinator-wide signal on `GET /api/v1/snapshot`
(`auditStore.state` / `auditStore.reason`, shipped in
IDENTIFIER-REGISTER.md as `coordinator.audit.store.state` /
`coordinator.audit.store.reason`), computed fresh on every request via a
real probe write to audit_log (an INSERT inside a transaction always
rolled back, never committed), not read from a latch fed by whatever
command traffic happened to pass through recently: `"usable"` if that
probe just now succeeded, `"unusable"` with a reason if it did not. There
is no third state; a probe always produces a definitive answer, so a
coordinator that has made no real audit write since startup still reports
its true state rather than a placeholder for one. That is the surface an
operator can see without touching a control at all, matching decision 9's
identical "loud and persistent" requirement for an unclaimed bootstrap
state.

`config:write` and `principal:write` are unaffected: the paragraph above
keeping them fail-closed is not one of the five paths this amendment
names, and this amendment does not extend the exemption to either one.

### 12. Authorization is expressed server-side, returned to the client, and carries freshness

`GET /api/v1/session` returns the current principal, its role, and its effective
scopes. OPERATOR-UI §14 requires that the API and UI architecture not block
future role-based access, and says that in practice this means authorization
decisions are expressed server-side and returned to the client rather than
inferred by it. §11 separately forbids hiding a control as a security measure.

**The scope list is subject to the same evidence rules as everything else the
API returns**, and this is stated because it is the one piece of server-returned
state that would otherwise escape them. A scope list has a freshness, and a
stale or unavailable one **renders as unknown, never as permissive**. Defaulting
a control to enabled because the client last heard it was allowed is precisely
the "blank reads as fine" failure ADR-011 and ADR-020 decision 5 exist to
prevent, applied to authorization instead of telemetry, and it reintroduces the
failure decision 5 of this record is built around: an admin demotes a principal
at 18:00, the browser has held a stream since 16:00, every control still renders
enabled, and the operator discovers it by pressing Blackout at 21:00. A role
change or a principal being disabled increments the generation counter in
decision 5, which closes open streams and forces a re-fetch, so the stale window
is bounded rather than indefinite.

The UI may render an action the principal cannot perform as disabled with a
stated reason. It must not silently omit it. An absent control is
indistinguishable from a feature that does not exist, and an operator debugging
at nine in the evening needs to know the difference between "ShowMesh cannot do
this" and "you may not do this."

The coordinator enforces regardless of what the client renders. A client that
ignores the scope list receives `403` with an RFC 9457 problem document naming
the missing scope, distinct from the `401` that means no valid credential was
presented.

## What implementation and research proved this record got wrong

Step 6 implemented this record and three reviews attacked the result. Source
verification of FPP itself, recorded in
[RES-015](../research/RES-015-fpp-plugin-distribution-model.md) on 2026-08-12,
then corrected a further argument. Everything below is recorded here rather
than quietly fixed, because a decision record that is silently wrong is worse
than one that is visibly incomplete.

- **The restore claim in decision 5 was false**, corrected in place above.
- **`POST /api/v1/session` has no cross-site protection, and this record never
  addressed it.** `SameSite=Lax` governs whether a cookie is *sent*, not whether
  one is *set*, so a cross-site form post to the login endpoint with an
  attacker's credentials makes the victim's browser hold, and the audit log
  attribute, the attacker's principal. Decision 5's persistent "signed in as"
  banner makes the substitution visible rather than silent, which is a mitigation
  by accident rather than by design. The record covering the first write endpoint
  must decide this deliberately.

  **Decided 2026-08-12, and amended into decision 6 above rather than settled in
  code**, because "which requests may set a session cookie" is a durable
  constraint and the next person to touch the login handler needs to find it
  here. The answer is the strict one: the same `Sec-Fetch-Site: same-origin`
  requirement every other write already carries, rejecting on absence, applied to
  `POST /api/v1/session` and to `POST /api/v1/bootstrap`. The finding is left
  standing above rather than rewritten, because the omission was real and the
  record having missed it is the part worth remembering.
- **Decision 7 exists nowhere in the code.** No macro format, no FPP plugin, and
  no node policy classifies a `401` or `403` as "the coordinator is unavailable
  to this caller". That is defensible, because Step 6 ships no macro and no
  plugin, so there is nothing to trigger. It is recorded as an explicit
  obligation on the step that adds the first consumer of `show:macro:run`,
  because a correction this record was reshaped around should not survive only
  as prose in an ADR nobody rereads.
- **Decision 7's asymmetry does not exist at the mechanism level, and the
  decision is written as though it does.** The argument runs: a coordinator
  outage is a transport failure, *which is what an ADR-004 reduced local
  fallback detects and fires on*, whereas a `403` is a successful conversation
  that fires nothing, so the refusal case is strictly worse than the outage.
  The second half is right. The clause in italics is not.

  Source-verified in FPP's `src/commands/MediaCommands.cpp` at tags `9.4` and
  `9.5.3` and at `master`, and re-verified independently during fold-in:
  `CURLINFO_RESPONSE_CODE` appears **zero times on every ref**, so a `401` is
  `CURLE_OK` and is indistinguishable from a `200` by any code in FPP. On `9.4`
  and `9.5.3`, which is what the deployed fleet runs, `isError()` is
  `m_curl == nullptr || m_curlm == nullptr`, which tests handle setup only, so
  **a URL command that hits DNS failure, a timeout, or connection refused also
  reports success.** FPP's own comment on `master` states the consequence:
  `isError()`/`isDone()` "only look at handle setup and CURLMSG_DONE, not the
  HTTP status or transfer result." On the scheduler and preset paths the
  returned `Result` is discarded outright, so there is nowhere to put a check
  even if the status were readable, and an unresolvable command marks the
  playlist item finished exactly as a success would.

  So through FPP's native `URL` command **neither** failure fires anything,
  because FPP detects neither. This record assumed a detection capability and
  did not check whether it existed. The conclusion survives unchanged, `401`
  and `403` must be defined fallback triggers, and the obligation grows: **so
  must transport failure**, because nothing in FPP supplies it either. Both
  detections must live in ShowMesh-authored code on the FPP host, which puts
  RES-015 on the critical path of the step that discharges this decision rather
  than beside it.

  **DISCHARGED 2026-08-15, on evidence from Step 9's acceptance run of
  2026-08-14** (recorded in the dated BUILD-LOG entry for that day; merged to
  `main` on 2026-08-15). Acceptance criteria 8 and 9 both passed: the FPP
  plugin's local status record distinguishes four classes rather than
  collapsing them, so a `401`, a `403`, a transport failure, and a successful
  call are each separable in evidence on the FPP host itself; and its failure
  buffer survives a `404` and a `409` and flushes into coalesced events on the
  next `2xx`. Both detections live in ShowMesh-authored code on the FPP host,
  which is what this paragraph said they would have to. **Verified against the
  bench `fppd` container and not on real show hardware**, per the standing
  owner decision that real-host install stays deferred, so RES-015 stays L1.

  The generalizable shape is worth stating, because it is the third variant of
  one defect this project keeps meeting. This decision exists because the first
  draft made **an argument against the wrong failure direction**. This is an
  argument against **a failure-detection capability assumed rather than
  checked**. Both times the conclusion happened to survive and the reasoning did
  not, and only the second kind is caught by reading the other system's source.
- **A credential on an FPP host is effectively public, and this record's
  `scheduler` principal is written around one living there.** Also from
  RES-015. FPP's native `URL` command never sets `CURLOPT_HTTPHEADER` on any
  ref, so no `Authorization` header is possible; the only way to attach a
  credential is in the URL itself, and **every command execution publishes its
  arguments in cleartext to MQTT `command/run`** from every trigger source. Add
  that FPP writes config files world-readable at `0664`, that
  `GET /api/configfile/**` streams any config file with no allowlist, that the
  web UI is unauthenticated by default, and that backup redaction is an exact
  key-name list containing only `emailpass`, `password`, and `secret` on the
  deployed versions, and a ShowMesh token placed on an FPP host must be treated
  as readable by anyone who can reach that host. The deployment posture accepts
  cleartext on an isolated show LAN for commands and status, and does **not**
  accept it for a credential, so the native `URL` command is unusable for an
  authenticated call and a ShowMesh-authored plugin is required rather than
  merely preferable. The `scheduler` principal's scope bundle should stay as
  narrow as decision 4 permits, and its credential should be cheap to rotate.
  Improving FPP's own posture is upstream work and is explicitly out of scope.
- **Decision 11's same-transaction audit rule is not achieved**, and the layering
  is why: the API package cannot reach the transaction boundary, which identity
  and store own. Session and bootstrap writes audit around the commit rather than
  inside it, so an audit failure on a bootstrap claim leaves the first
  administrator existing with no record of its creation. The blackout, stop, and
  power-off exemption is implementable whenever it is needed, since the audit
  write is a handler-level call rather than a store constraint, but nothing calls
  it yet. Closing the atomicity gap requires `identity.Service` to grow an atomic
  variant, and it must close before the first fail-closed write endpoint.

## Consequences

- **ADR-021 rule 5 is lifted.** The first write endpoint becomes possible, which
  is the entire point of this record and the reason it is the critical path out
  of Phase 0.
- **ADR-022 decision 4's semantics are deleted and its input affordance is
  kept.** Decisions 1, 2, 3, and 5 of that record survive, as it predicted.
  Rule 2 is tested harder than before: the proxy now forwards `Cookie` and
  `Set-Cookie`. `ui/nginx.conf` sets no `proxy_cookie_path` or
  `proxy_cookie_domain` today and needs no change, but adding either later would
  break login in a way that presents as a session that does not stick.
- **Two accepted documents are knowingly left partially met, not one.**
  ARCHITECTURE §10.4 moves from unmet to partial: authenticated identities yes,
  authorization by action yes, authorization by target no, and "agents accept
  only allowlisted operations" **not delivered here**, since the ACL restricts
  who may publish to an agent's command topic while the agent validating the
  operation itself is agent-side work this record does not discharge.
  OBSERVABILITY §13 is conditionally met on its telemetry-authentication clause,
  because reads stay open by default, and its least-privilege clause is **not**
  met for the FPP collector credential: decision 10 requires a dedicated
  read-only account in documentation and cannot enforce one on a broker ShowMesh
  does not administer.
- **The Mosquitto change breaks the shipped bundle, not only existing
  deployments.** The broker healthcheck, FPP's own MQTT output where an operator
  followed ADR-008's one-broker recommendation, and every agent without a
  provisioned credential. The migration belongs in `deploy/` documentation
  before the release that carries it.
- **A coordinator that still sees `SHOWMESH_API_TOKEN` refuses to start.** This
  is the most operator-visible upgrade hazard in the record and the one most
  likely to be met at an inconvenient moment. It is chosen over the alternative
  of silently reopening a closed read API.
- **Agents now need credentials**, which is provisioning work
  [RES-008](../research/RES-008-configuration-model.md) owns. Operator-provisioned
  per-node credentials ship first, and are permanent in practice until
  enrollment automation lands. Whoever builds that automation inherits a
  constraint worth recording now: delivering broker credentials from the
  coordinator would give a rebooting node a boot-time coordinator dependency it
  does not have today, which ARCHITECTURE §11's power-restoration case would
  find.
- **A password store, a session table, and an audit table are new persistent
  state** with real backup and secret-handling obligations. ADR-009 keeps
  secrets out of exported bundles, so password hashes, token hashes, session
  records, the broker credential mapping, and the bootstrap file must be
  excluded from YAML export explicitly rather than by omission.
- **Multi-operator becomes expressible but stays untested.** One operator is the
  only real consumer, and every claim beyond that is design intent.
- **A login screen exists where none did**, and it appears only when the
  operator acts, never when they look. That asymmetry is a direct consequence of
  reads staying open and is the most visible operator-facing effect.
- **The cookie makes `EventSource` viable again**, which is a trap. ADR-021's
  consequence that a browser must read the stream with `fetch` was a
  header-driven constraint, and a cookie removes it. Anyone who later
  "simplifies" the hand-rolled reader to `EventSource` silently breaks every
  bearer-token client's access to the stream.
- **Documents describing the retired posture become wrong the moment this is
  implemented, not the moment it is accepted.** `api/openapi.yaml`'s
  `bearerAuth` description, `SECURITY.md`, and `deploy/README.md` all currently
  describe ADR-021's shared secret, and all are currently accurate. Updating
  them is the implementing step's obligation, and doing it earlier would replace
  a true description with a false one.
- **Failure testing gains cases it did not have**: lockout, a session revoked
  mid-show, a session revoked against an open change stream, broker credential
  rotation with a show running, an agent rejected by the broker at boot, a
  coordinator restart underneath an active session, a database restore against
  the session generation counter, and an audit-write failure under disk
  exhaustion with a blackout attempted. These belong to
  [RES-009](../research/RES-009-failure-mode-testing.md), which is unresearched,
  and none of them is verified by this record.

## Alternatives considered

**Everything authenticated, reads included, with no opt-out.** The
straightforwardly stronger posture, and it would close OBSERVABILITY §13
properly. Rejected on three counts: it breaks every existing v1 read client in a
deployment running open, which is the default, and so would force either
`/api/v2` or a transitional open mode that is the thing being removed; the
disclosure gain on an isolated show VLAN is modest; and it makes a credential
problem cost the operator visibility at the moment they most need it. The cost
of rejecting it is real and is recorded in the consequences rather than argued
away.

**API tokens only, with no sessions and no passwords.** Genuinely tempting: no
password storage, no session table, no CSRF surface at all, no argon2 denial
surface, and ADR-022 decision 4 would survive rather than be deleted. Rejected
on the same constraint that produced decision 4 in the first place. A token in
`sessionStorage` is re-entered per tab and per device, and "paste a forty
character secret" is the single worst thing to ask of someone on a phone in the
cold. Keeping a token in `localStorage` instead would fix the persistence and
make it worse overall: a long-lived bearer credential readable by any script on
the origin is strictly weaker than an HttpOnly cookie for the same convenience.
The affordance survives as break-glass in decision 5, which is the part of this
alternative worth keeping.

**An external OIDC provider as the primary identity source.** The strongest
identity story and the one that scales past one operator. Rejected because it
adds a runtime dependency for operator access to a system whose defining
property is surviving the loss of everything non-essential, and because an
open-source operator with no identity provider needs the self-contained path to
exist anyway, at which point the provider is an extension, which is what it is
here.

**Trusted reverse-proxy forward-authentication.** The cheapest thing to build.
Rejected because it makes the proxy part of the trust decision, which ADR-022
rule 2 was written specifically to prevent and which OPERATOR-UI §11's "the UI
is not a security boundary" covers by extension, the proxy shipping inside the
UI container. It also makes the coordinator unsafe to address directly, which
breaks ADR-014's usable-with-no-UI property and ADR-022 decision 3's removal
test in one move.

**An `Origin`-versus-`Host` comparison as a CSRF fallback.** Rejected in
decision 6: it cannot be implemented without trusting a forwarded header, and
its failure mode under the deployment ADR-022 decision 5 invites is every write
returning `403` while every read works.

**Per-principal lockout after N failed logins.** The conventional answer, and
rejected in decision 8 because it is an operator lockout at showtime that an
attacker can trigger on purpose. A concurrency bound plus per-source latency
achieves the same economics without a state a correct password cannot escape.

**Refusing every write when the audit store is unavailable.** This was the
record's first position and it was wrong. Rejected in decision 11: it points
degradation at the operator's ability to stop the show, on a system where every
other degradation points at the show continuing.

**Signed command envelopes now, so broker compromise cannot forge a command.**
Rejected as scope rather than on merit. Key distribution and rotation deserve
their own evidence, and per-client ACLs close the exposure that is actually
reachable today, which is anonymous publish. The condition that reopens it is
named in decision 10 rather than left to judgement.

**Mosquitto authentication backed by the coordinator's SQLite store.** Rejected:
it needs a C broker plugin against ADR-012's pure-Go posture, and it would leave
agents unable to reconnect to the control plane whenever the coordinator is
down.

**Short sessions with refresh tokens.** Recorded explicitly because it is the
first thing a future contributor will propose. Rejected: it is an idle timeout
wearing better clothes, and its failure mode is a refresh that fails while the
operator is outdoors with a phone and no keyboard.

## Supersession

This record supersedes ADR-021, carrying forward its rules 3 and 4 and its CORS
posture in decision 3, and supersedes ADR-022 decision 4's semantics while
keeping its input affordance.

A future record must revisit three things: target-scoped authorization once
there is a consumer to design its taxonomy against; message-level command
authentication under the condition named in decision 10; and OIDC or
forward-authentication if ShowMesh acquires a genuinely multi-operator
deployment. None of the three is blocked by anything decided here, which is the
property this record was shaped for.

## Related research

[Configuration model](../research/RES-008-configuration-model.md) ·
[Failure testing](../research/RES-009-failure-mode-testing.md) ·
[Telemetry storage and alerting](../research/RES-013-telemetry-storage-and-alerting.md) ·
[FPP plugin distribution](../research/RES-015-fpp-plugin-distribution-model.md)
