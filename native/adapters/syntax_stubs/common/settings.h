#pragma once

#include <functional>
#include <string>

// Stub. See ../README.md.

std::string getSetting(const std::string& name);
void registerSettingsListener(const std::string& pluginName, const std::string& settingName,
                              std::function<void(const std::string&)> callback);
void unregisterSettingsListener(const std::string& pluginName, const std::string& settingName);

// Real signature takes a suffix and returns the absolute playlist
// directory path with it appended.
#define FPP_DIR_PLAYLIST(suffix) (std::string("/home/fpp/media/playlists") + (suffix))
