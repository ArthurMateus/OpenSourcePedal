// SHA-256 for preset resource hashes (tools/tonematch/Sha256.h), against the NIST FIPS 180-4 example vectors
// (https://csrc.nist.gov/projects/cryptographic-standards-and-guidelines/example-values, accessed 2026-10-26).

#include "Sha256.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {
std::string hashOf(const std::string& s)
{
    osp::tools::Sha256 h;
    h.update(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    return h.hex();
}
} // namespace

TEST_CASE("sha256: FIPS 180-4 example vectors", "[tonematch]")
{
    CHECK(hashOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hashOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 448 bits: the padding spills into a second block.
    CHECK(hashOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hashOf(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256: a file hashes like its bytes", "[tonematch]")
{
    const std::string path = std::string(OSP_ASSET_DIR) + "/cab/412-a.wav";
    // assets/LICENSES.md records this file's SHA-256.
    CHECK(osp::tools::Sha256::ofFile(path) == "821513696de3f73305708e6aaba752ad32ffdf0870a607d289273c3cc0e121fa");
    CHECK(osp::tools::Sha256::ofFile(path + ".missing").empty());
}
