#pragma once

#include <string>

// Stub. See ../README.md.

class Player {
 public:
    int IsPlaying();
    std::string GetPlaylistName();
    static Player INSTANCE;
};
