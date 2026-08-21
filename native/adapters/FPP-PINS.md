# Pinned FPP versions

The adapters are compiled against these exact trees in CI. A version label
is not evidence of ABI compatibility, so the commit is recorded alongside
the tag and every later beta, release candidate, and final release is
added and rebuilt against its own headers rather than assumed compatible.

| Major | Tag | Commit | Plugin ABI version declared by the header |
|---|---|---|---|
| FPP 9 | `9.5.3` | `7979a4bb0bb9068fea71f3b447e273d5c0ea01e3` | not versioned |
| FPP 10 | `10.0-beta5` | `741cfc4344bd0d1b913941d507c3a123a8c82e5a` | 6 |

Source: <https://github.com/FalconChristmas/fpp>

Pinned 2026-08-21. `10.0-beta5` was the latest published FPP 10 beta at
that time; `9.5.3` was the latest 9.x release.

## What each major needs to compile

FPP 9 headers reach for `libhttpserver`, which is not packaged by Debian.
FPP installs it from source at `0.19.0`, and CI does the same so the
`HTTP_RESPONSE_CONST` decision `fpp-pch.h` makes from the installed
version matches what a real host would decide. FPP 10's command headers
name their HTTP types through a Drogon-free forward-declaration header, so
that adapter needs only jsoncpp.

## What the compile actually proves

That each adapter's translation unit agrees with that major's headers, and
that the shared object exports the symbols the loader looks up. It is not
evidence that the plugin loads into a running `fppd`, that the action
appears in the FPP UI, or that the callback boundary meets its latency
budget on real hardware. Those need a real host.
