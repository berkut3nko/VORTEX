module;

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

module vortex.voxel;

// The world map bake (engine/docs/design/land.md): erosion (L3) and water at fixed levels (L4) on the coarse grid.
// Only the server runs it (land.md "Decision" 3), so it may use floats and libm: one build gives one result.

namespace vortex::voxel {

    namespace {
        using Clock = std::chrono::steady_clock;

        double Since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

        constexpr uint64_t Mix(uint64_t x) {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ull;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebull;
            x ^= x >> 31;
            return x;
        }

        float Unit(uint64_t a) { return float(Mix(a) >> 40) * (1.0f / 16777216.0f); }

        float Lattice(uint64_t salt, int x, int z) {
            return Unit(salt ^ (uint64_t(uint32_t(x)) << 32 | uint32_t(z)));
        }

        int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

        /// Smooth value noise in [0, 1) with a lattice every `step` cells.
        float ValueNoise(uint64_t salt, int x, int z, int step) {
            const int gx = FloorDiv(x, step), gz = FloorDiv(z, step);
            float fx = float(x - gx * step) / float(step), fz = float(z - gz * step) / float(step);
            fx = fx * fx * (3.0f - 2.0f * fx);
            fz = fz * fz * (3.0f - 2.0f * fz);
            const float a = Lattice(salt, gx, gz), b = Lattice(salt, gx + 1, gz);
            const float c = Lattice(salt, gx, gz + 1), d = Lattice(salt, gx + 1, gz + 1);
            return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
        }

        constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
        constexpr int kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};

        bool IsSea(uint8_t b) { return b == uint8_t(LandBiome::Sea); }
        bool IsDrawnWater(uint8_t b) { return b == uint8_t(LandBiome::Sea) || b == uint8_t(LandBiome::Lake); }

        struct Routing {
            std::vector<uint32_t> order;     ///< every cell, downstream first
            std::vector<uint32_t> receiver;  ///< the cell each drains into; itself for an outlet
            std::vector<float> filled;       ///< depressions filled (strictly descending paths with epsilon > 0)
        };

        /// Priority-flood (Barnes, Lehman, Mulla 2014) from the sea and the map border.
        void Route(const LandMap& m, const std::vector<float>& h, float epsilon, Routing& r) {
            const int w = m.width, hz = m.height;
            const uint32_t n = uint32_t(w) * uint32_t(hz);
            r.order.clear();
            r.order.reserve(n);
            r.receiver.assign(n, UINT32_MAX);
            r.filled = h;
            using Item = std::pair<float, uint32_t>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
            std::vector<uint8_t> seen(n, 0);
            for (int z = 0; z < hz; ++z) {
                for (int x = 0; x < w; ++x) {
                    const uint32_t i = m.Index(x, z);
                    if (IsSea(m.biomes[i]) || x == 0 || z == 0 || x == w - 1 || z == hz - 1) {
                        seen[i] = 1;
                        r.receiver[i] = i;
                        open.push({h[i], i});
                    }
                }
            }
            while (!open.empty()) {
                const uint32_t c = open.top().second;
                open.pop();
                r.order.push_back(c);
                const int cx = int(c % uint32_t(w)), cz = int(c / uint32_t(w));
                for (int k = 0; k < 8; ++k) {
                    const int nx = cx + kDx[k], nz = cz + kDz[k];
                    if (nx < 0 || nz < 0 || nx >= w || nz >= hz) continue;
                    const uint32_t ni = m.Index(nx, nz);
                    if (seen[ni]) continue;
                    seen[ni] = 1;
                    r.receiver[ni] = c;
                    const float f = std::max(h[ni], r.filled[c] + epsilon);
                    r.filled[ni] = f;
                    open.push({f, ni});
                }
            }
        }

        void Accumulate(const Routing& r, float cellArea, std::vector<float>& area) {
            area.assign(r.receiver.size(), cellArea);
            for (auto it = r.order.rbegin(); it != r.order.rend(); ++it) {
                const uint32_t c = *it, rc = r.receiver[c];
                if (rc != c) area[rc] += area[c];
            }
        }

        float StepLength(const LandMap& m, uint32_t a, uint32_t b) {
            const uint32_t w = uint32_t(m.width);
            const bool diagonal = (a % w != b % w) && (a / w != b / w);
            return diagonal ? m.cellSize * 1.41421356f : m.cellSize;
        }

        void Resample(const LandSource& s, const LandSettings& st, LandMap& m) {
            const float scale = st.cellSize / s.metresPerPixel;   // source pixels per cell
            m.cellSize = st.cellSize;
            m.width = std::max(1, int(float(s.width) * s.metresPerPixel / st.cellSize));
            m.height = std::max(1, int(float(s.height) * s.metresPerPixel / st.cellSize));
            const uint32_t n = uint32_t(m.width) * uint32_t(m.height);
            m.rawHeights.resize(n);
            m.biomes.resize(n);
            m.regions.resize(n);
            auto at = [&](int x, int z) {
                x = std::clamp(x, 0, s.width - 1);
                z = std::clamp(z, 0, s.height - 1);
                return size_t(z) * size_t(s.width) + size_t(x);
            };
            for (int z = 0; z < m.height; ++z) {
                for (int x = 0; x < m.width; ++x) {
                    const float u = (float(x) + 0.5f) * scale - 0.5f, v = (float(z) + 0.5f) * scale - 0.5f;
                    const int x0 = int(std::floor(u)), z0 = int(std::floor(v));
                    const float fx = u - float(x0), fz = v - float(z0);
                    const float a = s.heights[at(x0, z0)], b = s.heights[at(x0 + 1, z0)];
                    const float c = s.heights[at(x0, z0 + 1)], d = s.heights[at(x0 + 1, z0 + 1)];
                    float hgt = (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
                    const size_t near = at(int(std::lround(u)), int(std::lround(v)));
                    const uint32_t i = m.Index(x, z);
                    m.biomes[i] = s.biomes[near];
                    m.regions[i] = s.regions.empty() ? 0 : s.regions[near];
                    // Keep the coastline exactly where the map draws it.
                    if (IsSea(m.biomes[i])) hgt = std::min(hgt, -0.5f);
                    else if (m.biomes[i] != uint8_t(LandBiome::Lake)) hgt = std::max(hgt, 0.3f);
                    m.rawHeights[i] = hgt;
                }
            }
        }

        void Hardness(const LandSettings& st, LandMap& m) {
            m.hardness.resize(m.rawHeights.size());
            for (int z = 0; z < m.height; ++z) {
                for (int x = 0; x < m.width; ++x) {
                    const uint32_t i = m.Index(x, z);
                    uint8_t base = 3;
                    switch (LandBiome(m.biomes[i])) {
                    case LandBiome::Sea: base = 0; break;
                    case LandBiome::Desert: base = 2; break;
                    case LandBiome::Forest:
                    case LandBiome::DarkForest: base = 4; break;
                    case LandBiome::Mountain: base = 8; break;
                    case LandBiome::RockIsle: base = 10; break;
                    default: break;
                    }
                    // Strata: hard bands at some heights, tilted by slow noise, in the mountains and the high ground.
                    const float hgt = m.rawHeights[i];
                    if (base >= 8 || hgt > 30.0f) {
                        const float tilt = ValueNoise(st.seed ^ 0x5354u, x, z, 64) * 3.0f * st.strataBand;
                        const int band = int(std::floor((hgt + tilt) / st.strataBand));
                        if (Mix(st.seed ^ uint64_t(uint32_t(band))) % 3 == 0) base = uint8_t(std::min(15, base + 6));
                    }
                    m.hardness[i] = base;
                }
            }
        }

        float Erodibility(uint8_t hardness) { return 1.0f / (1.0f + 0.5f * float(hardness)); }

        /**
         * Hybrid breaching (after Lindsay 2016): every closed basin of the eroded height gets an outlet cut from its
         * lowest cell down its drainage path, at most `maxBreach` deep, so it drains like real land instead of being filled
         * flat. A drawn lake's basin is cut down to the drawn level (its lowest drawn bank less 0.5 m), however deep the
         * outlet gorge must be: the owner draws the lake, the land around it follows. What stays below the cut is a
         * lake or a puddle for Lakes().
         */
        void Breach(const LandSettings& st, LandMap& m) {
            Routing r;
            Route(m, m.heights, 0.0f, r);
            const uint32_t n = uint32_t(m.heights.size());
            const uint32_t w = uint32_t(m.width);
            std::vector<uint32_t> label(n, UINT32_MAX);
            std::vector<uint32_t> stack;
            auto depressed = [&](uint32_t i) { return !IsSea(m.biomes[i]) && r.filled[i] - m.heights[i] > 0.01f; };
            struct Basin {
                uint32_t label, bottom;
                float target;
            };
            std::vector<Basin> basins;
            for (uint32_t seed = 0; seed < n; ++seed) {
                if (label[seed] != UINT32_MAX || !depressed(seed)) continue;
                stack.assign(1, seed);
                label[seed] = seed;
                // A drawn lake's level: the lowest dry bank around the drawn lake, less 0.5 m.
                float bottom = m.heights[seed], drawnBank = 1.0e9f;
                uint32_t bottomCell = seed;
                bool drawn = false;
                while (!stack.empty()) {
                    const uint32_t c = stack.back();
                    stack.pop_back();
                    if (m.heights[c] < bottom) {
                        bottom = m.heights[c];
                        bottomCell = c;
                    }
                    const int cx = int(c % w), cz = int(c / w);
                    const bool lake = m.biomes[c] == uint8_t(LandBiome::Lake);
                    drawn = drawn || lake;
                    for (int k = 0; k < 8; ++k) {
                        const int nx = cx + kDx[k], nz = cz + kDz[k];
                        if (nx < 0 || nz < 0 || nx >= m.width || nz >= m.height) continue;
                        const uint32_t ni = m.Index(nx, nz);
                        if (lake && !IsDrawnWater(m.biomes[ni])) drawnBank = std::min(drawnBank, m.heights[ni]);
                        if (k >= 4 || label[ni] != UINT32_MAX || !depressed(ni)) continue;
                        if (std::abs(r.filled[ni] - r.filled[seed]) > 1.0e-4f) continue;
                        label[ni] = seed;
                        stack.push_back(ni);
                    }
                }
                const float spill = r.filled[seed];
                const float target = drawn ? std::min(spill, std::max(bottom, drawnBank - 0.5f))
                                           : std::max(bottom, spill - st.maxBreach);
                if (target < spill - 0.01f) basins.push_back({seed, bottomCell, target});
            }
            // Cut each outlet along the drainage path from the basin's lowest cell: inside the basin down to the target
            // level, outside descending from it by a millimetre a cell until the land is lower anyway.
            for (const Basin& b : basins) {
                uint32_t c = b.bottom;
                float level = b.target;
                while (!IsSea(m.biomes[c])) {
                    const bool inside = label[c] == b.label;
                    if (m.heights[c] > level) {
                        m.heights[c] = level;
                        ++m.breachedCells;
                    } else if (!inside) {
                        break;
                    }
                    if (r.receiver[c] == c) break;
                    c = r.receiver[c];
                    if (!inside) level -= 1.0e-3f;
                }
            }
        }

        /// The lowest height erosion may leave in each cell: the raw height less a cap; land stays above the sea.
        std::vector<float> ErosionFloor(const LandSettings& st, const LandMap& m) {
            std::vector<float> floorH(m.rawHeights.size());
            for (int z = 0; z < m.height; ++z) {
                for (int x = 0; x < m.width; ++x) {
                    const uint32_t i = m.Index(x, z);
                    float cut = st.maxStreamCut;
                    for (int k = 0; k < 8; ++k) {
                        const int nx = x + kDx[k], nz = z + kDz[k];
                        if (nx < 0 || nz < 0 || nx >= m.width || nz >= m.height) continue;
                        if (m.biomes[m.Index(nx, nz)] == uint8_t(LandBiome::Lake)) cut = std::min(cut, st.lakeRimCut);
                    }
                    float f = m.rawHeights[i] - cut;
                    if (!IsDrawnWater(m.biomes[i])) f = std::max(f, 0.3f);
                    floorH[i] = f;
                }
            }
            return floorH;
        }

        void StreamPower(const LandSettings& st, LandMap& m, const std::vector<float>& floorH) {
            Routing r;
            std::vector<float> area;
            std::vector<float>& h = m.heights;
            for (int it = 0; it < st.streamIterations; ++it) {
                Route(m, h, 1.0e-3f, r);
                Accumulate(r, m.cellSize * m.cellSize, area);
                for (uint32_t c : r.order) {
                    const uint32_t rc = r.receiver[c];
                    if (rc == c || IsDrawnWater(m.biomes[c]) || h[c] <= h[rc]) continue;
                    const float f = st.streamK * Erodibility(m.hardness[c]) * std::pow(area[c], st.streamM) /
                                    StepLength(m, c, rc);
                    h[c] = std::max((h[c] + f * h[rc]) / (1.0f + f), floorH[c]);
                }
            }
        }

        void Thermal(const LandSettings& st, LandMap& m, const std::vector<float>& floorH) {
            std::vector<float>& h = m.heights;
            std::vector<float> delta(h.size());
            for (int it = 0; it < st.thermalIterations; ++it) {
                std::fill(delta.begin(), delta.end(), 0.0f);
                for (int z = 0; z < m.height; ++z) {
                    for (int x = 0; x < m.width; ++x) {
                        const uint32_t c = m.Index(x, z);
                        if (IsDrawnWater(m.biomes[c])) continue;
                        const float talus = (m.hardness[c] >= 9 ? st.hardTalus : st.softTalus) * m.cellSize;
                        for (int k = 0; k < 4; ++k) {
                            const int nx = x + kDx[k], nz = z + kDz[k];
                            if (nx < 0 || nz < 0 || nx >= m.width || nz >= m.height) continue;
                            const uint32_t n = m.Index(nx, nz);
                            if (IsDrawnWater(m.biomes[n])) continue;
                            const float d = h[c] - h[n];
                            if (d <= talus) continue;
                            // At most a quarter of what the cell holds above its floor per neighbour: the slump
                            // conserves material.
                            const float move = std::min(0.2f * (d - talus), std::max(0.0f, 0.25f * (h[c] - floorH[c])));
                            delta[c] -= move;
                            delta[n] += move;
                        }
                    }
                }
                for (size_t i = 0; i < h.size(); ++i) h[i] = std::max(h[i] + delta[i], floorH[i]);
            }
        }

        /// Droplet erosion (after Beyer 2015), in cell units with heights in metres.
        void Droplets(const LandSettings& st, LandMap& m, const std::vector<float>& floorH) {
            std::vector<float>& h = m.heights;
            const int w = m.width, hz = m.height;
            std::vector<float> dropFloor(h.size());
            for (size_t i = 0; i < h.size(); ++i) dropFloor[i] = std::max(floorH[i], h[i] - st.maxDropletCut);
            uint64_t land = 0;
            for (uint8_t b : m.biomes) land += IsDrawnWater(b) ? 0 : 1;
            const uint64_t count = uint64_t(double(land) * double(st.dropletsPerLandCell));
            constexpr float kInertia = 0.1f, kCapacity = 4.0f, kMinCapacity = 0.01f, kDeposit = 0.3f, kErode = 0.3f;
            constexpr float kEvaporate = 0.02f, kGravity = 4.0f;

            auto sample = [&](float px, float pz, float& gx, float& gz) {
                const int x0 = int(px), z0 = int(pz);
                const float fx = px - float(x0), fz = pz - float(z0);
                const float a = h[m.Index(x0, z0)], b = h[m.Index(x0 + 1, z0)];
                const float c = h[m.Index(x0, z0 + 1)], d = h[m.Index(x0 + 1, z0 + 1)];
                gx = (b - a) * (1.0f - fz) + (d - c) * fz;
                gz = (c - a) * (1.0f - fx) + (d - b) * fx;
                return a * (1 - fx) * (1 - fz) + b * fx * (1 - fz) + c * (1 - fx) * fz + d * fx * fz;
            };
            // Adds (or removes) material around a point; returns what the ground really took or gave, so the droplet's
            // sediment and the ground keep the same total.
            auto add = [&](float px, float pz, float amount) {
                const int x0 = int(px), z0 = int(pz);
                const float fx = px - float(x0), fz = pz - float(z0);
                const uint32_t idx[4] = {m.Index(x0, z0), m.Index(x0 + 1, z0), m.Index(x0, z0 + 1),
                                         m.Index(x0 + 1, z0 + 1)};
                const float wt[4] = {(1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz};
                float applied = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    if (IsDrawnWater(m.biomes[idx[k]])) continue;
                    float v = amount * wt[k];
                    if (v < 0.0f) v *= Erodibility(m.hardness[idx[k]]);
                    const float before = h[idx[k]];
                    h[idx[k]] = std::max(before + v, dropFloor[idx[k]]);
                    applied += h[idx[k]] - before;
                }
                return applied;
            };

            for (uint64_t d = 0; d < count; ++d) {
                float px = Unit(st.seed * 31 + d * 2) * float(w - 1);
                float pz = Unit(st.seed * 31 + d * 2 + 1) * float(hz - 1);
                if (IsDrawnWater(m.biomes[m.Index(int(px), int(pz))])) continue;
                float dx = 0, dz = 0, speed = 1, water = 1, sediment = 0;
                for (int step = 0; step < st.dropletSteps; ++step) {
                    float gx, gz;
                    const float h0 = sample(px, pz, gx, gz);
                    dx = dx * kInertia - gx * (1 - kInertia);
                    dz = dz * kInertia - gz * (1 - kInertia);
                    const float len = std::sqrt(dx * dx + dz * dz);
                    if (len < 1.0e-6f) break;
                    dx /= len;
                    dz /= len;
                    const float ox = px, oz = pz;
                    px += dx;
                    pz += dz;
                    if (px < 0 || pz < 0 || px >= float(w - 1) || pz >= float(hz - 1)) break;
                    if (IsDrawnWater(m.biomes[m.Index(int(px), int(pz))])) break;
                    float ngx, ngz;
                    const float dh = sample(px, pz, ngx, ngz) - h0;
                    const float capacity = std::max(-dh * speed * water * kCapacity, kMinCapacity);
                    if (sediment > capacity || dh > 0) {
                        const float put = dh > 0 ? std::min(dh, sediment) : (sediment - capacity) * kDeposit;
                        sediment -= add(ox, oz, put);
                    } else {
                        const float take = std::min((capacity - sediment) * kErode, -dh);
                        sediment -= add(ox, oz, -take);
                    }
                    speed = std::sqrt(std::max(0.0f, speed * speed - dh * kGravity));
                    water *= 1 - kEvaporate;
                }
            }
        }

        /// L4a: fill depressions; deep and wide ones (or drawn ones) are lakes at their spill height, the rest
        /// puddles that are filled.
        void Lakes(const LandSettings& st, LandMap& m) {
            Routing r;
            Route(m, m.heights, 0.0f, r);
            const uint32_t n = uint32_t(m.heights.size());
            m.water.assign(n, 0.0f);
            m.waterKind.assign(n, WaterKind::None);
            m.lakeOf.assign(n, 0);
            for (uint32_t i = 0; i < n; ++i) {
                if (IsSea(m.biomes[i])) m.waterKind[i] = WaterKind::Sea;
            }
            std::vector<uint32_t> label(n, UINT32_MAX);
            auto depressed = [&](uint32_t i) { return !IsSea(m.biomes[i]) && r.filled[i] - m.heights[i] > 0.01f; };
            std::vector<uint32_t> stack, cells;
            const float cellArea = m.cellSize * m.cellSize;
            for (uint32_t seed = 0; seed < n; ++seed) {
                if (label[seed] != UINT32_MAX || !depressed(seed)) continue;
                cells.clear();
                stack.assign(1, seed);
                label[seed] = seed;
                LandLake lake;
                lake.level = r.filled[seed];
                while (!stack.empty()) {
                    const uint32_t c = stack.back();
                    stack.pop_back();
                    cells.push_back(c);
                    lake.maxDepth = std::max(lake.maxDepth, r.filled[c] - m.heights[c]);
                    lake.drawn = lake.drawn || m.biomes[c] == uint8_t(LandBiome::Lake);
                    const int cx = int(c % uint32_t(m.width)), cz = int(c / uint32_t(m.width));
                    for (int k = 0; k < 4; ++k) {
                        const int nx = cx + kDx[k], nz = cz + kDz[k];
                        if (nx < 0 || nz < 0 || nx >= m.width || nz >= m.height) continue;
                        const uint32_t ni = m.Index(nx, nz);
                        if (label[ni] != UINT32_MAX || !depressed(ni)) continue;
                        if (std::abs(r.filled[ni] - lake.level) > 1.0e-4f) continue;
                        label[ni] = seed;
                        stack.push_back(ni);
                    }
                }
                lake.cells = uint32_t(cells.size());
                const bool keep = lake.drawn ||
                                  (float(lake.cells) * cellArea >= st.minLakeArea && lake.maxDepth >= st.minLakeDepth);
                if (!keep || m.lakes.size() >= 65535) {
                    for (uint32_t c : cells) m.heights[c] = r.filled[c];
                    ++m.puddlesFilled;
                    continue;
                }
                m.lakes.push_back(lake);
                for (uint32_t c : cells) {
                    m.waterKind[c] = WaterKind::Lake;
                    m.water[c] = lake.level;
                    m.lakeOf[c] = uint16_t(m.lakes.size());
                }
            }
        }

        /// L4b: rivers where the drainage area passes the threshold; levels in steps, beds cut below them.
        void Rivers(const LandSettings& st, LandMap& m) {
            Routing r;
            Route(m, m.heights, 1.0e-3f, r);
            Accumulate(r, m.cellSize * m.cellSize, m.flow);
            const uint32_t n = uint32_t(m.heights.size());
            std::vector<uint8_t> isRiver(n, 0), hasUp(n, 0);
            for (uint32_t i = 0; i < n; ++i) {
                isRiver[i] = m.waterKind[i] == WaterKind::None && m.flow[i] >= st.riverArea ? 1 : 0;
            }
            for (uint32_t i = 0; i < n; ++i) {
                if (isRiver[i] && r.receiver[i] != i && isRiver[r.receiver[i]]) hasUp[r.receiver[i]] = 1;
            }
            // Levels downstream first: never above the receiver's water, a step of `reachStep` below the bank.
            std::vector<float> level(n, 0.0f);
            auto waterAt = [&](uint32_t c) {
                if (isRiver[c]) return level[c];
                if (m.waterKind[c] != WaterKind::None) return m.water[c];
                return m.heights[c];
            };
            for (uint32_t c : r.order) {
                if (!isRiver[c]) continue;
                const float bank = std::floor((m.heights[c] - st.riverFreeboard) / st.reachStep) * st.reachStep;
                level[c] = r.receiver[c] == c ? bank : std::max(waterAt(r.receiver[c]), bank);
            }
            auto depthOf = [&](uint32_t c) {
                return std::clamp(0.5f + 0.5f * std::log2(m.flow[c] / st.riverArea), 0.5f, 2.0f);
            };
            std::vector<uint32_t> riverOf(n, UINT32_MAX);
            for (uint32_t head = 0; head < n; ++head) {
                if (!isRiver[head] || hasUp[head]) continue;
                LandRiver river;
                uint32_t c = head;
                while (true) {
                    riverOf[c] = uint32_t(m.rivers.size());
                    river.cells.push_back(c);
                    const uint32_t rc = r.receiver[c];
                    if (rc == c || m.waterKind[rc] == WaterKind::Sea) {
                        river.end = RiverEnd::Sea;
                        river.endCell = rc;
                        break;
                    }
                    if (m.waterKind[rc] == WaterKind::Lake) {
                        river.end = RiverEnd::Lake;
                        river.endCell = rc;
                        break;
                    }
                    if (riverOf[rc] != UINT32_MAX) {
                        river.end = RiverEnd::River;
                        river.endCell = rc;
                        break;
                    }
                    c = rc;
                }
                for (uint32_t k = 0; k < river.cells.size(); ++k) {
                    const float lv = level[river.cells[k]];
                    if (river.reaches.empty() || river.reaches.back().level != lv) {
                        river.reaches.push_back({k, 0, lv});
                    }
                    ++river.reaches.back().count;
                }
                m.rivers.push_back(std::move(river));
            }
            // Cut the channels: the path cells, then a wider bed for large rivers, then banks above the water.
            for (const LandRiver& river : m.rivers) {
                for (uint32_t c : river.cells) {
                    const float bed = level[c] - depthOf(c);
                    const int half = m.flow[c] >= 4.0f * st.riverArea ? 1 : 0;
                    const int cx = int(c % uint32_t(m.width)), cz = int(c / uint32_t(m.width));
                    for (int dz = -half - 1; dz <= half + 1; ++dz) {
                        for (int dx = -half - 1; dx <= half + 1; ++dx) {
                            const int nx = cx + dx, nz = cz + dz;
                            if (nx < 0 || nz < 0 || nx >= m.width || nz >= m.height) continue;
                            const uint32_t ni = m.Index(nx, nz);
                            if (m.waterKind[ni] == WaterKind::Sea || m.waterKind[ni] == WaterKind::Lake) continue;
                            const bool bank = std::max(std::abs(dx), std::abs(dz)) > half;
                            if (!bank) {
                                m.heights[ni] = std::min(m.heights[ni], bed);
                                if (m.waterKind[ni] == WaterKind::None) {
                                    m.waterKind[ni] = WaterKind::River;
                                    m.water[ni] = level[c];
                                }
                            } else if (!isRiver[ni] && m.waterKind[ni] == WaterKind::None &&
                                       m.heights[ni] < level[c] + 0.2f) {
                                m.heights[ni] = level[c] + 0.2f;
                                ++m.banksRaised;
                            }
                        }
                    }
                }
            }
            for (const LandRiver& river : m.rivers) {
                for (uint32_t c : river.cells) {
                    m.waterKind[c] = WaterKind::River;
                    m.water[c] = level[c];
                    m.heights[c] = std::min(m.heights[c], level[c] - depthOf(c));
                }
            }
        }
    }

    LandMap BakeLand(const LandSource& source, const LandSettings& settings) {
        const auto start = Clock::now();
        LandMap m;
        auto t = Clock::now();
        Resample(source, settings, m);
        Hardness(settings, m);
        m.heights = m.rawHeights;
        const std::vector<float> floorH = ErosionFloor(settings, m);
        m.seconds.resample = Since(t);
        t = Clock::now();
        StreamPower(settings, m, floorH);
        m.seconds.streamPower = Since(t);
        t = Clock::now();
        Thermal(settings, m, floorH);
        m.seconds.thermal = Since(t);
        t = Clock::now();
        Droplets(settings, m, floorH);
        m.seconds.droplets = Since(t);
        m.eroded = m.heights;
        t = Clock::now();
        Breach(settings, m);
        Lakes(settings, m);
        m.seconds.water = Since(t);
        t = Clock::now();
        Rivers(settings, m);
        m.seconds.rivers = Since(t);
        m.seconds.total = Since(start);
        return m;
    }

    uint64_t LandHash(const LandMap& m) {
        uint64_t h = Mix(uint64_t(m.width) << 32 | uint32_t(m.height));
        auto put = [&](uint64_t v) { h = Mix(h ^ v) + 0x9e3779b97f4a7c15ull; };
        put(std::bit_cast<uint32_t>(m.cellSize));
        for (size_t i = 0; i < m.heights.size(); ++i) {
            put(std::bit_cast<uint32_t>(m.heights[i]));
            put(std::bit_cast<uint32_t>(m.water[i]) ^ uint64_t(m.waterKind[i]) << 32 ^ uint64_t(m.hardness[i]) << 40);
        }
        for (const LandLake& lake : m.lakes) put(std::bit_cast<uint32_t>(lake.level) ^ uint64_t(lake.cells) << 32);
        for (const LandRiver& river : m.rivers) {
            put(river.cells.size() ^ uint64_t(river.end) << 40);
            for (uint32_t c : river.cells) put(c);
        }
        return h;
    }

    std::string CheckLand(const LandMap& m) {
        const uint32_t n = uint32_t(m.heights.size());
        for (uint32_t i = 0; i < n; ++i) {
            const bool sea = IsSea(m.biomes[i]);
            if (sea != (m.waterKind[i] == WaterKind::Sea)) return std::format("cell {}: the sea moved", i);
            if (sea && m.heights[i] >= 0.0f) return std::format("cell {}: a sea cell rose to {} m", i, m.heights[i]);
            if (!IsDrawnWater(m.biomes[i]) && m.heights[i] <= 0.0f && m.waterKind[i] == WaterKind::None) {
                return std::format("cell {}: dry land at {} m, at or below the sea", i, m.heights[i]);
            }
            if (m.waterKind[i] == WaterKind::Lake) {
                const LandLake& lake = m.lakes[m.lakeOf[i] - 1];
                if (m.water[i] != lake.level) return std::format("cell {}: a lake with two levels", i);
                if (m.heights[i] >= lake.level) return std::format("cell {}: a lake bed at or above its level", i);
            }
            if (m.waterKind[i] == WaterKind::River && m.heights[i] >= m.water[i]) {
                return std::format("cell {}: a river bed at or above its level", i);
            }
        }
        // Follow every river through the rivers it joins: it must end in the sea or a lake.
        std::vector<uint32_t> riverOf(n, UINT32_MAX);
        for (uint32_t k = 0; k < m.rivers.size(); ++k) {
            for (uint32_t c : m.rivers[k].cells) riverOf[c] = k;
        }
        for (uint32_t k = 0; k < m.rivers.size(); ++k) {
            const LandRiver& river = m.rivers[k];
            for (size_t j = 1; j < river.cells.size(); ++j) {
                if (m.water[river.cells[j]] > m.water[river.cells[j - 1]]) {
                    return std::format("river {}: its level rises downstream", k);
                }
            }
            uint32_t at = k;
            for (size_t hops = 0;; ++hops) {
                const LandRiver& r = m.rivers[at];
                if (r.end != RiverEnd::River) break;
                if (hops > m.rivers.size() || riverOf[r.endCell] == UINT32_MAX) {
                    return std::format("river {}: does not reach the sea or a lake", k);
                }
                at = riverOf[r.endCell];
            }
        }
        return {};
    }
}
