#pragma once

#include <string>

// The outbound HTTP seam. The host-neutral core defines only this
// interface: a real client links a TLS-capable library, which this core
// must not, and a test needs to answer with a status code rather than
// open a socket. The concrete client lives beside the adapters, where
// linking a third-party library is already the norm.

namespace showmesh {

struct HttpRequest {
    std::string url;
    std::string body;
    std::string contentType = "application/json";
    // Sent as `Authorization: Bearer <token>`. It is never logged, never
    // copied into HttpResponse::error, and never written to a file: an
    // implementation of this interface that does any of those breaks the
    // credential rule this whole seam exists to keep.
    std::string bearerToken;
    int timeoutMillis = 10000;
};

struct HttpResponse {
    // False when no HTTP response was obtained at all: DNS, connect,
    // TLS, or timeout failure. statusCode carries no meaning then, which
    // is why this is a separate flag rather than a sentinel code.
    bool transportOk = false;
    int statusCode = 0;
    std::string body;
    // Operator-facing failure text. Must never contain the credential.
    std::string error;
};

class HttpTransport {
 public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse post(const HttpRequest& request) = 0;
};

}  // namespace showmesh
