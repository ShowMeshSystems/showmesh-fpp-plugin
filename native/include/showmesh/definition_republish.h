#pragma once

#include <cstddef>
#include <string>

// The coordinator-triggered definition republish,
// FPP-PLUGIN-COORDINATOR-CONTRACTS.md section 3.9, mirrored under
// docs/upstream/showmesh/. Host neutral: it parses a body and renders a
// response, and knows nothing about which major's web server delivered
// it. Nothing here invents a field or a spelling; a disagreement with the
// coordinator is a blocker raised against both repositories, never a
// unilateral change on one side.

namespace showmesh {

class DefinitionPublisher;

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
// So the address is kDefinitionRepublishLanPath below. Registering a path
// that already begins with /api produces a working but visibly wrong URL
// with /api in it twice.
extern const char* const kDefinitionRepublishPath;

// What a coordinator actually posts to, on both majors. Kept beside the
// registered path because the two are easy to confuse and the difference
// is not visible from inside the plugin: a route can register
// successfully, appear in the host's own route table, and still answer
// 404 to every real caller.
extern const char* const kDefinitionRepublishLanPath;

// Section 3.9's body is two small fields. The bound exists because the
// route is unauthenticated (section 2.2's accepted posture) and any host
// on the show LAN can post to it: an unbounded read on an FPP host during
// a show is a risk no body size justifies.
constexpr std::size_t kDefinitionRepublishBodyLimitBytes = 4096;

constexpr int kDefinitionRepublishSchemaVersion = 1;

// What the publisher's held and terminally refused sets contained at one
// instant. Every count is read in the single critical section that also
// performs the clear, so the response reports one consistent moment
// rather than three separately sampled ones.
struct DefinitionHoldings {
    std::size_t cleared = 0;
    std::size_t held = 0;
    std::size_t refusedTerminally = 0;
};

// The owed-sweep record section 3.9 item 1 requires. The route records
// that a sweep is due and returns; the worker thread performs it, sweeps
// regardless of how recently it last swept, and clears the record when
// the sweep it caused has completed. This is deliberately not the
// re-scan cadence: those variables are worker-thread only and have never
// needed a lock, and an inbound handler reaching in to reset the
// last-sweep time would be a data race on them.
class SweepRecord {
 public:
    virtual ~SweepRecord() = default;
    // Records that a sweep is owed. Safe to call from the HTTP thread.
    virtual void requestSweep() = 0;
    // Whether a sweep is owed and has not yet completed. Readable from
    // the HTTP thread without touching worker-thread-only state.
    virtual bool sweepPending() const = 0;
};

struct DefinitionRepublishResponse {
    // 200 for an applied republish and for an idempotent repeat, 400 for
    // anything the contract refuses.
    int status = 400;
    // The response body, always JSON, always populated.
    std::string body;
};

// Applies one section 3.9 request and renders the response it specifies:
// what this request dropped and what the plugin holds as the answer is
// written, never a bare 200 and never an echo of the request.
//
// An applied request clears the publisher's held-definition set and
// records that a sweep is owed. It must never clear the terminally
// refused set: those refusals cannot change until the plugin restarts, so
// re-sending them would spend the retry policy's backoff ahead of the
// definitions and observations that would succeed.
//
// requestId is a caller-minted idempotency key. lastRequestId is the one
// this route last applied, read and updated in place; a repeat of the
// same id applies nothing and answers with the state as it stands, so
// polling it is how a caller learns the sweep finished.
//
// Synchronous, and it hands nothing to another thread: on FPP 10 that is
// what keeps the handler inside the guarantee unregisterPluginApi()
// makes, which covers inbound HTTP only.
DefinitionRepublishResponse applyDefinitionRepublishRequest(const std::string& body, DefinitionPublisher* publisher,
                                                            SweepRecord* sweep, std::string* lastRequestId);

}  // namespace showmesh
