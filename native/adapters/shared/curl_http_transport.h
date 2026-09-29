#pragma once

#include <curl/curl.h>

#include <string>

#include "showmesh/http_transport.h"

// The concrete outbound client. It lives beside the adapters rather than
// in the host-neutral core for one reason: the coordinator URL may be
// https, and a TLS-capable client means a third-party library, which the
// core deliberately links none of. libcurl is not a new dependency on an
// FPP host; fppd links it already.
//
// Everything here runs on the resident worker thread.

namespace showmesh {
namespace adapter {

class CurlHttpTransport : public HttpTransport {
 public:
    CurlHttpTransport() {
        // fppd initializes libcurl for its own use, and a second init is
        // documented as safe and reference counted. The matching
        // curl_global_cleanup() is deliberately absent: it is not
        // reference counted in the same way, and calling it here would
        // tear libcurl down underneath fppd.
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    HttpResponse post(const HttpRequest& request) override { return perform(request, /*isPost=*/true); }

    HttpResponse get(const HttpRequest& request) override { return perform(request, /*isPost=*/false); }

 private:
    // Bounds an in-flight response body by the request's own cap, sized
    // for the response this specific call expects rather than one
    // constant shared by every call site.
    struct BoundedBody {
        std::string body;
        std::size_t maxBytes = 8192;
    };

    HttpResponse perform(const HttpRequest& request, bool isPost) {
        HttpResponse response;

        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            response.error = "libcurl could not create a request handle";
            return response;
        }

        // Built here and freed below rather than retained, so the
        // credential does not sit in a long-lived header list.
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Content-Type: " + request.contentType).c_str());
        // Omitted entirely, not sent with an empty value, when there is no
        // token: an unauthenticated route (the pairing claim POST) must
        // never carry a malformed "Authorization: Bearer " header.
        if (!request.bearerToken.empty()) {
            std::string authorization = "Authorization: Bearer " + request.bearerToken;
            headers = curl_slist_append(headers, authorization.c_str());
            authorization.assign(authorization.size(), '\0');
        }

        BoundedBody bounded;
        bounded.maxBytes = request.maxResponseBytes > 0 ? static_cast<std::size_t>(request.maxResponseBytes) : 0;
        curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
        if (isPost) {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        } else {
            curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
        }
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeoutMillis));
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.timeoutMillis));
        // The worker owns its own timeouts and retry; libcurl following a
        // redirect would replay the credential at whatever host answered.
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
        // fppd is multi threaded and libcurl's DNS timeout uses signals
        // unless this is set, which is not safe off the main thread.
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &appendBody);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &bounded);

        const CURLcode code = curl_easy_perform(handle);
        if (code == CURLE_OK) {
            long status = 0;
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
            response.transportOk = true;
            response.statusCode = static_cast<int>(status);
            response.body = std::move(bounded.body);
        } else {
            // curl's own text, which describes the transport and never
            // reflects a request header back.
            response.error = curl_easy_strerror(code);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);
        return response;
    }

    static std::size_t appendBody(char* data, std::size_t size, std::size_t count, void* userdata) {
        const std::size_t bytes = size * count;
        BoundedBody* out = static_cast<BoundedBody*>(userdata);
        if (out->body.size() < out->maxBytes) {
            const std::size_t room = out->maxBytes - out->body.size();
            out->body.append(data, bytes > room ? room : bytes);
        }
        return bytes;
    }
};

}  // namespace adapter
}  // namespace showmesh
