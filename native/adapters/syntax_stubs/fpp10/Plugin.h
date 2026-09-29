#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "fpp-json-compat.h"

// Stub of FPP 10's plugin base class. See ../README.md.

constexpr int FPPD_MAX_CHANNELS = 262144;

#define FPP_PLUGIN_SUPPORTS_UNLOAD()

class FPPPlugin {
 public:
    explicit FPPPlugin(const std::string& name);
    virtual ~FPPPlugin() = default;

    virtual std::function<bool()> shutdown();
    virtual void registerApis();
    virtual void unregisterApis();
    virtual void playlistCallback(const Json::Value& playlist, const std::string& action,
                                  const std::string& section, int item);
    virtual void modifyChannelData(int channelCount, std::uint8_t* channelData);
    virtual void multiSyncData(const std::uint8_t* data, int length);
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
