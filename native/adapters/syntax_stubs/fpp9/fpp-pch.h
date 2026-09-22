#pragma once

// Stub of FPP 9's precompiled-header umbrella. See ../README.md. The real
// header sets _FILE_OFFSET_BITS and decides HTTP_RESPONSE_CONST before
// anything else; this repository's own code depends on neither, so this
// stub only has to make httpserver's names available before
// commands/Commands.h and Plugin.h are parsed.

#include "httpserver.h"
