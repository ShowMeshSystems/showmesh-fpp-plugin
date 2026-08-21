#pragma once

#include <string>

#include "showmesh/brightness.h"

namespace showmesh {

// The full-state payload is written to disk and carried between nodes in
// the same encoding, so what a late joiner adopts and what a restart reads
// cannot drift apart. It is canonical JSON, which makes two encodings of
// one state byte-identical and therefore comparable.
std::string encodeBrightnessState(const BrightnessState& state);

struct BrightnessStateDecode {
    bool ok = false;
    BrightnessState state;
    std::string error;
};

// A payload missing a required field is rejected rather than defaulted:
// defaulting a brightness field silently invents a value for something a
// show depends on.
BrightnessStateDecode decodeBrightnessState(const std::string& text);

}  // namespace showmesh
