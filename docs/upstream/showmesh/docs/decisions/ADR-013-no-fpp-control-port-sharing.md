# ADR-013: ShowMesh Must Not Share the FPP Control Port with a Running fppd

Status: Accepted  
Date: 2026-08-10

## Context

`pkg/multisync` receives FPP MultiSync traffic on UDP 32320. The port is fixed by the protocol, so any ShowMesh component that listens for MultiSync must bind the same port fppd binds.

Sharing that port looked like a convenience: a node agent could then run directly on an FPP box. Bench verification on Linux during the Step 1 review (2026-08-10) established two facts that change the picture.

First, coexistence is not simply available. FPP sets `SO_REUSEPORT` only, never `SO_REUSEADDR`. Linux's UDP bind-conflict check requires either both sockets to set `SO_REUSEADDR`, or both to set `SO_REUSEPORT` with matching UIDs. Observed:

| Socket | Options | Result |
|---|---|---|
| fppd-like, root | `SO_REUSEPORT` only | bind succeeds |
| ShowMesh, uid 1000 | `SO_REUSEADDR` + `SO_REUSEPORT` | bind fails, address already in use |
| ShowMesh, root | `SO_REUSEADDR` + `SO_REUSEPORT` | bind succeeds |
| ShowMesh, root | `SO_REUSEADDR` only | bind fails, address already in use |

So a shared bind requires ShowMesh to run as the same user as fppd, in practice root.

Second, and decisively: when the bind does succeed, `SO_REUSEPORT` **load balances** unicast datagrams across the socket group by 4-tuple hash rather than delivering a copy to every member. Observed: 20 unicast datagrams sent to port 32320 with two `SO_REUSEPORT` sockets open were delivered 20 to one socket and 0 to the other. Multicast and broadcast are fanned out to all group members, so the hazard is specific to MultiSync unicast mode, which is exactly the mode deployments use where multicast is unavailable or blocked.

A ShowMesh listener sharing the port with fppd can therefore silently consume the unicast sync stream that FPP's own remotes depend on, desyncing a running show, with no error anywhere and no obvious causal link back to ShowMesh.

That is not merely a bug. [ADR-001](ADR-001-fpp-is-authoritative.md) makes FPP authoritative, and the standing constraint that ShowMesh is never in the real-time timing path exists precisely to prevent this class of interference. Port sharing is the one place in the codebase where ShowMesh could sit inside FPP's timing path, and it would do so invisibly.

## Decision

ShowMesh components must not share UDP 32320 with a running fppd.

Port sharing is exposed as listener configuration and defaults to off. With it off, ShowMesh sets **neither** `SO_REUSEADDR` nor `SO_REUSEPORT`, so a bind conflict against a running fppd fails loudly at startup, which is the correct and recoverable outcome. Enabling it is permitted only for diagnostics on a host where no show is running, and the option carries that warning at its definition.

Both options are gated, not just `SO_REUSEPORT`. The first implementation set `SO_REUSEADDR` unconditionally, on the assumption that for UDP it cannot by itself permit two processes to bind the same port. That assumption is false on Linux, and the first CI run on a Linux runner is what exposed it. Verified in a Linux container: two sockets setting only `SO_REUSEADDR` both bind the same UDP port, and 20 unicast datagrams sent to that port were delivered 20 to one socket and 0 to the other. macOS does not behave this way, which is why local testing missed it.

Gating both options also removes a dependency this decision should not have. With `SO_REUSEADDR` on by default, ShowMesh was protected from binding alongside fppd only because fppd happens to set `SO_REUSEPORT` and never `SO_REUSEADDR`, so the mismatched pair fails. That is an accident of FPP's current implementation, not a property of ours. Were a future FPP release to add `SO_REUSEADDR`, the accident would evaporate and ShowMesh would silently begin intercepting. Setting no sharing options at all makes the guarantee ShowMesh's own: the bind fails whenever anything else holds the port, whatever options that other process chose.

A ShowMesh component that needs MultiSync timing must run on a host that is not running fppd. Where co-location is genuinely required, the supported boundary is the FPP plugin callback interface that [RES-002](../research/RES-002-fpp-multisync-compatibility.md) documents (`addMultiSyncPlugin`, which delivers parsed sync callbacks in-process), not a second socket on the same port. FPP's REST and MQTT interfaces remain available for supervision-grade status, but they are not frame-accurate and are not a timing path.

## Consequences

- A bind conflict is a loud startup failure rather than a silent show hazard. Operators get an actionable error instead of an intermittently desynchronized show.
- Two ShowMesh processes cannot listen on UDP 32320 on the same host by default either. That is intended: the Linux result above shows one of them would take all the unicast traffic regardless, so permitting the bind would only hide the problem. A host needing both an agent and a diagnostic capture must run the capture elsewhere.
- Node agents requiring MultiSync timing must be separate hosts from FPP players. This is a real constraint on reference and community topologies, and it removes the appealing option of adding a listener to an existing FPP box. The cost is accepted deliberately.
- The bench probe must be run from a separate machine on the same segment during any live show. [The capture procedure](../bench/RES-002-capture-procedure.md) states this.
- The privileged configuration is not pursued. Running ShowMesh as root to satisfy Linux UID matching would still not address unicast interception, and it would add a privilege requirement the project does not otherwise need.
- Where co-location cannot be avoided, the plugin boundary becomes the required path, which means C++ code running inside fppd. That is a heavier integration than wire parsing and should be scoped as its own work if a deployment demands it.
- [RES-009](../research/RES-009-failure-mode-testing.md) failure testing must include the co-located case, to confirm the loud failure actually occurs and that no configuration silently re-enables sharing.

## Alternatives considered

Enabling port sharing and accepting the risk was rejected: silently desynchronizing a live show is the most damaging failure mode available to this project, and it would be attributed to FPP rather than to ShowMesh.

Running ShowMesh as root to satisfy Linux UID matching was rejected: it makes the bind succeed without addressing unicast interception, so it converts a loud failure into the silent one.

Binding a different port was rejected because MultiSync's port is fixed at 32320 by the protocol; there is nothing to choose.

Using the FPP plugin callback boundary for co-located hosts was not rejected. It is retained as the supported answer when co-location is required, and it remains the fallback direction RES-002 already identifies.

## Related research

[FPP MultiSync](../research/RES-002-fpp-multisync-compatibility.md) · [Failure testing](../research/RES-009-failure-mode-testing.md)

## 2026-08-23 note: the unicast hazard this decision guards against is now the FPP 10 default

Not a supersession — the decision above and its consequences stand
unchanged. Recorded because [RES-002](../research/RES-002-fpp-multisync-compatibility.md)'s
SM-209 amendment found something this decision's original reasoning did not
anticipate and that strengthens rather than weakens it.

This decision's Context section describes unicast MultiSync interception as
"exactly the mode deployments use where multicast is unavailable or
blocked" — an edge case, not the common path. On a fresh FPP 10 install,
that framing no longer holds: `MultiSyncUnicast` defaults to on and
`MultiSyncMulticast` carries no default at all (read directly at the `10.0`
tag, `370e62ed7e8c8318da6ee5b01312b8b75082d952`). Unicast is now the
*default* MultiSync transport on a fresh FPP 10 player, not a fallback for
unusual networks.

This does not change the port-sharing analysis above — the `SO_REUSEPORT`
load-balancing hazard this decision documents is about two sockets sharing
UDP 32320 on the same host, which is orthogonal to which transport carries
the traffic. It sharpens the stakes of the decision already made: had
ShowMesh ever shared the port with a running `fppd` on an FPP 10 host, the
traffic a co-located ShowMesh listener would silently steal is now the
transport an FPP 10 fleet uses by default, not an edge case. The decision
to keep ShowMesh off the FPP host entirely, made before this was known,
turns out to have been the right call for a reason that did not yet exist
when it was written.

This note does not itself close any of RES-002's open items; it is recorded
here only because ADR-013's own Context materially undersold how common the
unicast case is, and a reader relying on this ADR for risk framing should
not be left with the pre-FPP-10 picture.
