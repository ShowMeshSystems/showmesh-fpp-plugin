# Security Policy

## Current posture, stated plainly

ShowMesh is pre-alpha. Its security posture is deliberate and recorded, but it is **not** a posture suitable for exposure to an untrusted network.

**There is still no show write operation.** Nothing reachable through the API can change a device, a playlist, or a show. What [ADR-024](docs/decisions/ADR-024-identity-authorization-and-audit.md) added is the mechanism that will permit one: identity, authorization, audit, and broker authorization. The only non-GET routes are `POST`/`DELETE /api/v1/session` and `POST /api/v1/bootstrap`, none of which touch a show.

**Writes require an authenticated principal, with no opt-out.** Reads keep the previous posture: open by default, closable with `SHOWMESH_API_CLOSE_READS=true`. Reads staying open is an operational safety decision rather than inertia. A credential problem must never cost the operator visibility of a running show, so the failure is scoped to "you cannot act" instead of a blank screen indistinguishable from a dead coordinator. The honest cost is that a default deployment discloses its operational model to anything on the VLAN.

**`SHOWMESH_API_TOKEN` is retired, and a coordinator that still sees it set refuses to start**, naming the migration. That is the harshest of the three available behaviours and the only one that cannot silently reopen a read API an operator deliberately closed. If you are upgrading, remove it from your environment; set `SHOWMESH_API_CLOSE_READS=true` if you want reads to require a credential, then create your first administrator from the one-time bootstrap code.

**Identities are coordinator-local, in two credential forms.** A browser holds an `HttpOnly` session cookie minted by the coordinator; `showmeshctl`, automation, and machine principals hold API tokens (`SHOWMESH_CTL_TOKEN` for the CLI). Principal kind does not restrict credential form: a human may mint a token and act from a terminal, so the audit log records a person rather than a robot. Authorization is by scope, with roles as named scope bundles. This satisfies ARCHITECTURE §10.4 for **action** and explicitly **not** for target, which ADR-024 records as partial rather than letting it look like compliance.

**The show VLAN is still the actual security boundary.** That is the design assumption, not a workaround.

**Cleartext on that VLAN is accepted for everything except a credential, and that exception is a hard rule.** Commands, macro invocations, playback status, telemetry and event traffic all cross the show LAN in the clear, and that is a deliberate decision rather than an oversight: this is a holiday light display on an isolated network, and encrypting the command path would buy little against the threat model while adding operational failure modes to a system whose whole architecture points at the show continuing. **A token, password, session cookie, or any other secret is different, and must never be transmitted in cleartext or placed anywhere that will republish it in cleartext.** The distinction matters because the second failure is not contained by the VLAN: a leaked credential outlives the packet that carried it, and can be replayed later by anyone who captured it once. Anything that would put a secret on the wire in the clear is a defect, not an accepted risk.

**A ShowMesh credential placed on an FPP host must be treated as readable by anyone who can reach that host, and that is FPP's posture rather than ours.** Source-verified at FPP 9.4, 9.5.3, and master, recorded in [RES-015](docs/research/RES-015-fpp-plugin-distribution-model.md) §7.4: FPP publishes every command execution's arguments in cleartext to its own MQTT `command/run` topic from every trigger source; it writes config files world-readable at `0664`; `GET /api/configfile/**` streams any file under the config directory with no allowlist; its web UI and API are unauthenticated by default; and its backup redaction is an exact key-name match list containing only `emailpass`, `password`, and `secret` on the deployed versions, so a ShowMesh-named key is not redacted and backups download as plaintext JSON. FPP's own precedent agrees: it documents a GitHub personal access token as stored plaintext in its settings file.

Two consequences ShowMesh acts on rather than merely notes. **FPP's native `URL` command cannot be used for an authenticated call**, because it can set no `Authorization` header on any version, so the only way to attach a credential is in the URL, which the `command/run` publish then broadcasts in the clear. That is the rule above being violated, so the ShowMesh FPP plugin owns authenticated calls instead. And the `scheduler` principal's scope bundle stays as narrow as [ADR-024](docs/decisions/ADR-024-identity-authorization-and-audit.md) decision 4 permits, with a credential that is cheap to rotate, because the host it lives on cannot keep it secret. **Improving FPP's own posture is upstream work and is explicitly out of scope for this project.**

**The bundled Mosquitto no longer allows anonymous access.** It requires a credential and enforces an ACL with four principal classes: each agent confined to its own node's topics and explicitly excluding its own command topic, the coordinator as the only client that may publish a command, an FPP publisher role, and a read-only healthcheck principal. Credentials are generated per deployment by `deploy/mosquitto/generate-credentials.sh` and are never authored into this repository, because a credential shipped in the bundle would be identical in every installation.

**Broker trust is still the boundary for command authenticity.** Anyone holding the coordinator's broker credential can publish a forged command to any node, and an agent cannot tell. Message-level command authentication would close that and is deliberately not built; ADR-024 decision 10 names the condition that must reopen it.

**ShowMesh terminates no TLS**, in the coordinator container or the UI container. A deployment that needs TLS puts its own reverse proxy in front. With no TLS, the session cookie is readable on the wire, which is one more reason the VLAN is the boundary. Where TLS is terminated in front, set `SHOWMESH_API_SECURE_COOKIE=true`.

**The UI container never holds a credential.** It serves static assets and forwards `/api/*` to the coordinator unchanged. It forwards credentials; it never holds, injects, mints, validates, or refreshes one, and cannot be configured with one. If it could, reaching the UI would be equivalent to reaching the API.

## Deploying it safely

1. Put the coordinator, the broker, and the UI on an isolated show VLAN with no route in from a general-purpose network.
2. Run `deploy/mosquitto/generate-credentials.sh` once before the first `docker compose up`. The bundled broker will not start without it.
3. Provision each agent its own broker credential with `deploy/mosquitto/add-agent-credential.sh <node-id>`.
4. Claim the one-time bootstrap code from the coordinator's data volume to create the first administrator, then delete nothing else: the coordinator removes the file itself on a successful claim.
5. Consider `SHOWMESH_API_CLOSE_READS=true`. Weigh it honestly against the paragraph above: it closes a disclosure gap, and it means a credential problem costs you visibility of the show at the moment you least want to lose it.
6. Treat an exported configuration bundle as non-secret. Exports exclude secrets by default (ARCHITECTURE §10.4, [ADR-009](docs/decisions/ADR-009-sqlite-configuration-storage.md)), and keeping secrets in `deploy/.env`, which is gitignored, is the normal path. An explicit opt-in to include secrets in an export is a supported option ([RES-008](docs/research/RES-008-configuration-model.md) decision D4), and choosing it produces a plaintext credential file whose only protection is where you put it, so store and transfer it accordingly and do not commit it. Note that no export bundle exists yet, so all of this is an obligation recorded for whoever builds one rather than code.
7. If the UI must be reachable over TLS, terminate it in a reverse proxy you control, and set `SHOWMESH_API_SECURE_COOKIE=true`.

## What is out of scope for a report

These are known and documented, not vulnerabilities to report:

- The read API being open by default ([ADR-024](docs/decisions/ADR-024-identity-authorization-and-audit.md) decision 2)
- Authorization being by action and not by target ([ADR-024](docs/decisions/ADR-024-identity-authorization-and-audit.md) decision 4, recorded as partially satisfying ARCHITECTURE §10.4)
- A compromised coordinator broker credential being able to forge a command, with no message-level authentication ([ADR-024](docs/decisions/ADR-024-identity-authorization-and-audit.md) decision 10)
- Hand-provisioned broker credentials being permanent in practice until node enrollment automation exists
- No TLS anywhere ([ADR-022](docs/decisions/ADR-022-operator-ui-serves-the-api-same-origin.md))
- A session with no absolute lifetime, expiring only after 90 consecutive days without use ([ADR-024](docs/decisions/ADR-024-identity-authorization-and-audit.md) decision 5, which exists because the operator runs this show outdoors, at night, from a phone)
- Commands, macro invocations, status, and telemetry crossing the show LAN in cleartext, per the accepted posture above. A **credential** in cleartext is not covered by this and is a defect worth reporting.
- Anything arising from FPP's own security posture: its unauthenticated-by-default web UI, its world-readable config files, its `/api/configfile/**` endpoint, its cleartext `command/run` publishes, or its narrow backup redaction list ([RES-015](docs/research/RES-015-fpp-plugin-distribution-model.md) §7.4). These are documented, designed around, and fixable only upstream in FPP. What *is* worth reporting is ShowMesh putting a secret somewhere FPP will expose it.

If you think one of those decisions is *wrong* rather than merely unfinished, that is a valuable conversation, so open an issue arguing the case. Superseding a recorded decision is how this project is designed to change.

## Reporting a vulnerability

For anything not on the list above, please report privately rather than opening a public issue: use GitHub's **[private vulnerability reporting](https://github.com/ShowMeshSystems/showmesh/security/advisories/new)** on this repository.

Please include the affected component and version or commit, reproduction steps, and what an attacker gains. Expect an acknowledgement within a week; this is a personal project and not staffed for a faster commitment.

## Supported versions

None. There is no released version and no backport policy. The `main` branch is the only supported code.
