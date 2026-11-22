module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

module vortex.net;

namespace vortex::net {

    namespace {
        constexpr std::array<uint32_t, 64> kRound = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        constexpr uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    }

    Sha256::Sha256()
        : m_State{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

    void Sha256::Block(const uint8_t* p) {
        std::array<uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16 | static_cast<uint32_t>(p[4 * i + 2]) << 8 |
                   p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = m_State[0], b = m_State[1], c = m_State[2], d = m_State[3];
        uint32_t e = m_State[4], f = m_State[5], g = m_State[6], h = m_State[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
            const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        m_State[0] += a;
        m_State[1] += b;
        m_State[2] += c;
        m_State[3] += d;
        m_State[4] += e;
        m_State[5] += f;
        m_State[6] += g;
        m_State[7] += h;
    }

    void Sha256::Update(std::span<const uint8_t> bytes) {
        m_Length += bytes.size();
        size_t i = 0;
        if (m_Buffered > 0) {
            const size_t take = std::min(bytes.size(), 64 - m_Buffered);
            std::memcpy(m_Buffer.data() + m_Buffered, bytes.data(), take);
            m_Buffered += take;
            i = take;
            if (m_Buffered < 64) return;
            Block(m_Buffer.data());
            m_Buffered = 0;
        }
        for (; i + 64 <= bytes.size(); i += 64) Block(bytes.data() + i);
        if (i < bytes.size()) {
            m_Buffered = bytes.size() - i;
            std::memcpy(m_Buffer.data(), bytes.data() + i, m_Buffered);
        }
    }

    void Sha256::Update(std::string_view text) { Update(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size())); }

    Sha256Digest Sha256::Finish() {
        const uint64_t bits = m_Length * 8;
        const uint8_t one = 0x80;
        Update(std::span<const uint8_t>(&one, 1));
        const std::array<uint8_t, 64> zeros{};
        const size_t pad = m_Buffered <= 56 ? 56 - m_Buffered : 120 - m_Buffered;
        Update(std::span<const uint8_t>(zeros.data(), pad));
        std::array<uint8_t, 8> length{};
        for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        Update(length);
        Sha256Digest out{};
        for (int i = 0; i < 8; ++i)
            for (int k = 0; k < 4; ++k) out[4 * i + k] = static_cast<uint8_t>(m_State[i] >> (24 - 8 * k));
        return out;
    }

    Sha256Digest Sha256::Of(std::span<const uint8_t> bytes) {
        Sha256 h;
        h.Update(bytes);
        return h.Finish();
    }

    Sha256Digest Sha256::Of(std::string_view text) {
        Sha256 h;
        h.Update(text);
        return h.Finish();
    }

    Sha256Digest HmacSha256(std::span<const uint8_t> key, std::span<const std::span<const uint8_t>> parts) {
        std::array<uint8_t, 64> block{};
        if (key.size() > block.size()) {
            const Sha256Digest k = Sha256::Of(key);
            std::memcpy(block.data(), k.data(), k.size());
        } else if (!key.empty()) {
            std::memcpy(block.data(), key.data(), key.size());
        }
        std::array<uint8_t, 64> pad{};
        for (size_t i = 0; i < pad.size(); ++i) pad[i] = block[i] ^ 0x36;
        Sha256 inner;
        inner.Update(pad);
        for (const std::span<const uint8_t> part : parts) inner.Update(part);
        const Sha256Digest innerDigest = inner.Finish();
        for (size_t i = 0; i < pad.size(); ++i) pad[i] = block[i] ^ 0x5c;
        Sha256 outer;
        outer.Update(pad);
        outer.Update(innerDigest);
        return outer.Finish();
    }

    Sha256Digest HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message) {
        const std::span<const uint8_t> parts[] = {message};
        return HmacSha256(key, parts);
    }
}
