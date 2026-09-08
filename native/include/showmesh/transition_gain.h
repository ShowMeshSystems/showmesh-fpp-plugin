#pragma once

#include <cstddef>
#include <string>

#include "showmesh/brightness.h"

// The transition-gain write, FPP-PLUGIN-COORDINATOR-CONTRACTS.md section
// 2.2, mirrored under docs/upstream/showmesh/. Host neutral: it parses a
// body and renders a response, and knows nothing about which major's web
// server delivered it. Nothing here invents a field or a spelling; a
// disagreement with the coordinator is a blocker raised against both
// repositories, never a unilateral change on one side.

namespace showmesh {

// The path each major registers with its own web server. Both adapters
// register this same constant, so the two majors cannot drift onto
// different paths.
//
// This is NOT the URL a coordinator posts to. Both majors bind their own
// HTTP server to 127.0.0.1 only (FPP 10's drogon on 32322; FPP 9's
// libhttpserver, whose APIServer::Init comments "so we only allow access
// via 127.0.0.1"), so every caller from the show LAN arrives through
// FPP's Apache, which proxies plugin routes under one prefix:
//
//   RewriteRule ^plugin-apis/(.*)$ http://localhost:32322/$1 [P]
//
// So the address is kTransitionGainLanPath below. Registering a path that
// already begins with /api produces a working but visibly wrong URL with
// /api in it twice.
extern const char* const kTransitionGainPath;

// What a coordinator actually posts to, on both majors. Kept beside the
// registered path because the two are easy to confuse and the difference
// is not visible from inside the plugin: a route can register
// successfully, appear in the host's own route table, and still answer
// 404 to every real caller.
extern const char* const kTransitionGainLanPath;

// Section 2.2's body is four small fields, so this bound is generous. It
// exists because the route is unauthenticated (section 2.2's accepted
// posture) and any host on the show LAN can post to it: an unbounded read
// on an FPP host during a show is a risk the body size never justifies.
constexpr std::size_t kTransitionGainBodyLimitBytes = 4096;

constexpr int kTransitionGainSchemaVersion = 1;

struct TransitionGainResponse {
    // The HTTP status to answer with. 200 for an applied write and for an
    // idempotent repeat, 400 for anything the contract refuses.
    int status = 400;
    // The response body, always JSON, always populated.
    std::string body;
};

// Applies one section 2.2 request against engine and renders the response
// section 2.2 specifies: the applied state, never an echo of the request
// and never a bare 200, so a caller has evidence rather than an
// acknowledgement.
//
// requestId is a caller-minted idempotency key. lastRequestId is the one
// this route last applied, read and updated in place; a repeat of the same
// id applies nothing and answers with the state as it stands, so a caller
// that retries a response it never saw cannot restart a fade.
//
// The caller holds whatever lock guards engine. This function does no
// locking of its own and hands nothing to another thread: on FPP 10 that
// is what keeps the handler inside the guarantee unregisterPluginApi()
// makes, which covers inbound HTTP only.
TransitionGainResponse applyTransitionGainRequest(const std::string& body, BrightnessEngine* engine,
                                                  std::string* lastRequestId, TimeMillis now);

}  // namespace showmesh
