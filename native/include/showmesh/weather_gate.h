#pragma once

#include <cstddef>
#include <string>

#include "showmesh/brightness.h"

// The coordinator-facing weather-gate write, registered beside the
// transition-gain route (transition_gain.h) with the same registration,
// authentication, body-size, and refusal handling on both FPP majors. Host
// neutral: it parses a body and renders a response, and knows nothing about
// which major's web server delivered it.

namespace showmesh {

// See transition_gain.h's kTransitionGainPath/kTransitionGainLanPath for
// why these are two different strings: both majors bind loopback only, and
// FPP's Apache proxies plugin routes under the LAN path's prefix.
extern const char* const kWeatherGatePath;
extern const char* const kWeatherGateLanPath;

// The body is two fields, so this bound is generous. It exists for the same
// reason transition-gain's does: the route is unauthenticated, and any host
// on the show LAN can post to it.
constexpr std::size_t kWeatherGateBodyLimitBytes = 4096;

constexpr int kWeatherGateSchemaVersion = 1;

struct WeatherGateResponse {
    // 200 for an applied write or a read, 400 for anything refused.
    int status = 400;
    // The response body, always JSON, always populated.
    std::string body;
};

// Applies one weather-gate write: the body must be exactly
// {"closed": true|false, "revision": <integer 0..2^53-1>}, with no other key,
// and anything else is refused rather than clamped or guessed. A valid write
// always applies, whatever its revision (see BrightnessEngine::setWeatherGate).
// Renders the same full state document the transition-gain write returns,
// extended with "weatherGateClosed", "weatherGateRevision" and
// "effectiveOutputPercent".
//
// The caller holds whatever lock guards engine; this function does no
// locking of its own, for the same reason applyTransitionGainRequest does
// not.
WeatherGateResponse applyWeatherGateRequest(const std::string& body, BrightnessEngine* engine, TimeMillis now);

// Renders the same document as a successful write, without changing
// anything: the GET route registered beside the write, for a reader that
// wants the current state and never writes to it.
WeatherGateResponse renderWeatherGateState(BrightnessEngine* engine, TimeMillis now);

}  // namespace showmesh
