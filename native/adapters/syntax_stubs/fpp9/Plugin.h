#pragma once

#include <cstdint>
#include <string>

#include "fpp-json-compat.h"
#include "httpserver.h"

// Stub of FPP 9's plugin base class. See ../README.md.

constexpr int FPPD_MAX_CHANNELS = 262144;

class FPPPlugin {
 public:
    explicit FPPPlugin(const std::string& name);
    virtual ~FPPPlugin() = default;

    virtual void playlistCallback(const Json::Value& playlist, const std::string& action,
                                  const std::string& section, int item);
    virtual void modifyChannelData(int channelCount, std::uint8_t* channelData);
    virtual void multiSyncData(const std::uint8_t* data, int length);
    virtual void registerApis(httpserver::webserver* server);
    virtual void unregisterApis(httpserver::webserver* server);
};

class PluginManagerClass {
 public:
    void multiSyncData(const std::string& pluginName, std::uint8_t* data, int length);
};

class PluginManager {
 public:
    static PluginManagerClass INSTANCE;
};

extern "C" FPPPlugin* createPlugin();
