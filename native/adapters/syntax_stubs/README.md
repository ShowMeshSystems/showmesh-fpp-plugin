# Adapter syntax-check stubs

These headers exist only to let `make check` (via `native/Makefile`'s
`adapter-syntax-check` target) parse `fpp9/plugin.cpp`, `fpp10/plugin.cpp`,
and the shared delivery headers with `-fsyntax-only`, without a real FPP
source tree. They declare the shapes those files actually reference (`grep`ed
from the adapters themselves), not the real FPP API: a stub that compiles
here proves the adapter's own code is self-consistent (types line up, no
missing include, no typo'd member), never that it matches the real FPP
headers or that the plugin runs.

The one authoritative proof that both adapters build against real, pinned
FPP source is `bench/fpp-plugin-load` (`make test-plugin-load-fpp
MAJOR=fpp9|fpp10`), which compiles inside a container built from the exact
commit `native/adapters/FPP-PINS.md` pins. Re-run that bench after any
change to a stub-covered signature; this syntax check is a fast, offline
first line of defense between those runs, not a replacement for one.

jsoncpp is the one dependency here that is NOT stubbed: `Json::Value` is
pulled in for real (`adapters/shared/fpp-json-compat.h`, the identical file
the real adapters use), because both adapter builds already require real
jsoncpp headers and the API is too large to usefully fake.
