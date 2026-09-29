#pragma once

#include <functional>
#include <memory>
#include <string>

// Stub of the drogon-facing names FPP 10's Plugins.h exposes. See
// ../README.md.

namespace drogon {

enum HttpMethod { Get, Post, Put, Delete, Head, Options, Patch };

class HttpRequest {
 public:
    HttpMethod method() const { return Get; }
};
class HttpResponse {};
using HttpResponsePtr = std::shared_ptr<HttpResponse>;

class HttpAppFramework {};
HttpAppFramework& app();

}  // namespace drogon

using HttpRequestPtr = std::shared_ptr<drogon::HttpRequest>;
using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;

drogon::HttpResponsePtr makeStringResponse(const std::string& body, int statusCode, const std::string& contentType);
std::string getRequestContent(const HttpRequestPtr& request);
