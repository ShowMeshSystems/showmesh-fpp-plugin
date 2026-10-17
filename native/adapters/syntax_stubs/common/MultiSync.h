#pragma once

#include <cstdint>
#include <string>

// Stub. See ../README.md.

class MultiSync {
 public:
    static MultiSync INSTANCE;
    bool isMultiSyncEnabled();
    void SendPluginData(const std::string& name, const uint8_t* data, int len);
};
