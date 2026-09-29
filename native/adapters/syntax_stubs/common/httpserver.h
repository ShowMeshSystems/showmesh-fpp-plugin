#pragma once

#include <memory>
#include <string>

// Stub of the pieces of libhttpserver FPP 9's plugin.cpp actually
// references. See ../README.md.

namespace httpserver {

class http_request {
 public:
    std::string get_content() const;
};

class http_response {
 public:
    virtual ~http_response() = default;
};

class string_response : public http_response {
 public:
    string_response(const std::string& body, int statusCode, const std::string& contentType);
};

class http_resource {
 public:
    virtual ~http_resource() = default;
    void disallow_all();
    void set_allowing(const std::string& verb, bool allowed);
    virtual std::shared_ptr<http_response> render_GET(const http_request& request);
    virtual std::shared_ptr<http_response> render_POST(const http_request& request);
};

class webserver {
 public:
    void register_resource(const std::string& path, http_resource* resource, bool familyExtensions);
    void unregister_resource(const std::string& path);
};

}  // namespace httpserver
