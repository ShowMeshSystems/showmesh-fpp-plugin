#include "showmesh/sha256.h"

#include <string>

#include "check.h"

using showmesh::Sha256;
using showmesh::sha256Hex;

TEST(Sha256MatchesTheKnownVectors) {
    CHECK_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK_EQ(sha256Hex(std::string(1000000, 'a')),
             "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256IsInsensitiveToUpdateChunking) {
    const std::string message(200, 'x');
    Sha256 chunked;
    for (std::size_t i = 0; i < message.size(); i += 7) {
        chunked.update(message.data() + i, std::min<std::size_t>(7, message.size() - i));
    }
    std::uint8_t digest[32];
    chunked.finish(digest);

    std::string hex;
    static const char* kHex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        hex.push_back(kHex[digest[i] >> 4]);
        hex.push_back(kHex[digest[i] & 0x0F]);
    }
    CHECK_EQ(hex, sha256Hex(message));
}

// The message length is encoded in the final block, so a message whose
// length lands exactly on a block boundary and one that fills the padding
// window are the two cases an off-by-one in finish() would break.
TEST(Sha256HandlesBlockBoundaryLengths) {
    CHECK_EQ(sha256Hex(std::string(55, 'a')),
             "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK_EQ(sha256Hex(std::string(56, 'a')),
             "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK_EQ(sha256Hex(std::string(64, 'a')),
             "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK_EQ(sha256Hex(std::string(119, 'a')),
             "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
}
