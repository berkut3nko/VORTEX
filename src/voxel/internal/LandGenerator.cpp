module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

module vortex.voxel;

import vortex.core;

// The baked world map as voxels (engine/docs/design/land.md "From the map to voxels", [land-3]). Unlike the bake, the
// sampling runs on every client in a session ([land-5]), so it uses only +, -, *, / and floor on floats and integer
// hashes: no libm function decides a voxel. LandSampling.GoldenHash pins it.

namespace vortex::voxel {

    namespace {
        constexpr int kTileSize = kChunkSize + 2;          // one voxel of border for slopes
        constexpr float kMaxTreeHeight = 7.5f;             // metres above ground a tree can reach
        constexpr float kMaxCanopyRadius = 2.6f;
        constexpr float kDarkStoneDepth = 12.0f;
        constexpr float kTreeSpacing = 5.0f;
        constexpr float kMaxDetail = 1.6f;                 // the largest detail amplitude (mountains)
        constexpr float kShoreMetres = 1.25f;              // sand this far from the water's edge

        float Lattice(uint32_t seed, int x, int z) { return float(Hash3(x, z, 0x1a7d, seed) >> 8) * (1.0f / 16777216.0f); }

        /// Smooth value noise in [0, 1); x and z in lattice steps.
        float Noise(uint32_t seed, float x, float z) {
            const float x0 = std::floor(x), z0 = std::floor(z);
            const int ix = int(x0), iz = int(z0);
            float fx = x - x0, fz = z - z0;
            fx = fx * fx * (3.0f - 2.0f * fx);
            fz = fz * fz * (3.0f - 2.0f * fz);
            const float a = Lattice(seed, ix, iz), b = Lattice(seed, ix + 1, iz);
            const float c = Lattice(seed, ix, iz + 1), d = Lattice(seed, ix + 1, iz + 1);
            const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
            return top + (bottom - top) * fz;
        }

        float BiomeDetail(uint8_t biome) {
            switch (LandBiome(biome)) {
            case LandBiome::Grassland: return 0.35f;
            case LandBiome::Forest:
            case LandBiome::DarkForest: return 0.5f;
            case LandBiome::Desert: return 0.7f;
            case LandBiome::Mountain: return kMaxDetail;
            case LandBiome::RockIsle: return 1.0f;
            default: return 0.0f;
            }
        }

        float TreeDensity(uint8_t biome) {
            switch (LandBiome(biome)) {
            case LandBiome::Grassland: return 0.05f;
            case LandBiome::Forest: return 0.55f;
            case LandBiome::DarkForest: return 0.7f;
            case LandBiome::Mountain: return 0.05f;
            default: return 0.0f;
            }
        }

        /// Reads the baked map at world positions (metres).
        class Sampler {
        public:
            Sampler(const LandMap& map, const LandGeneratorSettings& settings)
                : m(map), s(settings), m_Inv(1.0f / map.cellSize) {}

            uint32_t Cell(int i, int j) const {
                return m.Index(std::clamp(i, 0, m.width - 1), std::clamp(j, 0, m.height - 1));
            }

            /// Detail amplitude of a cell: none on water and next to it, so banks and shores keep their baked height.
            float Amplitude(int i, int j) const {
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di)
                        if (m.waterKind[Cell(i + di, j + dj)] != WaterKind::None) return 0.0f;
                return BiomeDetail(m.biomes[Cell(i, j)]);
            }

            float Height(float x, float z) const {
                const float u = (x - s.origin.x) * m_Inv - 0.5f, v = (z - s.origin.y) * m_Inv - 0.5f;
                const float u0 = std::floor(u), v0 = std::floor(v);
                const int i = int(u0), j = int(v0);
                const float tu = u - u0, tv = v - v0;
                float wu[4], wv[4];
                Weights(tu, wu);
                Weights(tv, wv);
                float h = 0.0f;
                for (int b = 0; b < 4; ++b) {
                    float row = 0.0f;
                    for (int a = 0; a < 4; ++a) row += wu[a] * m.heights[Cell(i - 1 + a, j - 1 + b)];
                    h += wv[b] * row;
                }
                // Catmull-Rom overshoots at a coast or a river bank: keep it inside the four nearest cells.
                const float h00 = m.heights[Cell(i, j)], h10 = m.heights[Cell(i + 1, j)];
                const float h01 = m.heights[Cell(i, j + 1)], h11 = m.heights[Cell(i + 1, j + 1)];
                h = std::clamp(h, std::min(std::min(h00, h10), std::min(h01, h11)), std::max(std::max(h00, h10), std::max(h01, h11)));
                const float a00 = Amplitude(i, j), a10 = Amplitude(i + 1, j), a01 = Amplitude(i, j + 1), a11 = Amplitude(i + 1, j + 1);
                const float atop = a00 + (a10 - a00) * tu, abottom = a01 + (a11 - a01) * tu;
                const float amp = atop + (abottom - atop) * tv;
                if (amp > 0.0f) {
                    const float n = (Noise(s.seed, x / 9.0f, z / 9.0f) - 0.5f) * 2.0f + (Noise(s.seed + 1u, x / 3.5f, z / 3.5f) - 0.5f);
                    h += n * amp * (2.0f / 3.0f);
                }
                return h;
            }

            float Water(float x, float z) const {
                const float u = (x - s.origin.x) * m_Inv - 0.5f, v = (z - s.origin.y) * m_Inv - 0.5f;
                const int i = int(std::floor(u)), j = int(std::floor(v));
                float level = kNoLandWater;
                for (int b = 0; b < 2; ++b)
                    for (int a = 0; a < 2; ++a) {
                        const uint32_t c = Cell(i + a, j + b);
                        if (m.waterKind[c] != WaterKind::None) level = std::max(level, m.water[c]);
                    }
                return level;
            }

            uint32_t NearestCell(float x, float z) const {
                return Cell(int(std::floor((x - s.origin.x) * m_Inv)), int(std::floor((z - s.origin.y) * m_Inv)));
            }

            /// The cell whose biome a column shows: the nearest cell to a jittered point, so biome borders wander.
            uint32_t BiomeCell(float x, float z) const {
                const float jx = (Noise(s.seed + 2u, x / 16.0f, z / 16.0f) - 0.5f) * 14.0f;
                const float jz = (Noise(s.seed + 3u, x / 16.0f, z / 16.0f) - 0.5f) * 14.0f;
                return NearestCell(x + jx, z + jz);
            }

            const LandMap& m;
            const LandGeneratorSettings& s;

        private:
            static void Weights(float t, float w[4]) {
                const float t2 = t * t, t3 = t2 * t;
                w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
                w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
                w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
                w[3] = 0.5f * (t3 - t2);
            }

            float m_Inv;
        };

        struct LandTile {
            float height[kTileSize * kTileSize];   // ground in voxels
            float water[kChunkSize * kChunkSize];  // water level in voxels, or a very low value
            uint32_t biomeCell[kChunkSize * kChunkSize];
            uint8_t shore[kChunkSize * kChunkSize] = {};   // within kShoreMetres of a wet column: sand
            float minHeight = 0.0f, maxHeight = 0.0f, maxWater = -1.0e30f;
            float At(int x, int z) const { return height[(x + 1) + (z + 1) * kTileSize]; }
        };

        class LandGenerator final : public IWorldGenerator {
        public:
            LandGenerator(std::shared_ptr<const LandMap> map, const LandGeneratorSettings& settings)
                : m_Map(std::move(map)), m_S(settings), m_Sampler(*m_Map, m_S), m_Inv(1.0f / settings.voxelSize) {
                float lo = 0.0f, hi = 0.0f;
                for (size_t i = 0; i < m_Map->heights.size(); ++i) {
                    lo = std::min(lo, m_Map->heights[i]);
                    hi = std::max(hi, m_Map->heights[i]);
                    if (m_Map->waterKind[i] != WaterKind::None) hi = std::max(hi, m_Map->water[i]);
                }
                m_Bounds.min = lo - kMaxDetail - kDarkStoneDepth - 2.0f * settings.voxelSize;
                m_Bounds.max = hi + kMaxDetail + (settings.trees ? kMaxTreeHeight : 0.0f) + 2.0f * settings.voxelSize;
            }

            HeightBounds VerticalBounds() const override { return m_Bounds; }

            ColumnClass ClassifyColumn(int cx, int cz) const override { return Classify(*Tile(cx, cz)); }

            ChunkContent Generate(const glm::ivec3& coord, Chunk& chunk) const override {
                const std::shared_ptr<const LandTile> tile = Tile(coord.x, coord.z);
                const WorldMaterials& mat = m_S.materials;
                const LandMap& map = *m_Map;
                const float vs = m_S.voxelSize;
                const int y0 = coord.y * kChunkSize, y1 = y0 + kChunkSize;
                if (m_S.skipClassifiedRows) {
                    const ColumnClass column = Classify(*tile);
                    if (coord.y >= column.emptyFrom) return {ChunkKind::Empty, 0};
                    if (coord.y < column.solidBelow) return {ChunkKind::Uniform, column.solidId};
                }
                const float grassV = std::max(1.0f, 0.25f * m_Inv), dirtV = 2.0f * m_Inv, beachV = 1.5f * m_Inv;
                const float duneV = 3.0f * m_Inv, snowV = std::max(1.0f, 0.4f * m_Inv), darkV = kDarkStoneDepth * m_Inv;

                for (int z = 0; z < kChunkSize; ++z) {
                    for (int x = 0; x < kChunkSize; ++x) {
                        const float h = tile->At(x, z);
                        const int surface = int(std::floor(h));
                        const float wv = tile->water[x + z * kChunkSize];
                        const int waterTop = wv > -1.0e29f ? int(std::floor(wv)) : INT32_MIN;
                        if (surface <= y0 && waterTop <= y0) continue;

                        const int yEnd = std::min(surface, y1);
                        if (yEnd > y0) {
                            const float slope = 0.5f * std::max(std::abs(tile->At(x + 1, z) - tile->At(x - 1, z)),
                                                                std::abs(tile->At(x, z + 1) - tile->At(x, z - 1)));
                            const float hm = h * vs;
                            const uint32_t cell = tile->biomeCell[x + z * kChunkSize];
                            const uint8_t biome = map.biomes[cell];
                            const bool underwater = waterTop > surface;
                            const bool shore = tile->shore[x + z * kChunkSize] != 0;

                            enum class Cover { Grass, DryGrass, Beach, Dune, Snow, Rock };
                            Cover cover = Cover::Grass;
                            if (underwater || shore) cover = Cover::Beach;
                            else if (slope > 1.15f || (map.hardness[cell] >= 9 && slope > 0.6f)) cover = Cover::Rock;
                            else {
                                switch (LandBiome(biome)) {
                                case LandBiome::Desert: cover = slope > 0.8f ? Cover::Rock : Cover::Dune; break;
                                case LandBiome::Mountain:
                                    cover = hm > 92.0f ? Cover::Snow : (slope > 0.55f || hm > 45.0f ? Cover::Rock : Cover::DryGrass);
                                    break;
                                case LandBiome::RockIsle: cover = Cover::Rock; break;
                                case LandBiome::Sea:
                                case LandBiome::Lake: cover = Cover::Beach; break;
                                default: break;
                                }
                            }
                            for (int y = y0; y < yEnd; ++y) {
                                const float depth = float(surface - y);
                                uint8_t id = mat.stone;
                                if (depth > darkV) id = mat.stoneDark;
                                else {
                                    switch (cover) {
                                    case Cover::Grass: id = depth <= grassV ? mat.grass : (depth <= dirtV ? mat.dirt : mat.stone); break;
                                    case Cover::DryGrass: id = depth <= grassV ? mat.grassDry : (depth <= dirtV ? mat.dirt : mat.stone); break;
                                    case Cover::Beach: id = depth <= beachV ? mat.sand : mat.stone; break;
                                    case Cover::Dune: id = depth <= duneV ? mat.sand : mat.stone; break;
                                    case Cover::Snow: id = depth <= snowV ? mat.snow : mat.stone; break;
                                    case Cover::Rock: id = mat.stone; break;
                                    }
                                }
                                chunk.ids[Chunk::VoxelIndex(x, y - y0, z)] = id;
                            }
                        }
                        if (waterTop > surface) {
                            const int wEnd = std::min(waterTop, y1);
                            for (int y = std::max(surface, y0); y < wEnd; ++y) chunk.ids[Chunk::VoxelIndex(x, y - y0, z)] = mat.water;
                        }
                    }
                }
                if (m_S.trees && float(y1) > tile->minHeight - 2.0f && float(y0) < tile->maxHeight + kMaxTreeHeight * m_Inv) {
                    PlantTrees(coord, chunk);
                }
                return {ChunkKind::Dense, 0};
            }

        private:
            ColumnClass Classify(const LandTile& tile) const {
                ColumnClass c;
                c.known = true;
                const float top = std::max(tile.maxHeight, tile.maxWater) + (m_S.trees ? kMaxTreeHeight * m_Inv : 0.0f) + 1.0f;
                const auto isEmpty = [top](int row) { return float(row * kChunkSize) > top; };
                const float bottom = tile.minHeight - kDarkStoneDepth * m_Inv - 2.0f;
                const auto isSolid = [bottom](int row) { return float((row + 1) * kChunkSize) < bottom; };
                int e = int(std::floor(top / kChunkSize));
                while (isEmpty(e - 1)) --e;
                while (!isEmpty(e)) ++e;
                c.emptyFrom = e;
                int b = int(std::floor(bottom / kChunkSize));
                while (isSolid(b)) ++b;
                while (!isSolid(b - 1)) --b;
                c.solidBelow = b;
                c.solidId = m_S.materials.stoneDark;
                const int lowestSurfaceVoxel = int(std::floor(tile.minHeight)) - 1;
                c.surfaceFrom = std::max(c.solidBelow, FloorDiv(lowestSurfaceVoxel, kChunkSize) - 1);
                return c;
            }

            static int FloorDiv(int a, int b) { return a / b - ((a % b != 0) && (a < 0) ? 1 : 0); }

            std::shared_ptr<const LandTile> Tile(int cx, int cz) const {
                const uint64_t key = uint64_t(uint32_t(cx)) | (uint64_t(uint32_t(cz)) << 32);
                {
                    std::scoped_lock lock(m_CacheMutex);
                    auto it = m_Cache.find(key);
                    if (it != m_Cache.end()) return it->second;
                }
                auto tile = std::make_shared<LandTile>();
                float mn = 3.0e38f, mx = -3.0e38f, mw = -1.0e30f;
                const float vs = m_S.voxelSize;
                for (int z = -1; z <= kChunkSize; ++z) {
                    for (int x = -1; x <= kChunkSize; ++x) {
                        const float wx = (float(cx * kChunkSize + x) + 0.5f) * vs;
                        const float wz = (float(cz * kChunkSize + z) + 0.5f) * vs;
                        const float h = m_Sampler.Height(wx, wz) * m_Inv;
                        tile->height[(x + 1) + (z + 1) * kTileSize] = h;
                        if (x < 0 || x >= kChunkSize || z < 0 || z >= kChunkSize) continue;
                        mn = std::min(mn, h);
                        mx = std::max(mx, h);
                        const float w = m_Sampler.Water(wx, wz);
                        const float wvox = w > kNoLandWater ? w * m_Inv : -1.0e30f;
                        tile->water[x + z * kChunkSize] = wvox;
                        mw = std::max(mw, wvox);
                        tile->biomeCell[x + z * kChunkSize] = m_Sampler.BiomeCell(wx, wz);
                    }
                }
                MarkShore(cx, cz, *tile);
                tile->minHeight = mn;
                tile->maxHeight = mx;
                tile->maxWater = mw;
                std::scoped_lock lock(m_CacheMutex);
                if (m_Cache.size() > 4096) m_Cache.clear();
                auto [it, inserted] = m_Cache.emplace(key, std::move(tile));
                return it->second;
            }

            /// Sand along the water's edge: columns within kShoreMetres of a column whose water stands above its ground
            /// (a square around each, so the band follows the shore, not the map's cells). Skipped where no cell near the
            /// tile holds water.
            void MarkShore(int cx, int cz, LandTile& tile) const {
                const float vs = m_S.voxelSize;
                const float x0 = float(cx * kChunkSize) * vs, z0 = float(cz * kChunkSize) * vs, size = float(kChunkSize) * vs;
                const LandMap& map = *m_Map;
                const float pad = kShoreMetres + 2.0f * map.cellSize;
                bool water = false;
                for (float z = z0 - pad; z <= z0 + size + pad && !water; z += map.cellSize)
                    for (float x = x0 - pad; x <= x0 + size + pad && !water; x += map.cellSize)
                        water = map.waterKind[m_Sampler.NearestCell(x, z)] != WaterKind::None;
                if (!water) return;
                const int r = std::max(1, int(std::ceil(kShoreMetres * m_Inv)));
                const int n = kChunkSize + 2 * r;
                std::vector<uint8_t> wet(size_t(n) * size_t(n)), rows(size_t(n) * size_t(n));
                for (int z = 0; z < n; ++z)
                    for (int x = 0; x < n; ++x) {
                        const float wx = (float(cx * kChunkSize + x - r) + 0.5f) * vs;
                        const float wz = (float(cz * kChunkSize + z - r) + 0.5f) * vs;
                        const float level = m_Sampler.Water(wx, wz);
                        wet[size_t(x + z * n)] = level > kNoLandWater && std::floor(level * m_Inv) > std::floor(m_Sampler.Height(wx, wz) * m_Inv);
                    }
                // A square dilation by r: along x, then along z.
                for (int z = 0; z < n; ++z)
                    for (int x = r; x < n - r; ++x) {
                        uint8_t any = 0;
                        for (int k = -r; k <= r && !any; ++k) any = wet[size_t(x + k + z * n)];
                        rows[size_t(x + z * n)] = any;
                    }
                for (int z = 0; z < kChunkSize; ++z)
                    for (int x = 0; x < kChunkSize; ++x) {
                        uint8_t any = 0;
                        for (int k = 0; k <= 2 * r && !any; ++k) any = rows[size_t(x + r + (z + k) * n)];
                        tile.shore[x + z * kChunkSize] = any;
                    }
            }

            /// Trees on a jittered 5 m grid, each cell grows one with its biome's chance; a chunk draws the part of
            /// each nearby tree inside it.
            void PlantTrees(const glm::ivec3& coord, Chunk& chunk) const {
                const WorldMaterials& mat = m_S.materials;
                const LandMap& map = *m_Map;
                const float vs = m_S.voxelSize;
                const float cell = kTreeSpacing;
                const glm::ivec3 origin = coord * kChunkSize;
                const float x0 = float(origin.x) * vs, x1 = x0 + kChunkSize * vs;
                const float z0 = float(origin.z) * vs, z1 = z0 + kChunkSize * vs;
                const int i0 = int(std::floor((x0 - kMaxCanopyRadius) / cell)), i1 = int(std::floor((x1 + kMaxCanopyRadius) / cell));
                const int j0 = int(std::floor((z0 - kMaxCanopyRadius) / cell)), j1 = int(std::floor((z1 + kMaxCanopyRadius) / cell));
                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i) {
                        const uint32_t h = Hash3(i, j, 0x7ee5, m_S.seed);
                        const float r1 = HashToFloat(Hash32(h + 1u)), r2 = HashToFloat(Hash32(h + 2u));
                        const float tx = (float(i) + 0.15f + 0.7f * r1) * cell;
                        const float tz = (float(j) + 0.15f + 0.7f * r2) * cell;
                        const uint8_t biome = map.biomes[m_Sampler.NearestCell(tx, tz)];
                        if (HashToFloat(h) >= TreeDensity(biome)) continue;
                        const float ground = m_Sampler.Height(tx, tz);
                        if (ground < 1.2f || (biome == uint8_t(LandBiome::Mountain) && ground > 50.0f)) continue;
                        if (m_Sampler.Water(tx, tz) > kNoLandWater) continue;
                        if (std::abs(m_Sampler.Height(tx + 0.6f, tz) - ground) > 0.5f ||
                            std::abs(m_Sampler.Height(tx, tz + 0.6f) - ground) > 0.5f) continue;
                        const float r3 = HashToFloat(Hash32(h + 3u)), r4 = HashToFloat(Hash32(h + 4u));
                        const float r5 = HashToFloat(Hash32(h + 5u)), r6 = HashToFloat(Hash32(h + 6u));
                        const float trunkHeight = 2.2f + 2.4f * r3;
                        const float canopyRadius = 1.2f + 1.3f * r4;
                        const bool dark = biome == uint8_t(LandBiome::DarkForest);
                        const uint8_t leaf = !dark && r5 < 0.15f ? mat.leavesAutumn : mat.leaves;

                        const glm::ivec3 base(int(std::floor(tx * m_Inv)), int(std::floor(ground * m_Inv)), int(std::floor(tz * m_Inv)));
                        const int trunkV = std::max(2, int(trunkHeight * m_Inv));
                        const float trunkWidthV = 0.24f * (0.8f + 0.6f * r6) * m_Inv;
                        const int trunkHalf = std::max(0, int(std::floor(0.5f * (trunkWidthV - 1.0f) + 0.5f)));
                        const float canopyV = canopyRadius * m_Inv;
                        const glm::ivec3 crown = base + glm::ivec3(0, trunkV, 0);
                        const int reach = int(std::ceil(canopyV));
                        const glm::ivec3 cmin = glm::max(crown - glm::ivec3(reach), origin) - origin;
                        const glm::ivec3 cmax = glm::min(crown + glm::ivec3(reach + 1), origin + glm::ivec3(kChunkSize)) - origin;
                        for (int z = cmin.z; z < cmax.z; ++z)
                            for (int y = cmin.y; y < cmax.y; ++y)
                                for (int x = cmin.x; x < cmax.x; ++x) {
                                    const glm::ivec3 w = origin + glm::ivec3(x, y, z);
                                    const glm::vec3 d = glm::vec3(w - crown) / glm::vec3(canopyV, canopyV * 0.8f, canopyV);
                                    const float n = HashToFloat(Hash3(w.x, w.y, w.z, m_S.seed));
                                    if (d.x * d.x + d.y * d.y + d.z * d.z + 0.45f * n * n > 1.0f) continue;
                                    uint8_t& id = chunk.ids[Chunk::VoxelIndex(x, y, z)];
                                    if (id == 0) id = leaf;
                                }
                        const int tyMin = std::max(base.y - 2, origin.y) - origin.y;
                        const int tyMax = std::min(base.y + trunkV + 1, origin.y + kChunkSize) - origin.y;
                        for (int dz = -trunkHalf; dz <= trunkHalf; ++dz)
                            for (int dx = -trunkHalf; dx <= trunkHalf; ++dx) {
                                if (trunkHalf > 0 && dx * dx + dz * dz > trunkHalf * trunkHalf + 1) continue;
                                const int lx = base.x + dx - origin.x, lz = base.z + dz - origin.z;
                                if (lx < 0 || lx >= kChunkSize || lz < 0 || lz >= kChunkSize) continue;
                                for (int y = tyMin; y < tyMax; ++y) chunk.ids[Chunk::VoxelIndex(lx, y, lz)] = mat.wood;
                            }
                    }
            }

            std::shared_ptr<const LandMap> m_Map;
            LandGeneratorSettings m_S;
            Sampler m_Sampler;
            float m_Inv;
            HeightBounds m_Bounds;
            mutable std::mutex m_CacheMutex;
            mutable std::unordered_map<uint64_t, std::shared_ptr<const LandTile>> m_Cache;
        };
    }

    glm::vec2 CenteredLandOrigin(const LandMap& map) {
        return glm::vec2(-0.5f * float(map.width) * map.cellSize, -0.5f * float(map.height) * map.cellSize);
    }

    std::shared_ptr<IWorldGenerator> CreateLandGenerator(std::shared_ptr<const LandMap> map, const LandGeneratorSettings& settings) {
        return std::make_shared<LandGenerator>(std::move(map), settings);
    }

    float LandHeight(const LandMap& map, const LandGeneratorSettings& settings, float x, float z) {
        return Sampler(map, settings).Height(x, z);
    }

    float LandWaterLevel(const LandMap& map, const LandGeneratorSettings& settings, float x, float z) {
        return Sampler(map, settings).Water(x, z);
    }

    glm::vec2 LandSpawn(const LandMap& m, const LandGeneratorSettings& settings, int region) {
        if (region == 0) region = m.starterRegion;
        double sx = 0.0, sz = 0.0;
        uint64_t count = 0;
        for (int z = 0; z < m.height; ++z)
            for (int x = 0; x < m.width; ++x)
                if (m.regions[m.Index(x, z)] == region && m.waterKind[m.Index(x, z)] == WaterKind::None) {
                    sx += x;
                    sz += z;
                    ++count;
                }
        const double cx = count ? sx / double(count) : 0.5 * m.width, cz = count ? sz / double(count) : 0.5 * m.height;
        auto level = [&](int x, int z) {
            if (x < 3 || z < 3 || x + 3 >= m.width || z + 3 >= m.height) return false;
            const uint32_t c = m.Index(x, z);
            if (region != 0 && m.regions[c] != region) return false;
            for (int dz = -3; dz <= 3; ++dz)
                for (int dx = -3; dx <= 3; ++dx) {
                    const uint32_t n = m.Index(x + dx, z + dz);
                    if (m.waterKind[n] != WaterKind::None) return false;
                    if (std::abs(dx) <= 1 && std::abs(dz) <= 1 && std::abs(m.heights[n] - m.heights[c]) > 0.5f) return false;
                }
            return true;
        };
        // The least steep dry cell of the region, for a region with no level ground.
        auto steepness = [&](int x, int z) {
            float worst = 0.0f;
            const uint32_t c = m.Index(x, z);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    worst = std::max(worst, std::abs(m.heights[m.Index(std::clamp(x + dx, 0, m.width - 1), std::clamp(z + dz, 0, m.height - 1))] -
                                                     m.heights[c]));
            return worst;
        };
        int best = -1, gentlestCell = 0;
        double bestDistance = 1.0e30;
        float gentlest = 1.0e30f;
        for (int z = 0; z < m.height; ++z)
            for (int x = 0; x < m.width; ++x) {
                const uint32_t c = m.Index(x, z);
                if (m.waterKind[c] == WaterKind::None && (region == 0 || m.regions[c] == region)) {
                    const float s = steepness(x, z);
                    if (s < gentlest) {
                        gentlest = s;
                        gentlestCell = int(c);
                    }
                }
                if (!level(x, z)) continue;
                const double d = (x - cx) * (x - cx) + (z - cz) * (z - cz);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = int(c);
                }
            }
        const int cell = best >= 0 ? best : gentlestCell;
        const int x = cell % m.width, z = cell / m.width;
        return settings.origin + glm::vec2((float(x) + 0.5f) * m.cellSize, (float(z) + 0.5f) * m.cellSize);
    }
}
