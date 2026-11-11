module;

#include <cstdint>

export module vortex.core:random;

export namespace vortex {

    /// @brief Integer hash (lowbias32 by Chris Wellons): good avalanche, 5 instructions.
    constexpr uint32_t Hash32(uint32_t x) {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    constexpr uint32_t HashCombine(uint32_t a, uint32_t b) { return Hash32(a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2))); }

    /// @brief Hash of an integer lattice point. Deterministic across platforms.
    constexpr uint32_t Hash3(int32_t x, int32_t y, int32_t z, uint32_t seed = 0) {
        uint32_t h = Hash32(static_cast<uint32_t>(x) + seed * 0x9e3779b9u);
        h = Hash32(h ^ (static_cast<uint32_t>(y) * 0x85ebca6bu));
        h = Hash32(h ^ (static_cast<uint32_t>(z) * 0xc2b2ae35u));
        return h;
    }

    /// @brief Maps a hash to [0, 1).
    constexpr float HashToFloat(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

    /**
     * @brief PCG32 random number generator: small, fast, statistically solid.
     * @details Deterministic for a given seed on every platform, which matters for
     * procedural generation and (later) network-synchronised simulation.
     */
    class Rng {
    public:
        constexpr explicit Rng(uint64_t seed = 0x853c49e6748fea9bull, uint64_t stream = 0xda3e39cb94b95bdbull)
            : m_State(0), m_Inc((stream << 1u) | 1u) {
            NextU32();
            m_State += seed;
            NextU32();
        }

        constexpr uint32_t NextU32() {
            const uint64_t old = m_State;
            m_State = old * 6364136223846793005ull + m_Inc;
            const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
            const uint32_t rot = static_cast<uint32_t>(old >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
        }

        /// @brief Uniform float in [0, 1).
        constexpr float NextFloat() { return static_cast<float>(NextU32() >> 8) * (1.0f / 16777216.0f); }
        /// @brief Uniform float in [lo, hi).
        constexpr float Range(float lo, float hi) { return lo + (hi - lo) * NextFloat(); }
        /// @brief Uniform integer in [0, bound).
        constexpr uint32_t NextBounded(uint32_t bound) {
            return bound == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(NextU32()) * bound) >> 32);
        }

    private:
        uint64_t m_State;
        uint64_t m_Inc;
    };
}
