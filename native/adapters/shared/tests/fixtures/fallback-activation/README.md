# Fallback activation fixtures

The files this plugin's tests need, copied unchanged from
`test/fixtures/fallback-activation/` in
`ShowMeshSystems/showmesh`, branch `claude/node-fallback-activation-ingress`,
at commit `fde36268e3a99caa31392038da8a20322a99aaca`. They are the shared data
for section 5 of that repository's
`docs/build/FPP-PLUGIN-COORDINATOR-CONTRACTS.md`: the node agent (Go) and this
plugin (C++) implement the same signing and canonicalization rules
independently, and these files are how that agreement is checked.

Every key pair here is a published RFC 8032 section 7.1 test vector. No file
holds a key anyone uses.

- `keys.json`: the coordinator public key the programs are signed under, and
  the seed and public key of two executors.
- `program.json`: a signed program that carries `executorPublicKey` and
  `targets[].address`.
- `cases.json`: `validRequest.canonical` and `validRequest.signature`, plus one
  request body per node answer.

`fallback_executor_test.cpp` builds the valid request from `program.json`,
compares its canonical bytes and its whole body against `cases.json`, and signs
those bytes with `executorSeedHex`. Ed25519 signatures are deterministic, so
any other signature is a defect in this plugin's signer.

To refresh, copy the files again from the source directory and update the
commit above. Nothing here is generated in this repository.
