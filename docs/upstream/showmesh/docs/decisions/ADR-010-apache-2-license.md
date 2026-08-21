# ADR-010: The Repository License Is Apache-2.0

Status: Accepted  
Date: 2026-08-10

## Context

RES-006 established the only sanctioned open-source NDI pattern: MIT-licensed SDK headers vendored in-repo plus `dlopen` of a user-installed proprietary runtime, never bundling or linking it. FFmpeg's 2019 removal of NDI followed a GPL dispute over exactly this proximity. The project also wants controller vendors and commercial show operators to embed agents without license friction.

## Decision

All ShowMesh code is licensed Apache-2.0. Vendored NDI headers retain their MIT license with attribution. The NDI runtime is never redistributed in this repository or its release artifacts; the agent locates it at runtime (`NDI_RUNTIME_DIR_V6`, system paths) and degrades gracefully with an install pointer when absent. Required NDI trademark attributions and ndi.video links appear wherever NDI is referenced in product surfaces.

## Consequences

- The dlopen boundary is legally uncontroversial under a permissive license; the FFmpeg failure mode is avoided.
- Patent grant protects contributors and adopters beyond plain MIT.
- Copyleft protection is forgone: closed forks are possible and accepted.
- CI/release tooling must enforce that no NDI binaries enter build artifacts.
- Contributions are accepted under Apache-2.0 inbound=outbound; a CLA is not required.

## Alternatives considered

MIT was rejected in favor of Apache-2.0's explicit patent grant. GPL-3.0 was rejected because the NDI dlopen pattern would sit permanently on contested ground and vendor embedding would be deterred.

## Related research

[Linux NDI](../research/RES-006-linux-ndi-support.md)
