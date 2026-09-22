#pragma once

#include <string>

#include "showmesh/brightness.h"

// The brightness read route, wire contract section 3, mirrored under
// docs/upstream/showmesh/. Host neutral: it renders a response from the
// engine's current state and knows nothing about which major's web server
// serves it.

namespace showmesh {

// The path each major registers with its own web server.
extern const char* const kBrightnessQueryPath;
// What a coordinator collector actually polls, through FPP's Apache
// plugin-apis proxy; see transition_gain.h's identical distinction.
extern const char* const kBrightnessQueryLanPath;

constexpr int kBrightnessQuerySchemaVersion = 1;

struct BrightnessQueryResponse {
    // Always 200: this route never refuses anything, it has nothing to
    // parse.
    int status = 200;
    std::string body;
};

// Renders contract section 3's GET response from engine's current state,
// the same values the brightness-state file persists. now is both the
// clock reading the engine's fades are evaluated at and the response's
// own updatedAtMillis, so a caller sees exactly when this snapshot was
// taken. Never writes.
BrightnessQueryResponse renderBrightnessQuery(const BrightnessEngine& engine, TimeMillis now);

}  // namespace showmesh
