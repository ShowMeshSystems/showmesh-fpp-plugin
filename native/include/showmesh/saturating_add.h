#pragma once

#include <cstdint>
#include <limits>

namespace showmesh {

// Adds b to a, clamping to the maximum representable value instead of
// wrapping. unacknowledgedCoalesced_ uses this to report "at least this
// many observations were dropped" even across a pathologically long run
// of an unreachable sink; wrapping back through zero would understate the
// gap instead.
inline std::uint32_t saturatingAdd(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t sum = a + b;
    return sum < a ? std::numeric_limits<std::uint32_t>::max() : sum;
}

}  // namespace showmesh
