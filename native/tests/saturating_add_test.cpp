#include "showmesh/saturating_add.h"

#include <cstdint>
#include <limits>

#include "check.h"

using showmesh::saturatingAdd;

TEST(SaturatingAddSumsNormallyWhenWellBelowTheLimit) {
    CHECK_EQ(saturatingAdd(3, 4), static_cast<std::uint32_t>(7));
    CHECK_EQ(saturatingAdd(0, 0), static_cast<std::uint32_t>(0));
}

// If saturatingAdd were reduced to `return sum;`, this overflows
// UINT32_MAX + 1 and wraps to 0, which this check would catch.
TEST(SaturatingAddClampsRatherThanWrappingAtTheMaximum) {
    const std::uint32_t max = std::numeric_limits<std::uint32_t>::max();
    CHECK_EQ(saturatingAdd(max, 1), max);
    CHECK_EQ(saturatingAdd(max - 5, 10), max);
    CHECK_EQ(saturatingAdd(max, max), max);
}

// The boundary case: a sum landing exactly on the maximum is not itself
// evidence of overflow and must not be reported as anything other than
// the true, exact sum.
TEST(SaturatingAddReturnsTheExactSumWhenItLandsOnTheMaximum) {
    const std::uint32_t max = std::numeric_limits<std::uint32_t>::max();
    CHECK_EQ(saturatingAdd(max - 10, 10), max);
}
