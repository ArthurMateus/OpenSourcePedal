#pragma once

// SHA-256 (NIST FIPS 180-4, "Secure Hash Standard", 2015, §6.2), own implementation for the preset resource hashes
// (preset.v1 requires a sha256 for every non-factory resource, ADR-0003 §5). Offline tools only.
// Checked against the FIPS 180-4 example vectors "abc" and the empty message in tests/tools/test_sha256.cpp.

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace osp::tools {

class Sha256 {
public:
    void update(const unsigned char* data, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i) {
            block_[used_++] = data[i];
            if (used_ == 64) {
                compress();
                used_ = 0;
            }
        }
        bits_ += static_cast<std::uint64_t>(n) * 8;
    }

    // Lower-case hex digest. The object is spent afterwards.
    std::string hex()
    {
        const std::uint64_t bits = bits_;
        const unsigned char one = 0x80, zero = 0x00;
        update(&one, 1);
        while (used_ != 56)
            update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i)
            len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(len, 8);
        std::string out;
        char b[3];
        for (std::uint32_t v : h_)
            for (int s = 24; s >= 0; s -= 8) {
                std::snprintf(b, sizeof b, "%02x", static_cast<unsigned>((v >> s) & 0xff));
                out += b;
            }
        return out;
    }

    // Digest of a whole file, or "" when it cannot be read.
    static std::string ofFile(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return {};
        Sha256 s;
        std::vector<char> buf(1 << 16);
        while (f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            s.update(reinterpret_cast<const unsigned char*>(buf.data()), static_cast<std::size_t>(f.gcount()));
        }
        return f.bad() ? std::string{} : s.hex();
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress()
    {
        static constexpr std::array<std::uint32_t, 64> k{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 64> w{};
        for (int t = 0; t < 16; ++t)
            w[t] = static_cast<std::uint32_t>(block_[4 * t]) << 24 |
                   static_cast<std::uint32_t>(block_[4 * t + 1]) << 16 |
                   static_cast<std::uint32_t>(block_[4 * t + 2]) << 8 | static_cast<std::uint32_t>(block_[4 * t + 3]);
        for (int t = 16; t < 64; ++t) {
            const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int t = 0; t < 64; ++t) {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[t] + w[t];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::array<std::uint32_t, 8> h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t bits_ = 0;
};

} // namespace osp::tools
