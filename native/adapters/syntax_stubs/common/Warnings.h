#pragma once

#include <string>
#include <vector>

// Stub. See ../README.md.

class WarningHolder {
 public:
    static void AddWarningTimeout(int timeoutSeconds, int id, const std::string& message,
                                  std::vector<std::string> notificationOnly, const std::string& plugin);
    static void RemoveWarning(int id, const std::string& message, const std::string& plugin);
};
