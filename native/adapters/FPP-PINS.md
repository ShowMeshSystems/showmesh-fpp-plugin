# Pinned FPP versions

The adapters are compiled against these exact trees in CI. A version label
is not evidence of ABI compatibility, so the commit is recorded alongside
the tag and every later beta, release candidate, and final release is
added and rebuilt against its own headers rather than assumed compatible.

| Major | Tag | Commit | Plugin ABI version declared by the header |
|---|---|---|---|
| FPP 9 | `9.5.3` | `7979a4bb0bb9068fea71f3b447e273d5c0ea01e3` | not versioned |
| FPP 10 | `10.0` | `370e62ed7e8c8318da6ee5b01312b8b75082d952` | 6 |

Source: <https://github.com/FalconChristmas/fpp>

Re-pinned 2026-08-24: FPP 10 moved from the `10.0-beta5` pin to the `10.0`
final release, which is what the fleet runs. `Plugin.h` and `Plugins.h`
are byte-identical between the two commits, so this re-pin changed no ABI
assumption the adapter depends on; it was still re-verified against the
release's own headers rather than assumed compatible, per this file's own
rule above. `9.5.3` remains the latest 9.x release.

## What each major needs to compile

FPP 9 headers reach for `libhttpserver`, which is not packaged by Debian.
FPP installs it from source at `0.19.0`, and CI does the same so the
`HTTP_RESPONSE_CONST` decision `fpp-pch.h` makes from the installed
version matches what a real host would decide. FPP 10's command headers
name their HTTP types through a Drogon-free forward-declaration header, so
that adapter needs only jsoncpp.

## Pinned third-party dependencies

| Dependency | Tag | Commit |
|---|---|---|
| `libhttpserver` | `0.19.0` | `60be2d347fb3c757b2e77efc997120e47ed8fb7f` |

Source: <https://github.com/etr/libhttpserver>

The `HTTP_RESPONSE_CONST` decision the FPP 9 adapter derives from depends
on the installed `libhttpserver` version, so its tag is pinned to a
recorded commit the same way the FPP checkouts are.

## What the compile actually proves

That each adapter's translation unit agrees with that major's headers, and
that the shared object exports the symbols the loader looks up. It is not
evidence that the plugin loads into a running `fppd`, that the action
appears in the FPP UI, or that the callback boundary meets its latency
budget on real hardware. Those need a real host.

See [`docs/research/prebuilt-native-plugin-distribution.md`](../../docs/research/prebuilt-native-plugin-distribution.md) for whether the compiled adapter could ship prebuilt instead of building on the host.
