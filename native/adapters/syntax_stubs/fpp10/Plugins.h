#pragma once

#include <functional>
#include <string>
#include <vector>

#include "fpphttp.h"

// Stub of FPP 10's plugin-API registration surface. See ../README.md.

namespace FPPPlugins {

using HttpApiHandler = std::function<void(const HttpRequestPtr&, HttpCallback&&)>;

void registerPluginApi(const std::string& path, HttpApiHandler handler, std::vector<drogon::HttpMethod> methods);
void unregisterPluginApi(const std::string& path);

}  // namespace FPPPlugins
