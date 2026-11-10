module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

module vortex.voxel;

import vortex.core;
import vortex.math;

namespace vortex::voxel {

    namespace {
        constexpr int kTileSize = kChunkSize + 2;          // one voxel of border for slopes
        constexpr float kMaxTreeHeight = 7.5f;             // metres above ground a tree can reach
        constexpr float kMaxCanopyRadius = 2.6f;           // metres
        constexpr float kDarkStoneDepth = 12.0f;           // metres below the surface

        /// Terrain surface of one chunk column (shared by all chunks stacked above each other).
        struct HeightTile {
            float height[kTileSize * kTileSize];           // surface height in voxels
            float minHeight = 0.0f;                        // over the inner 32 x 32
            float maxHeight = 0.0f;
            float At(int x, int z) const { return height[(x + 1) + (z + 1) * kTileSize]; }   // x, z in [-1, 32]
        };

        class TerrainGenerator final : public IWorldGenerator {
        public:
            explicit TerrainGenerator(const TerrainSettings& settings)
                : m_S(settings),
                  m_Inv(1.0f / settings.voxelSize),
                  m_Continent(settings.seed * 7919u + 1u),
                  m_Hills(settings.seed * 7919u + 2u),
                  m_Mountain(settings.seed * 7919u + 3u),
                  m_Detail(settings.seed * 7919u + 4u),
                  m_Biome(settings.seed * 7919u + 5u),
                  m_CaveA(settings.seed * 7919u + 6u),
                  m_CaveB(settings.seed * 7919u + 7u) {}

            ChunkContent Generate(const glm::ivec3& coord, Chunk& chunk) const override {
                const std::shared_ptr<const HeightTile> tile = Tile(coord.x, coord.z);
                const WorldMaterials& mat = m_S.materials;
                const float vs = m_S.voxelSize;
                const int y0 = coord.y * kChunkSize;
                const int y1 = y0 + kChunkSize;
                const float seaV = m_S.seaLevel * m_Inv;
                const int seaLevelVoxel = static_cast<int>(std::floor(seaV));

                // Entirely above everything (terrain, water, tree tops): air.
                const float top = std::max(tile->maxHeight, seaV) + (m_S.trees ? kMaxTreeHeight * m_Inv : 0.0f);
                if (static_cast<float>(y0) > top + 1.0f) return {ChunkKind::Empty, 0};

                // Entirely below the deepest thing that varies (caves, soil layers): solid rock.
                const float variableDepth = std::max(m_S.caves ? m_S.caveMaxDepth : 0.0f, kDarkStoneDepth) * m_Inv + 2.0f;
                if (static_cast<float>(y1) < tile->minHeight - variableDepth) return {ChunkKind::Uniform, mat.stoneDark};

                const float grassV = std::max(1.0f, 0.25f * m_Inv);
                const float dirtV = 2.0f * m_Inv;
                const float sandV = 1.5f * m_Inv;
                const float snowV = std::max(1.0f, 0.4f * m_Inv);
                const float darkV = kDarkStoneDepth * m_Inv;

                for (int z = 0; z < kChunkSize; ++z) {
                    for (int x = 0; x < kChunkSize; ++x) {
                        const float h = tile->At(x, z);
                        const int surface = static_cast<int>(std::floor(h));     // voxels with y < surface are ground
                        if (surface <= y0 && seaLevelVoxel <= y0) continue;      // column is above this chunk

                        const int yEnd = std::min(surface, y1);
                        if (yEnd > y0) {
                            const float slope = 0.5f * std::max(std::abs(tile->At(x + 1, z) - tile->At(x - 1, z)),
                                                                std::abs(tile->At(x, z + 1) - tile->At(x, z - 1)));
                            const float hm = h * vs;
                            const float wxm = static_cast<float>(coord.x * kChunkSize + x) * vs;
                            const float wzm = static_cast<float>(coord.z * kChunkSize + z) * vs;
                            const float biome = m_Biome.Sample(wxm / 55.0f, wzm / 55.0f);

                            enum class Cover { Grass, Sand, Snow, Rock };
                            Cover cover = Cover::Grass;
                            if (hm < m_S.seaLevel + 0.9f + 0.4f * biome) cover = Cover::Sand;
                            else if (slope > 1.15f) cover = Cover::Rock;
                            else if (hm > m_S.snowLine + 5.0f * biome) cover = Cover::Snow;
                            const uint8_t grass = biome > 0.35f ? mat.grassDry : mat.grass;

                            for (int y = y0; y < yEnd; ++y) {
                                const float depth = static_cast<float>(surface - y);
                                uint8_t id;
                                if (depth > darkV) id = mat.stoneDark;
                                else if (cover == Cover::Rock) id = mat.stone;
                                else if (cover == Cover::Sand) id = depth <= sandV ? mat.sand : mat.stone;
                                else if (cover == Cover::Snow) id = depth <= snowV ? mat.snow : (depth <= dirtV ? mat.dirt : mat.stone);
                                else id = depth <= grassV ? grass : (depth <= dirtV ? mat.dirt : mat.stone);
                                chunk.ids[Chunk::VoxelIndex(x, y - y0, z)] = id;
                            }
                        }

                        // Sea: fill from the sea bed up to the water line.
                        if (surface < seaLevelVoxel) {
                            const int wEnd = std::min(seaLevelVoxel, y1);
                            for (int y = std::max(surface, y0); y < wEnd; ++y) chunk.ids[Chunk::VoxelIndex(x, y - y0, z)] = mat.water;
                        }
                    }
                }

                if (m_S.caves && static_cast<float>(y1) > tile->minHeight - m_S.caveMaxDepth * m_Inv &&
                    static_cast<float>(y0) < tile->maxHeight) {
                    CarveCaves(coord, *tile, chunk);
                }
                if (m_S.trees && static_cast<float>(y1) > tile->minHeight - 2.0f &&
                    static_cast<float>(y0) < tile->maxHeight + kMaxTreeHeight * m_Inv) {
                    PlantTrees(coord, chunk);
                }
                return {ChunkKind::Dense, 0};
            }

        private:
            /// Terrain height in metres at a world position (metres).
            float HeightMeters(float x, float z) const {
                const float s = 1.0f / m_S.horizontalScale;
                const float continent = m_Continent.Fbm(x * s / 420.0f, z * s / 420.0f, 3);
                const float hills = m_Hills.Fbm(x * s / 95.0f, z * s / 95.0f, 4);
                const float ridge = m_Mountain.Ridged(x * s / 300.0f, z * s / 300.0f, 5);
                const float detail = m_Detail.Fbm(x * s / 11.0f, z * s / 11.0f, 2);
                const float inland = math::SmoothStep(0.05f, 0.55f, continent);     // mountains only rise inland
                return m_S.baseHeight + continent * m_S.continentHeight +
                       hills * m_S.hillHeight * (0.35f + 0.65f * (continent * 0.5f + 0.5f)) +
                       ridge * ridge * m_S.mountainHeight * inland + detail * 0.35f;
            }

            std::shared_ptr<const HeightTile> Tile(int cx, int cz) const {
                const uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(cx)) | (static_cast<uint64_t>(static_cast<uint32_t>(cz)) << 32);
                {
                    std::scoped_lock lock(m_CacheMutex);
                    auto it = m_Cache.find(key);
                    if (it != m_Cache.end()) return it->second;
                }

                auto tile = std::make_shared<HeightTile>();
                float mn = 3.0e38f, mx = -3.0e38f;
                for (int z = -1; z <= kChunkSize; ++z)
                    for (int x = -1; x <= kChunkSize; ++x) {
                        const float wx = (static_cast<float>(cx * kChunkSize + x) + 0.5f) * m_S.voxelSize;
                        const float wz = (static_cast<float>(cz * kChunkSize + z) + 0.5f) * m_S.voxelSize;
                        const float h = HeightMeters(wx, wz) * m_Inv;
                        tile->height[(x + 1) + (z + 1) * kTileSize] = h;
                        if (x >= 0 && x < kChunkSize && z >= 0 && z < kChunkSize) {
                            mn = std::min(mn, h);
                            mx = std::max(mx, h);
                        }
                    }
                tile->minHeight = mn;
                tile->maxHeight = mx;

                std::scoped_lock lock(m_CacheMutex);
                if (m_Cache.size() > 4096) m_Cache.clear();   // bounded memory; tiles are cheap to rebuild
                auto [it, inserted] = m_Cache.emplace(key, std::move(tile));
                return it->second;
            }

            /// Tunnels where two independent noise fields are both near zero.
            void CarveCaves(const glm::ivec3& coord, const HeightTile& tile, Chunk& chunk) const {
                constexpr int kStep = 4;
                constexpr int kLattice = kChunkSize / kStep + 1;
                float a[kLattice][kLattice][kLattice];
                float b[kLattice][kLattice][kLattice];
                const float vs = m_S.voxelSize;
                const glm::ivec3 origin = coord * kChunkSize;
                for (int k = 0; k < kLattice; ++k)
                    for (int j = 0; j < kLattice; ++j)
                        for (int i = 0; i < kLattice; ++i) {
                            const float x = static_cast<float>(origin.x + i * kStep) * vs;
                            const float y = static_cast<float>(origin.y + j * kStep) * vs;
                            const float z = static_cast<float>(origin.z + k * kStep) * vs;
                            a[k][j][i] = m_CaveA.Sample(x / 9.0f, y / 6.0f, z / 9.0f);
                            b[k][j][i] = m_CaveB.Sample(x / 9.0f + 31.4f, y / 6.0f - 5.2f, z / 9.0f - 17.2f);
                        }

                const float caveDepthV = m_S.caveMaxDepth * m_Inv;
                const float crustV = 1.0f * m_Inv;
                const float seaV = m_S.seaLevel * m_Inv;
                const uint8_t water = m_S.materials.water;

                auto lerp3 = [](const float f[kLattice][kLattice][kLattice], int i, int j, int k, float fx, float fy, float fz) {
                    const float c00 = f[k][j][i] + (f[k][j][i + 1] - f[k][j][i]) * fx;
                    const float c10 = f[k][j + 1][i] + (f[k][j + 1][i + 1] - f[k][j + 1][i]) * fx;
                    const float c01 = f[k + 1][j][i] + (f[k + 1][j][i + 1] - f[k + 1][j][i]) * fx;
                    const float c11 = f[k + 1][j + 1][i] + (f[k + 1][j + 1][i + 1] - f[k + 1][j + 1][i]) * fx;
                    const float c0 = c00 + (c10 - c00) * fy;
                    const float c1 = c01 + (c11 - c01) * fy;
                    return c0 + (c1 - c0) * fz;
                };

                for (int z = 0; z < kChunkSize; ++z)
                    for (int x = 0; x < kChunkSize; ++x) {
                        const float h = tile.At(x, z);
                        // Keep a thicker roof under the sea so caves never drain into it.
                        const float roof = h < seaV ? crustV * 3.0f : crustV;
                        for (int y = 0; y < kChunkSize; ++y) {
                            uint8_t& id = chunk.ids[Chunk::VoxelIndex(x, y, z)];
                            if (id == 0 || id == water) continue;
                            const float depth = h - static_cast<float>(origin.y + y);
                            if (depth < roof || depth > caveDepthV) continue;
                            const int i = x / kStep, j = y / kStep, k = z / kStep;
                            const float fx = static_cast<float>(x % kStep) / kStep;
                            const float fy = static_cast<float>(y % kStep) / kStep;
                            const float fz = static_cast<float>(z % kStep) / kStep;
                            const float va = lerp3(a, i, j, k, fx, fy, fz);
                            const float vb = lerp3(b, i, j, k, fx, fy, fz);
                            // Tunnels narrow towards the bottom of the cave layer.
                            const float fade = 1.0f - math::Saturate((depth - caveDepthV * 0.7f) / (caveDepthV * 0.3f));
                            if (va * va + vb * vb < 0.013f * fade) id = 0;
                        }
                    }
            }

            /// Trees live on a jittered grid; every chunk rasterises the part of each nearby tree
            /// that falls inside it, so trees crossing chunk borders come out seamless.
            void PlantTrees(const glm::ivec3& coord, Chunk& chunk) const {
                const WorldMaterials& mat = m_S.materials;
                const float vs = m_S.voxelSize;
                const float cell = m_S.treeSpacing;
                const glm::ivec3 origin = coord * kChunkSize;
                const float x0 = static_cast<float>(origin.x) * vs, x1 = x0 + kChunkSize * vs;
                const float z0 = static_cast<float>(origin.z) * vs, z1 = z0 + kChunkSize * vs;

                const int i0 = static_cast<int>(std::floor((x0 - kMaxCanopyRadius) / cell));
                const int i1 = static_cast<int>(std::floor((x1 + kMaxCanopyRadius) / cell));
                const int j0 = static_cast<int>(std::floor((z0 - kMaxCanopyRadius) / cell));
                const int j1 = static_cast<int>(std::floor((z1 + kMaxCanopyRadius) / cell));

                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i) {
                        const uint32_t h = Hash3(i, j, 0x7ee5, m_S.seed);
                        if (HashToFloat(h) >= m_S.treeDensity) continue;
                        const float r1 = HashToFloat(Hash32(h + 1u)), r2 = HashToFloat(Hash32(h + 2u));
                        const float r3 = HashToFloat(Hash32(h + 3u)), r4 = HashToFloat(Hash32(h + 4u));
                        const float r5 = HashToFloat(Hash32(h + 5u)), r6 = HashToFloat(Hash32(h + 6u));

                        const float tx = (static_cast<float>(i) + 0.15f + 0.7f * r1) * cell;
                        const float tz = (static_cast<float>(j) + 0.15f + 0.7f * r2) * cell;
                        const float ground = HeightMeters(tx, tz);
                        if (ground < m_S.seaLevel + 1.2f || ground > m_S.snowLine - 3.0f) continue;
                        if (std::abs(HeightMeters(tx + 0.6f, tz) - ground) > 0.5f ||
                            std::abs(HeightMeters(tx, tz + 0.6f) - ground) > 0.5f) continue;      // too steep

                        const float trunkHeight = 2.2f + 2.4f * r3;
                        const float canopyRadius = 1.2f + 1.3f * r4;
                        const uint8_t leaf = r5 < 0.2f ? mat.leavesAutumn : mat.leaves;

                        const glm::ivec3 base(static_cast<int>(std::floor(tx * m_Inv)), static_cast<int>(std::floor(ground * m_Inv)),
                                              static_cast<int>(std::floor(tz * m_Inv)));
                        const int trunkV = std::max(2, static_cast<int>(trunkHeight * m_Inv));
                        const int trunkHalf = std::max(0, static_cast<int>(std::lround(0.12f * m_Inv * (0.8f + 0.6f * r6))) - 1);
                        const float canopyV = canopyRadius * m_Inv;
                        const glm::ivec3 crown = base + glm::ivec3(0, trunkV, 0);

                        // Canopy: a noisy ellipsoid of leaves (never overwrites existing voxels).
                        const int reach = static_cast<int>(std::ceil(canopyV));
                        const glm::ivec3 cmin = glm::max(crown - glm::ivec3(reach), origin) - origin;
                        const glm::ivec3 cmax = glm::min(crown + glm::ivec3(reach + 1), origin + glm::ivec3(kChunkSize)) - origin;
                        for (int z = cmin.z; z < cmax.z; ++z)
                            for (int y = cmin.y; y < cmax.y; ++y)
                                for (int x = cmin.x; x < cmax.x; ++x) {
                                    const glm::ivec3 w = origin + glm::ivec3(x, y, z);
                                    const glm::vec3 d = glm::vec3(w - crown) / glm::vec3(canopyV, canopyV * 0.8f, canopyV);
                                    const float n = HashToFloat(Hash3(w.x, w.y, w.z, m_S.seed));
                                    if (glm::dot(d, d) + 0.45f * n * n > 1.0f) continue;
                                    uint8_t& id = chunk.ids[Chunk::VoxelIndex(x, y, z)];
                                    if (id == 0) id = leaf;
                                }

                        // Trunk (drawn last so it replaces leaves and roots into the ground).
                        const int tyMin = std::max(base.y - 2, origin.y) - origin.y;
                        const int tyMax = std::min(base.y + trunkV + 1, origin.y + kChunkSize) - origin.y;
                        for (int dz = -trunkHalf; dz <= trunkHalf; ++dz)
                            for (int dx = -trunkHalf; dx <= trunkHalf; ++dx) {
                                if (trunkHalf > 0 && dx * dx + dz * dz > trunkHalf * trunkHalf + 1) continue;
                                const int lx = base.x + dx - origin.x;
                                const int lz = base.z + dz - origin.z;
                                if (lx < 0 || lx >= kChunkSize || lz < 0 || lz >= kChunkSize) continue;
                                for (int y = tyMin; y < tyMax; ++y) chunk.ids[Chunk::VoxelIndex(lx, y, lz)] = mat.wood;
                            }
                    }
            }

            TerrainSettings m_S;
            float m_Inv;
            math::SimplexNoise m_Continent, m_Hills, m_Mountain, m_Detail, m_Biome, m_CaveA, m_CaveB;
            mutable std::mutex m_CacheMutex;
            mutable std::unordered_map<uint64_t, std::shared_ptr<const HeightTile>> m_Cache;
        };

        class FlatGenerator final : public IWorldGenerator {
        public:
            FlatGenerator(int height, uint8_t surfaceId, uint8_t groundId) : m_Height(height), m_Surface(surfaceId), m_Ground(groundId) {}

            ChunkContent Generate(const glm::ivec3& coord, Chunk& chunk) const override {
                const int y0 = coord.y * kChunkSize;
                if (y0 >= m_Height) return {ChunkKind::Empty, 0};
                if (y0 + kChunkSize <= m_Height - 1) return {ChunkKind::Uniform, m_Ground};
                const int yEnd = std::min(m_Height, y0 + kChunkSize);
                for (int y = y0; y < yEnd; ++y) {
                    const uint8_t id = (y == m_Height - 1) ? m_Surface : m_Ground;
                    for (int z = 0; z < kChunkSize; ++z)
                        for (int x = 0; x < kChunkSize; ++x) chunk.ids[Chunk::VoxelIndex(x, y - y0, z)] = id;
                }
                return {ChunkKind::Dense, 0};
            }

        private:
            int m_Height;
            uint8_t m_Surface, m_Ground;
        };
    }

    std::shared_ptr<IWorldGenerator> CreateTerrainGenerator(const TerrainSettings& settings) {
        return std::make_shared<TerrainGenerator>(settings);
    }

    std::shared_ptr<IWorldGenerator> CreateFlatGenerator(int groundVoxelHeight, uint8_t surfaceId, uint8_t groundId) {
        return std::make_shared<FlatGenerator>(groundVoxelHeight, surfaceId, groundId);
    }
}
