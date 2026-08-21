#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace showmesh {

// SHA-256 (FIPS 180-4). Written here rather than linked because the native
// component ships as an architecture-independent source bundle compiled on
// the FPP host, and a hash dependency would have to be resolvable there
// too.
class Sha256 {
 public:
    Sha256() { reset(); }

    void reset();
    void update(const void* data, std::size_t length);
    void update(const std::string& s) { update(s.data(), s.size()); }
    // Finalizes into 32 raw bytes. The object must be reset before reuse.
    void finish(std::uint8_t out[32]);

 private:
    void compress(const std::uint8_t block[64]);

    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::size_t bufferLength_;
    std::uint64_t totalBits_;
};

std::string sha256Hex(const std::string& data);

}  // namespace showmesh
