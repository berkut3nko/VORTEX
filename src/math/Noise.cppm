module;

#include <cmath>
#include <cstdint>

export module vortex.math:noise;

import vortex.core;

export namespace vortex::math {

    /**
     * @brief Seedable simplex noise (after Stefan Gustavson's public domain reference).
     * @details Gradients are chosen with an integer hash instead of a permutation table, so the
     * noise is a pure function of (position, seed): any thread can evaluate any region of the
     * world independently and get the same result, which is what chunk streaming requires.
     * Output is roughly in [-1, 1].
     */
    class SimplexNoise {
    public:
        explicit SimplexNoise(uint32_t seed = 0) : m_Seed(seed) {}

        float Sample(float x, float y) const {
            constexpr float F2 = 0.3660254037844386f;   // (sqrt(3) - 1) / 2
            constexpr float G2 = 0.21132486540518713f;  // (3 - sqrt(3)) / 6

            const float s = (x + y) * F2;
            const int i = FastFloor(x + s);
            const int j = FastFloor(y + s);
            const float t = static_cast<float>(i + j) * G2;
            const float x0 = x - (static_cast<float>(i) - t);
            const float y0 = y - (static_cast<float>(j) - t);

            const int i1 = x0 > y0 ? 1 : 0;
            const int j1 = x0 > y0 ? 0 : 1;

            const float x1 = x0 - static_cast<float>(i1) + G2;
            const float y1 = y0 - static_cast<float>(j1) + G2;
            const float x2 = x0 - 1.0f + 2.0f * G2;
            const float y2 = y0 - 1.0f + 2.0f * G2;

            float n = 0.0f;
            n += Corner2(x0, y0, i, j);
            n += Corner2(x1, y1, i + i1, j + j1);
            n += Corner2(x2, y2, i + 1, j + 1);
            return 70.0f * n;
        }

        float Sample(float x, float y, float z) const {
            constexpr float F3 = 1.0f / 3.0f;
            constexpr float G3 = 1.0f / 6.0f;

            const float s = (x + y + z) * F3;
            const int i = FastFloor(x + s);
            const int j = FastFloor(y + s);
            const int k = FastFloor(z + s);
            const float t = static_cast<float>(i + j + k) * G3;
            const float x0 = x - (static_cast<float>(i) - t);
            const float y0 = y - (static_cast<float>(j) - t);
            const float z0 = z - (static_cast<float>(k) - t);

            int i1, j1, k1, i2, j2, k2;
            if (x0 >= y0) {
                if (y0 >= z0)      { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
                else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
                else               { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
            } else {
                if (y0 < z0)       { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
                else if (x0 < z0)  { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
                else               { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
            }

            const float x1 = x0 - static_cast<float>(i1) + G3;
            const float y1 = y0 - static_cast<float>(j1) + G3;
            const float z1 = z0 - static_cast<float>(k1) + G3;
            const float x2 = x0 - static_cast<float>(i2) + 2.0f * G3;
            const float y2 = y0 - static_cast<float>(j2) + 2.0f * G3;
            const float z2 = z0 - static_cast<float>(k2) + 2.0f * G3;
            const float x3 = x0 - 1.0f + 3.0f * G3;
            const float y3 = y0 - 1.0f + 3.0f * G3;
            const float z3 = z0 - 1.0f + 3.0f * G3;

            float n = 0.0f;
            n += Corner3(x0, y0, z0, i, j, k);
            n += Corner3(x1, y1, z1, i + i1, j + j1, k + k1);
            n += Corner3(x2, y2, z2, i + i2, j + j2, k + k2);
            n += Corner3(x3, y3, z3, i + 1, j + 1, k + 1);
            return 32.0f * n;
        }

        /// @brief Fractal Brownian motion: `octaves` layers, each at double frequency and `gain` amplitude.
        float Fbm(float x, float y, int octaves, float lacunarity = 2.0f, float gain = 0.5f) const {
            float sum = 0.0f, amp = 1.0f, norm = 0.0f;
            for (int o = 0; o < octaves; ++o) {
                sum += amp * Sample(x, y);
                norm += amp;
                x = x * lacunarity + 17.13f;   // decorrelate octaves
                y = y * lacunarity - 9.71f;
                amp *= gain;
            }
            return norm > 0.0f ? sum / norm : 0.0f;
        }

        float Fbm(float x, float y, float z, int octaves, float lacunarity = 2.0f, float gain = 0.5f) const {
            float sum = 0.0f, amp = 1.0f, norm = 0.0f;
            for (int o = 0; o < octaves; ++o) {
                sum += amp * Sample(x, y, z);
                norm += amp;
                x = x * lacunarity + 17.13f;
                y = y * lacunarity - 9.71f;
                z = z * lacunarity + 4.37f;
                amp *= gain;
            }
            return norm > 0.0f ? sum / norm : 0.0f;
        }

        /// @brief Ridged noise in [0, 1]: sharp crests, good for mountain ranges.
        float Ridged(float x, float y, int octaves, float lacunarity = 2.0f, float gain = 0.5f) const {
            float sum = 0.0f, amp = 1.0f, norm = 0.0f;
            for (int o = 0; o < octaves; ++o) {
                const float r = 1.0f - std::abs(Sample(x, y));
                sum += amp * r * r;
                norm += amp;
                x = x * lacunarity + 31.7f;
                y = y * lacunarity + 11.3f;
                amp *= gain;
            }
            return norm > 0.0f ? sum / norm : 0.0f;
        }

    private:
        static int FastFloor(float v) {
            const int i = static_cast<int>(v);
            return v < static_cast<float>(i) ? i - 1 : i;
        }

        float Corner2(float x, float y, int i, int j) const {
            float t = 0.5f - x * x - y * y;
            if (t <= 0.0f) return 0.0f;
            t *= t;
            // 8 gradient directions on the unit square / axes.
            const uint32_t h = vortex::Hash3(i, j, 0, m_Seed) & 7u;
            static constexpr float gx[8] = {1, -1, 1, -1, 1, -1, 0, 0};
            static constexpr float gy[8] = {1, 1, -1, -1, 0, 0, 1, -1};
            return t * t * (gx[h] * x + gy[h] * y);
        }

        float Corner3(float x, float y, float z, int i, int j, int k) const {
            float t = 0.6f - x * x - y * y - z * z;
            if (t <= 0.0f) return 0.0f;
            t *= t;
            // 12 gradient directions: the midpoints of the edges of a cube.
            const uint32_t h = vortex::Hash3(i, j, k, m_Seed) % 12u;
            static constexpr float gx[12] = {1, -1, 1, -1, 1, -1, 1, -1, 0, 0, 0, 0};
            static constexpr float gy[12] = {1, 1, -1, -1, 0, 0, 0, 0, 1, -1, 1, -1};
            static constexpr float gz[12] = {0, 0, 0, 0, 1, 1, -1, -1, 1, 1, -1, -1};
            return t * t * (gx[h] * x + gy[h] * y + gz[h] * z);
        }

        uint32_t m_Seed;
    };
}
