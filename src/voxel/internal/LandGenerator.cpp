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
        constexpr float kMaxTreeHeight = 13.0f;            // metres above ground a tree can reach (a tall spruce or birch)
        constexpr float kDarkStoneDepth = 12.0f;
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

        enum class Feature : uint8_t { None, Broadleaf, Conifer, Shrub, Fern, Flowers, Mushrooms, Boulder, Log, DeadTree, DuneGrass };

        /// The chance of each feature in one 3 m cell of the nature grid, by biome (land.md "Nature"); the density field
        /// scales them (1 in the dense pockets, 0.25 elsewhere).
        struct Odds {
            float broadleaf = 0, conifer = 0, shrub = 0, fern = 0, flowers = 0, mushrooms = 0, boulder = 0, log = 0, deadTree = 0;
        };

        Odds BiomeOdds(LandBiome biome) {
            Odds o;
            switch (biome) {
            // Trees grow on their own 6 m grid (PlaceNature's tree pass); these are the plants and rocks below them.
            case LandBiome::Forest:
                o = {0.0f, 0.0f, 0.15f, 0.15f, 0.0f, 0.03f, 0.03f, 0.03f, 0.0f};
                break;
            case LandBiome::DarkForest:
                o = {0.0f, 0.0f, 0.08f, 0.20f, 0.0f, 0.05f, 0.04f, 0.05f, 0.0f};
                break;
            case LandBiome::Grassland:
                o = {0.0f, 0.0f, 0.03f, 0.0f, 0.18f, 0.0f, 0.01f, 0.0f, 0.0f};
                break;
            case LandBiome::Mountain:
                o = {0.0f, 0.0f, 0.03f, 0.0f, 0.0f, 0.0f, 0.08f, 0.0f, 0.0f};
                break;
            case LandBiome::Desert:
                o = {0.0f, 0.0f, 0.01f, 0.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f};
                break;
            case LandBiome::RockIsle:
                o.boulder = 0.05f;
                break;
            default: break;
            }
            return o;
        }

        void Weights(float t, float w[4]) {
            const float t2 = t * t, t3 = t2 * t;
            w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
            w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
            w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
            w[3] = 0.5f * (t3 - t2);
        }

        /// Catmull-Rom over the 4 x 4 nearest cells, clamped to the 4 nearest (no overshoot at a coast or a bank).
        float CoarseHeight(const LandMap& m, float mx, float mz) {
            const float inv = 1.0f / m.cellSize;
            const float u = mx * inv - 0.5f, v = mz * inv - 0.5f;
            const float u0 = std::floor(u), v0 = std::floor(v);
            const int i = int(u0), j = int(v0);
            auto cell = [&](int a, int b) { return m.Index(std::clamp(a, 0, m.width - 1), std::clamp(b, 0, m.height - 1)); };
            float wu[4], wv[4];
            Weights(u - u0, wu);
            Weights(v - v0, wv);
            float h = 0.0f;
            for (int b = 0; b < 4; ++b) {
                float row = 0.0f;
                for (int a = 0; a < 4; ++a) row += wu[a] * m.heights[cell(i - 1 + a, j - 1 + b)];
                h += wv[b] * row;
            }
            const float h00 = m.heights[cell(i, j)], h10 = m.heights[cell(i + 1, j)];
            const float h01 = m.heights[cell(i, j + 1)], h11 = m.heights[cell(i + 1, j + 1)];
            return std::clamp(h, std::min(std::min(h00, h10), std::min(h01, h11)), std::max(std::max(h00, h10), std::max(h01, h11)));
        }

        /// Bilinear over the fine grid's cell centres; `outside` beyond its cells.
        template <class T>
        float FineBilinear(const LandFine& f, const std::vector<T>& values, float mx, float mz, float outside) {
            const float u = mx - float(f.x0) - 0.5f, v = mz - float(f.z0) - 0.5f;
            const float u0 = std::floor(u), v0 = std::floor(v);
            const int i = int(u0), j = int(v0);
            if (i < 0 || j < 0 || i + 1 >= f.width || j + 1 >= f.height) return outside;
            const float tu = u - u0, tv = v - v0;
            const float a = float(values[f.Index(i, j)]), b = float(values[f.Index(i + 1, j)]);
            const float c = float(values[f.Index(i, j + 1)]), d = float(values[f.Index(i + 1, j + 1)]);
            const float top = a + (b - a) * tu, bottom = c + (d - c) * tu;
            return top + (bottom - top) * tv;
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
                const float mx = x - s.origin.x, mz = z - s.origin.y;
                const float u = mx * m_Inv - 0.5f, v = mz * m_Inv - 0.5f;
                const float u0 = std::floor(u), v0 = std::floor(v);
                const int i = int(u0), j = int(v0);
                const float tu = u - u0, tv = v - v0;
                float h = CoarseHeight(m, mx, mz);
                float detail = 1.0f;
                if (!m.fine.Empty()) {
                    h += FineBilinear(m.fine, m.fine.delta, mx, mz, 0.0f);
                    detail = FineBilinear(m.fine, m.fine.detail, mx, mz, 255.0f) * (1.0f / 255.0f);
                }
                const float a00 = Amplitude(i, j), a10 = Amplitude(i + 1, j), a01 = Amplitude(i, j + 1), a11 = Amplitude(i + 1, j + 1);
                const float atop = a00 + (a10 - a00) * tu, abottom = a01 + (a11 - a01) * tu;
                const float amp = (atop + (abottom - atop) * tv) * detail;
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

            /// Metres from a point to the nearest sea cell's centre within 3 cells (12 m), else 12: smooth along the
            /// coast, unlike the cells' edges.
            float SeaDistance(float x, float z) const {
                const float u = (x - s.origin.x) * m_Inv, v = (z - s.origin.y) * m_Inv;
                const int i = int(std::floor(u)), j = int(std::floor(v));
                float best = 9.0f;
                for (int b = -3; b <= 3; ++b)
                    for (int a = -3; a <= 3; ++a) {
                        if (m.waterKind[Cell(i + a, j + b)] != WaterKind::Sea) continue;
                        const float du = float(i + a) + 0.5f - u, dv = float(j + b) + 0.5f - v;
                        best = std::min(best, du * du + dv * dv);
                    }
                return std::min(12.0f, std::sqrt(best) / m_Inv);
            }

            /// True when one of the 4 nearest cells holds a river or a lake (not the sea).
            bool FreshWater(float x, float z) const {
                const float u = (x - s.origin.x) * m_Inv - 0.5f, v = (z - s.origin.y) * m_Inv - 0.5f;
                const int i = int(std::floor(u)), j = int(std::floor(v));
                for (int b = 0; b < 2; ++b)
                    for (int a = 0; a < 2; ++a) {
                        const WaterKind k = m.waterKind[Cell(i + a, j + b)];
                        if (k == WaterKind::River || k == WaterKind::Lake) return true;
                    }
                return false;
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

            /// The fine grid's surface at world metres (nearest fine cell); Natural outside it.
            LandSurface Surface(float x, float z) const {
                const LandFine& f = m.fine;
                if (f.Empty()) return LandSurface::Natural;
                const int i = int(std::floor(x - s.origin.x)) - f.x0, j = int(std::floor(z - s.origin.y)) - f.z0;
                if (i < 0 || j < 0 || i >= f.width || j >= f.height) return LandSurface::Natural;
                return f.surface[f.Index(i, j)];
            }

            const LandMap& m;
            const LandGeneratorSettings& s;

        private:
            float m_Inv;
        };

        struct LandTile {
            float height[kTileSize * kTileSize];   // ground in voxels
            float water[kChunkSize * kChunkSize];  // water level in voxels, or a very low value
            uint32_t biomeCell[kChunkSize * kChunkSize];
            uint8_t shore[kChunkSize * kChunkSize] = {};   // within kShoreMetres of a wet column: sand
            LandSurface surface[kChunkSize * kChunkSize];   // the fine grid's surface kind
            bool cave = false;                              // the cave's box crosses this column
            bool fort = false;                              // a fort record crosses this column
            float fortLo = 0.0f, fortHi = 0.0f;             // the rows the fort writes here (voxels)
            uint8_t fresh[kChunkSize * kChunkSize] = {};    // water near this column is a river's or a lake's
            uint8_t sea[kChunkSize * kChunkSize] = {};      // SeaDistance in quarter metres (48: 12 m or more)
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
                float dlo = 0.0f, dhi = 0.0f;
                for (float d : m_Map->fine.delta) {
                    dlo = std::min(dlo, d);
                    dhi = std::max(dhi, d);
                }
                m_Bounds.min += dlo;
                m_Bounds.max += dhi;
                if (settings.fort) {
                    m_Fort = PlanFort(*m_Map, settings);
                    for (const FortRecord& r : m_Fort.records) {
                        m_Bounds.min = std::min(m_Bounds.min, float(r.min.y) * settings.voxelSize);
                        m_Bounds.max = std::max(m_Bounds.max, float(r.max.y) * settings.voxelSize);
                    }
                }
                // Cairns beside the roads (land.md "Nature", history): one 2.8 m to the side every 90 m of road, off the
                // water.
                for (const LandRoad& road : m_Map->roads) {
                    for (size_t k = 45; k + 1 < road.x.size(); k += 90) {
                        const float dx = road.x[k + 1] - road.x[k - 1], dz = road.z[k + 1] - road.z[k - 1];
                        const float len = std::sqrt(dx * dx + dz * dz);
                        if (len < 1.0e-3f) continue;
                        const float side = (k / 90) % 2 == 0 ? 2.8f : -2.8f;
                        const float x = road.x[k] + settings.origin.x - dz / len * side, z = road.z[k] + settings.origin.y + dx / len * side;
                        if (m_Sampler.Water(x, z) > kNoLandWater) continue;
                        m_Cairns.push_back(glm::vec3(x, m_Sampler.Height(x, z), z));
                    }
                }
                // Road bridges (land.md "The starter region as built", roads): a deck point per road point over a river
                // and 2 more on each bank, at the bed's height (never under 0.4 m over the water).
                for (const LandRoad& road : m_Map->roads) {
                    if (road.trail || road.bridgeCells == 0) continue;
                    const size_t n = road.x.size();
                    std::vector<uint8_t> over(n, 0);
                    for (size_t k = 0; k < n; ++k) {
                        const float x = road.x[k] + settings.origin.x, z = road.z[k] + settings.origin.y;
                        if (m_Sampler.Water(x, z) <= kNoLandWater) continue;
                        for (size_t j = k >= 2 ? k - 2 : 0; j <= std::min(n - 1, k + 2); ++j) over[j] = 1;
                    }
                    for (size_t k = 0; k < n; ++k) {
                        if (!over[k]) continue;
                        const size_t a = k > 0 ? k - 1 : k, b = std::min(n - 1, k + 1);
                        const float dx = road.x[b] - road.x[a], dz = road.z[b] - road.z[a];
                        const float len = std::sqrt(dx * dx + dz * dz);
                        if (len < 1.0e-3f) continue;
                        const float x = road.x[k] + settings.origin.x, z = road.z[k] + settings.origin.y;
                        const float w = m_Sampler.Water(x, z);
                        const float top = w > kNoLandWater ? std::max(road.bed[k], w + 0.4f) : road.bed[k];
                        m_Bridges.push_back({glm::vec3(x, top, z), glm::vec2(dx, dz) / len, uint32_t(k)});
                    }
                }
                const LandCave& cave = m_Map->cave;
                if (cave.placed) {
                    // The cave in world metres: its box, for the column classes and the chunks it crosses.
                    const float r = std::max(cave.radius, cave.chamberRadius) + settings.voxelSize;
                    for (int k = 0; k < 2; ++k) {
                        m_CaveA[k] = glm::vec3(cave.x[k] + settings.origin.x, cave.y[k], cave.z[k] + settings.origin.y);
                    }
                    m_CaveMin = glm::min(m_CaveA[0], m_CaveA[1]) - glm::vec3(r);
                    m_CaveMax = glm::max(m_CaveA[0], m_CaveA[1]) + glm::vec3(r);
                    m_Bounds.min = std::min(m_Bounds.min, m_CaveMin.y - 2.0f * settings.voxelSize);
                }
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
                const float mudV = 0.4f * m_Inv, gravelV = 0.6f * m_Inv;

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

                            enum class Cover { Grass, DryGrass, Beach, Dune, Snow, Rock, Road, Moss, Litter, Mud, Gravel };
                            Cover cover = Cover::Grass;
                            const LandSurface fine = tile->surface[x + z * kChunkSize];
                            const bool fresh = tile->fresh[x + z * kChunkSize] != 0;
                            const float seaDistance = float(tile->sea[x + z * kChunkSize]) * 0.25f;
                            if (fine == LandSurface::Road) cover = Cover::Road;
                            else if (fine == LandSurface::Site) cover = slope > 1.15f ? Cover::Rock : Cover::Grass;
                            else if (fine == LandSurface::StreamBed) cover = slope > 1.15f ? Cover::Rock : Cover::Beach;
                            else if (underwater) cover = fresh ? Cover::Gravel : Cover::Beach;   // river and lake beds: cobbles
                            else if (seaDistance < 10.0f && slope > 0.7f) cover = Cover::Rock;   // sea cliffs
                            else if (hm < (0.8f + 1.6f * Noise(m_S.seed + 40u, (float(coord.x * kChunkSize + x) + 0.5f) * vs / 18.0f,
                                                              (float(coord.z * kChunkSize + z) + 0.5f) * vs / 18.0f)) *
                                              (1.0f - seaDistance / 12.0f)) {
                                // The beach: its width wanders along the coast, with shingle in patches.
                                const float n = Noise(m_S.seed + 41u, (float(coord.x * kChunkSize + x) + 0.5f) * vs / 9.0f,
                                                      (float(coord.z * kChunkSize + z) + 0.5f) * vs / 9.0f);
                                cover = n > 0.68f ? Cover::Gravel : Cover::Beach;
                            }
                            else if (shore) cover = fresh ? Cover::Mud : Cover::Beach;           // mud flats at fresh water
                            else if (slope > 1.15f || (map.hardness[cell] >= 9 && slope > 0.6f)) cover = Cover::Rock;
                            else if (map.hardness[cell] >= 9 && slope > 0.35f) cover = Cover::Gravel;   // scree below hard rock
                            else {
                                const float wx = (float(coord.x * kChunkSize + x) + 0.5f) * vs, wz = (float(coord.z * kChunkSize + z) + 0.5f) * vs;
                                switch (LandBiome(biome)) {
                                case LandBiome::Desert: cover = slope > 0.8f ? Cover::Rock : Cover::Dune; break;
                                case LandBiome::Mountain:
                                    cover = hm > 92.0f ? Cover::Snow : (slope > 0.55f || hm > 45.0f ? Cover::Rock : Cover::DryGrass);
                                    break;
                                case LandBiome::RockIsle: cover = Cover::Rock; break;
                                case LandBiome::Sea:
                                case LandBiome::Lake: cover = Cover::Beach; break;
                                case LandBiome::Forest:
                                case LandBiome::DarkForest: {
                                    // The forest floor: leaf litter, moss and grass in patches of a few metres.
                                    const float n = Noise(m_S.seed + 10u, wx / 7.0f, wz / 7.0f);
                                    cover = n < 0.24f ? Cover::Litter : (n > 0.7f ? Cover::Moss : Cover::Grass);
                                    break;
                                }
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
                                    case Cover::Road: id = depth <= dirtV ? mat.dirt : mat.stone; break;
                                    case Cover::Moss: id = depth <= grassV ? (mat.moss ? mat.moss : mat.grass) : (depth <= dirtV ? mat.dirt : mat.stone); break;
                                    case Cover::Litter: id = depth <= grassV ? (mat.litter ? mat.litter : mat.dirt) : (depth <= dirtV ? mat.dirt : mat.stone); break;
                                    case Cover::Mud: id = depth <= mudV ? (mat.mud ? mat.mud : mat.dirt) : (depth <= dirtV ? mat.dirt : mat.stone); break;
                                    case Cover::Gravel: id = depth <= gravelV ? (mat.gravel ? mat.gravel : mat.stone) : mat.stone; break;
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
                if (tile->cave) CarveCave(coord, chunk);
                if (m_S.trees && float(y1) > tile->minHeight - 4.0f * m_Inv && float(y0) < tile->maxHeight + kMaxTreeHeight * m_Inv) {
                    PlaceNature(coord, *tile, chunk);
                }
                // Hollowkeep last, so its ditch and walls replace the plants and rocks they cross.
                if (tile->fort) {
                    float ground[kChunkSize * kChunkSize];
                    for (int z = 0; z < kChunkSize; ++z)
                        for (int x = 0; x < kChunkSize; ++x) ground[x + z * kChunkSize] = tile->At(x, z);
                    StampFort(m_Fort, mat, vs, coord, ground, chunk);
                }
                return {ChunkKind::Dense, 0};
            }

        private:
            /// True when (x, z) world metres lies within `margin` of a fort record's footprint.
            bool NearFort(float x, float z, float margin) const {
                if (!m_Fort.placed) return false;
                const float vx = x * m_Inv, vz = z * m_Inv, mv = margin * m_Inv;
                for (const FortRecord& r : m_Fort.records)
                    if (vx >= float(r.min.x) - mv && vx < float(r.max.x) + mv && vz >= float(r.min.z) - mv && vz < float(r.max.z) + mv) return true;
                return false;
            }

            ColumnClass Classify(const LandTile& tile) const {
                ColumnClass c;
                c.known = true;
                float top = std::max(tile.maxHeight, tile.maxWater) + (m_S.trees ? kMaxTreeHeight * m_Inv : 0.0f) + 1.0f;
                if (tile.fort) top = std::max(top, tile.fortHi + 1.0f);
                const auto isEmpty = [top](int row) { return float(row * kChunkSize) > top; };
                const float bottom = std::min(tile.minHeight, tile.fort ? tile.fortLo : tile.minHeight) - kDarkStoneDepth * m_Inv - 2.0f;
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
                const int lowestSurfaceVoxel = int(std::floor(std::min(tile.minHeight, tile.fort ? tile.fortLo : tile.minHeight))) - 1;
                c.surfaceFrom = std::max(c.solidBelow, FloorDiv(lowestSurfaceVoxel, kChunkSize) - 1);
                if (tile.cave) {
                    // The cave goes below the soil layers, and the sky sees into it through its mouth.
                    c.solidBelow = std::min(c.solidBelow, FloorDiv(int(std::floor(m_CaveMin.y * m_Inv)), kChunkSize) - 1);
                    c.surfaceFrom = c.solidBelow;
                }
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
                        tile->surface[x + z * kChunkSize] = m_Sampler.Surface(wx, wz);
                        if (w > kNoLandWater) tile->fresh[x + z * kChunkSize] = m_Sampler.FreshWater(wx, wz) ? 1 : 0;
                        tile->sea[x + z * kChunkSize] = uint8_t(std::floor(m_Sampler.SeaDistance(wx, wz) * 4.0f));
                    }
                }
                MarkShore(cx, cz, *tile);
                if (m_Map->cave.placed) {
                    const float x0 = float(cx * kChunkSize) * vs, z0 = float(cz * kChunkSize) * vs, size = float(kChunkSize) * vs;
                    tile->cave = x0 < m_CaveMax.x && x0 + size > m_CaveMin.x && z0 < m_CaveMax.z && z0 + size > m_CaveMin.z;
                }
                tile->minHeight = mn;
                tile->maxHeight = mx;
                tile->maxWater = mw;
                int fortLo = 0, fortHi = 0;
                if (m_Fort.placed && FortRows(m_Fort, cx * kChunkSize, cz * kChunkSize, (cx + 1) * kChunkSize, (cz + 1) * kChunkSize, fortLo, fortHi)) {
                    tile->fort = true;
                    tile->fortLo = float(fortLo);
                    tile->fortHi = float(fortHi);
                }
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

            /// The cave: a tunnel round the axis from the mouth to the chamber, with a floor at 0.55 of its radius below the
            /// axis, and a chamber at its end with a floor at half its radius. Water stays.
            void CarveCave(const glm::ivec3& coord, Chunk& chunk) const {
                const LandCave& cave = m_Map->cave;
                const float vs = m_S.voxelSize;
                const glm::ivec3 origin = coord * kChunkSize;
                const glm::vec3 lo = glm::vec3(origin) * vs, hi = glm::vec3(origin + glm::ivec3(kChunkSize)) * vs;
                if (lo.x > m_CaveMax.x || lo.y > m_CaveMax.y || lo.z > m_CaveMax.z || hi.x < m_CaveMin.x || hi.y < m_CaveMin.y ||
                    hi.z < m_CaveMin.z) {
                    return;
                }
                const glm::vec3 a = m_CaveA[0], ab = m_CaveA[1] - m_CaveA[0];
                const float abLen2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
                const float r2 = cave.radius * cave.radius, c2 = cave.chamberRadius * cave.chamberRadius;
                for (int z = 0; z < kChunkSize; ++z)
                    for (int y = 0; y < kChunkSize; ++y)
                        for (int x = 0; x < kChunkSize; ++x) {
                            uint8_t& id = chunk.ids[Chunk::VoxelIndex(x, y, z)];
                            if (id == 0 || id == m_S.materials.water) continue;
                            const glm::vec3 p = (glm::vec3(origin + glm::ivec3(x, y, z)) + glm::vec3(0.5f)) * vs;
                            const glm::vec3 ap = p - a;
                            const float t = std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / abLen2, 0.0f, 1.0f);
                            const glm::vec3 axis = a + ab * t, d = p - axis;
                            const bool tunnel = d.x * d.x + d.y * d.y + d.z * d.z < r2 && p.y >= axis.y - 0.55f * cave.radius;
                            const glm::vec3 e = p - m_CaveA[1];
                            const bool chamber = e.x * e.x + e.y * e.y + e.z * e.z < c2 && p.y >= m_CaveA[1].y - 0.5f * cave.chamberRadius;
                            if (tunnel || chamber) id = 0;
                        }
            }

            /// Nature (land.md "Nature"): one feature at most per cell of a jittered 3 m grid, chosen by the biome's odds
            /// and a slow density field (about 30 % of the ground holds the dense pockets, the rest stays open); none on
            /// the fine grid's roads and site, none in or beside water but reeds. Foliage (leaves, needles, ferns,
            /// flowers, reeds, mushrooms) has no collision: shrubs and reeds stay under 1.2 m, canopies start at 2.4 m.
            /// Wood, deadwood and boulders are solid. A chunk draws the part of each nearby feature inside it.
            void PlaceNature(const glm::ivec3& coord, const LandTile& tile, Chunk& chunk) const {
                const WorldMaterials& mat = m_S.materials;
                const LandMap& map = *m_Map;
                const float vs = m_S.voxelSize;
                const glm::ivec3 origin = coord * kChunkSize;
                const float x0 = float(origin.x) * vs, x1 = x0 + kChunkSize * vs;
                const float z0 = float(origin.z) * vs, z1 = z0 + kChunkSize * vs;
                const float y0 = float(origin.y) * vs, y1 = y0 + kChunkSize * vs;
                constexpr float kCell = 3.0f, kReach = 4.5f;
                const int i0 = int(std::floor((x0 - kReach) / kCell)), i1 = int(std::floor((x1 + kReach) / kCell));
                const int j0 = int(std::floor((z0 - kReach) / kCell)), j1 = int(std::floor((z1 + kReach) / kCell));
                const uint8_t water = mat.water;
                auto id = [](uint8_t v, uint8_t fallback) { return v != 0 ? v : fallback; };

                // Writes one voxel (world voxel coordinates) if it lies in this chunk. Soft voxels only fill air (reeds
                // also water); solid ones replace anything.
                auto put = [&](const glm::ivec3& w, uint8_t v, bool soft, bool intoWater = false) {
                    const glm::ivec3 l = w - origin;
                    if (l.x < 0 || l.y < 0 || l.z < 0 || l.x >= kChunkSize || l.y >= kChunkSize || l.z >= kChunkSize) return;
                    uint8_t& at = chunk.ids[Chunk::VoxelIndex(l.x, l.y, l.z)];
                    if (!soft || at == 0 || (intoWater && at == water)) at = v;
                };
                // The ground voxel row of a column inside this chunk (the tile's), else -1e9.
                auto groundRow = [&](int wx, int wz) {
                    const int lx = wx - origin.x, lz = wz - origin.z;
                    if (lx < 0 || lz < 0 || lx >= kChunkSize || lz >= kChunkSize) return INT32_MIN / 2;
                    return int(std::floor(tile.At(lx, lz)));
                };
                // An ellipsoid of voxels (centre and radii in voxels), with hashed ragged edges. Its voxels join its centre
                // voxel by faces: one is kept only when its neighbour a step toward the centre (along its largest scaled
                // offset) is kept, so a blast that frees the blob never leaves a loose voxel of it in the air. The rule
                // reads world coordinates only, so every chunk keeps the same voxels; `keep` caches it per blob.
                std::vector<uint8_t> keep;
                auto blob = [&](const glm::vec3& c, const glm::vec3& r, float ragged, auto&& pick) {
                    const glm::ivec3 lo = glm::max(glm::ivec3(glm::floor(c - r)), origin);
                    const glm::ivec3 hi = glm::min(glm::ivec3(glm::ceil(c + r)) + 1, origin + glm::ivec3(kChunkSize));
                    if (lo.x >= hi.x || lo.y >= hi.y || lo.z >= hi.z) return;
                    const glm::ivec3 centre(glm::floor(c));
                    const glm::ivec3 ext = glm::ivec3(glm::ceil(r)) + 2;
                    // A walk only moves toward the centre: the cache spans this chunk's part and the centre.
                    const glm::ivec3 mlo = glm::max(glm::min(lo, centre), centre - ext), mhi = glm::min(glm::max(hi - 1, centre), centre + ext);
                    const glm::ivec3 size = mhi - mlo + 1;
                    keep.assign(size_t(size.x) * size_t(size.y) * size_t(size.z), 0);   // 0 unknown, 1 kept, 2 not
                    const auto slot = [&](const glm::ivec3& w) {
                        const glm::ivec3 o = w - mlo;
                        return size_t(o.x) + size_t(size.x) * (size_t(o.y) + size_t(size.y) * size_t(o.z));
                    };
                    const auto inside = [&](const glm::ivec3& w) {
                        if (w == centre) return true;
                        const glm::vec3 d = (glm::vec3(w) + 0.5f - c) / r;
                        const float n = HashToFloat(Hash3(w.x, w.y, w.z, m_S.seed));
                        return d.x * d.x + d.y * d.y + d.z * d.z + ragged * n * n <= 1.0f;
                    };
                    const auto parent = [&](const glm::ivec3& w) {
                        const glm::ivec3 o = w - centre;
                        const glm::vec3 a = glm::abs(glm::vec3(o)) / r;
                        const int axis = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
                        glm::ivec3 p = w;
                        p[axis] -= o[axis] > 0 ? 1 : -1;
                        return p;
                    };
                    glm::ivec3 chain[96];
                    for (int z = lo.z; z < hi.z; ++z)
                        for (int y = lo.y; y < hi.y; ++y)
                            for (int x = lo.x; x < hi.x; ++x) {
                                const glm::ivec3 w(x, y, z);
                                const glm::ivec3 o = glm::abs(w - centre);
                                if (o.x > ext.x || o.y > ext.y || o.z > ext.z) continue;
                                // Walk toward the centre to the first voxel already known, then settle the walk back.
                                int n = 0;
                                glm::ivec3 q = w;
                                uint8_t state = 0;
                                while (n < 96) {
                                    state = keep[slot(q)];
                                    if (state != 0) break;
                                    if (!inside(q)) {
                                        state = 2;
                                        keep[slot(q)] = 2;
                                        break;
                                    }
                                    chain[n++] = q;
                                    if (q == centre) {
                                        state = 1;
                                        break;
                                    }
                                    q = parent(q);
                                }
                                for (int k = 0; k < n; ++k) keep[slot(chain[k])] = state;
                                if (state == 1) pick(w, (glm::vec3(w) + 0.5f - c) / r);
                            }
                };
                auto column = [&](int wx, int wz, int from, int to, uint8_t v, bool soft, bool intoWater = false) {
                    for (int y = std::max(from, origin.y); y < std::min(to, origin.y + kChunkSize); ++y) put(glm::ivec3(wx, y, wz), v, soft, intoWater);
                };
                // Ground-hugging patches (ferns, flowers, reeds, mushrooms) read the tile's ground per column.
                auto patch = [&](float cx, float cz, float radius, uint32_t salt, auto&& grow) {
                    const int r = int(std::ceil(radius * m_Inv));
                    const int vx = int(std::floor(cx * m_Inv)), vz = int(std::floor(cz * m_Inv));
                    for (int dz = -r; dz <= r; ++dz)
                        for (int dx = -r; dx <= r; ++dx) {
                            if (dx * dx + dz * dz > r * r) continue;
                            const int wx = vx + dx, wz = vz + dz;
                            const int g = groundRow(wx, wz);
                            if (g == INT32_MIN / 2) continue;
                            grow(wx, wz, g, HashToFloat(Hash3(wx, wz, int(salt), m_S.seed)));
                        }
                };

                // Trees (land.md "Nature", "Trees"): one candidate per cell of a jittered 6 m grid. A parametric skeleton
                // (after Weber and Penn 1995, Veloren's tree generator): tapered wood segments in two levels and ragged
                // leaf clumps at the branch tips; species in patches; on slopes the trunk is sunk to the lowest ground
                // under it, leans downhill and curves back up, and roots run down the slope. Directions turn by the golden
                // angle as fixed (cos, sin) constants: no libm function decides a voxel.
                {
                    constexpr float kTreeCell = 6.0f, kTreeReach = 8.0f;
                    constexpr float kGoldenCos = -0.73736888f, kGoldenSin = 0.67559021f;
                    const int ti0 = int(std::floor((x0 - kTreeReach) / kTreeCell)), ti1 = int(std::floor((x1 + kTreeReach) / kTreeCell));
                    const int tj0 = int(std::floor((z0 - kTreeReach) / kTreeCell)), tj1 = int(std::floor((z1 + kTreeReach) / kTreeCell));
                    const uint8_t barkPale = id(mat.barkPale, mat.planks);
                    const uint8_t dead = id(mat.deadwood, mat.wood);
                    const uint8_t needles = id(mat.needles, mat.leaves);
                    // A tapered wood segment (voxel units), clipped to this chunk.
                    auto segment = [&](const glm::vec3& a, const glm::vec3& b, float ra, float rb, uint8_t v) {
                        const float rmax = std::max(std::max(ra, rb), 0.9f) + 0.5f;
                        const glm::ivec3 lo = glm::max(glm::ivec3(glm::floor(glm::min(a, b) - rmax)), origin);
                        const glm::ivec3 hi = glm::min(glm::ivec3(glm::ceil(glm::max(a, b) + rmax)) + 1, origin + glm::ivec3(kChunkSize));
                        if (lo.x >= hi.x || lo.y >= hi.y || lo.z >= hi.z) return;
                        const glm::vec3 ab = b - a;
                        const float len2 = std::max(1.0e-6f, ab.x * ab.x + ab.y * ab.y + ab.z * ab.z);
                        for (int z = lo.z; z < hi.z; ++z)
                            for (int y = lo.y; y < hi.y; ++y)
                                for (int x = lo.x; x < hi.x; ++x) {
                                    const glm::vec3 p = glm::vec3(float(x), float(y), float(z)) + 0.5f;
                                    const glm::vec3 ap = p - a;
                                    const float t = std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2, 0.0f, 1.0f);
                                    const glm::vec3 d = p - (a + ab * t);
                                    // At least 0.9 voxel: a line in any direction then joins by faces, and the voxel
                                    // holding an end point is always filled (a thinner trunk on a voxel corner had none).
                                    const float r = std::max(0.9f, ra + (rb - ra) * t);
                                    if (d.x * d.x + d.y * d.y + d.z * d.z <= r * r) put(glm::ivec3(x, y, z), v, false);
                                }
                    };
                    // A ragged leaf clump (voxel units), its foot kept at 2.4 m above the tree's ground or higher; a clump
                    // lifted off its branch hangs on a twig from it.
                    auto clump = [&](glm::vec3 c, float r, float flat, float groundVoxel, uint8_t v) {
                        const glm::vec3 on = c;
                        c.y = std::max(c.y, groundVoxel + 2.4f * m_Inv + r * flat);
                        if (c.y > on.y) segment(on, c, 0.0f, 0.0f, mat.wood);
                        blob(c, glm::vec3(r, r * flat, r), 1.2f, [&](glm::ivec3 w, glm::vec3) { put(w, v, true); });
                    };
                    auto unit = [](glm::vec3 v) {
                        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                        return l > 1.0e-6f ? v / l : glm::vec3(0.0f, 1.0f, 0.0f);
                    };
                    auto turn = [](glm::vec2 u, float c, float s) { return glm::vec2(u.x * c - u.y * s, u.x * s + u.y * c); };
                    for (int j = tj0; j <= tj1; ++j)
                        for (int i = ti0; i <= ti1; ++i) {
                            const uint32_t h = Hash3(i, j, 0x7472, m_S.seed);
                            const float tx = (float(i) + 0.15f + 0.7f * HashToFloat(Hash32(h + 1u))) * kTreeCell;
                            const float tz = (float(j) + 0.15f + 0.7f * HashToFloat(Hash32(h + 2u))) * kTreeCell;
                            const uint32_t cell = m_Sampler.NearestCell(tx, tz);
                            const LandBiome biome = LandBiome(map.biomes[cell]);
                            float chance = 0.0f;
                            switch (biome) {
                            case LandBiome::Forest: chance = 0.8f; break;
                            case LandBiome::DarkForest: chance = 0.85f; break;
                            case LandBiome::Grassland: chance = 0.05f; break;
                            case LandBiome::Mountain: chance = 0.2f; break;
                            case LandBiome::Desert: chance = 0.01f; break;
                            default: break;
                            }
                            const float density = Noise(m_S.seed + 20u, tx / 40.0f, tz / 40.0f) > 0.55f ? 1.0f : 0.3f;
                            if (HashToFloat(h) >= chance * density) continue;
                            // The trunk, its roots and its canopy keep clear of roads, trails, the castle site and water:
                            // 8 directions at 1.5, 3 and 4.5 m.
                            bool clear = m_Sampler.Surface(tx, tz) == LandSurface::Natural;
                            for (int k = 0; k < 24 && clear; ++k) {
                                static constexpr float kAround[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.70710678f, 0.70710678f},
                                                                       {-0.70710678f, 0.70710678f}, {0.70710678f, -0.70710678f},
                                                                       {-0.70710678f, -0.70710678f}};
                                const float r = 1.5f * float(1 + k / 8);
                                const float ax = tx + kAround[k % 8][0] * r, az = tz + kAround[k % 8][1] * r;
                                clear = m_Sampler.Surface(ax, az) == LandSurface::Natural && m_Sampler.Water(ax, az) <= kNoLandWater;
                            }
                            // Nor inside or within 4 m of what Hollowkeep stamps (its canopy would hang over the cut).
                            if (!clear || NearFort(tx, tz, 4.0f)) continue;
                            // The trunk's foot: the lowest ground under it, 0.3 m into it; the slope and its downhill side.
                            const float gc = m_Sampler.Height(tx, tz);
                            const float ge = m_Sampler.Height(tx + 0.5f, tz), gw = m_Sampler.Height(tx - 0.5f, tz);
                            const float gs = m_Sampler.Height(tx, tz + 0.5f), gn = m_Sampler.Height(tx, tz - 0.5f);
                            if (gc < 1.2f) continue;
                            const glm::vec2 grad((ge - gw), (gs - gn));
                            const float slope = std::sqrt(grad.x * grad.x + grad.y * grad.y);
                            if (slope > 0.9f) continue;
                            const float footM = std::min(std::min(gc, ge), std::min(std::min(gw, gs), gn)) - 0.3f;
                            if (biome == LandBiome::Mountain && gc > 70.0f) continue;
                            // Species in patches.
                            const float patchN = Noise(m_S.seed + 30u, tx / 35.0f, tz / 35.0f);
                            enum class Species { Oak, Birch, Spruce, Pine, Snag } sp = Species::Oak;
                            switch (biome) {
                            case LandBiome::Forest: sp = patchN < 0.45f ? Species::Oak : (patchN < 0.75f ? Species::Birch : Species::Spruce); break;
                            case LandBiome::DarkForest: sp = patchN < 0.15f ? Species::Birch : (patchN < 0.7f ? Species::Spruce : Species::Pine); break;
                            case LandBiome::Grassland: sp = patchN < 0.6f ? Species::Oak : Species::Birch; break;
                            case LandBiome::Mountain: sp = gc > 45.0f || patchN > 0.5f ? Species::Pine : Species::Spruce; break;
                            default: sp = Species::Snag; break;
                            }
                            const float r3 = HashToFloat(Hash32(h + 3u)), r4 = HashToFloat(Hash32(h + 4u));
                            if (HashToFloat(Hash32(h + 5u)) < (biome == LandBiome::Mountain ? 0.1f : 0.05f)) sp = Species::Snag;
                            // Age: smaller trees in the sparse pockets and high on the mountain.
                            float scale = (0.75f + 0.25f * Noise(m_S.seed + 31u, tx / 60.0f, tz / 60.0f)) * (density < 1.0f ? 0.85f : 1.0f);
                            if (biome == LandBiome::Mountain && gc > 45.0f) scale *= 0.7f;
                            const float top = footM + 13.0f * scale;
                            if (top < y0 - 0.5f || footM - 3.0f > y1) continue;

                            const float vy = footM * m_Inv;
                            const glm::vec3 foot(tx * m_Inv, vy, tz * m_Inv);
                            const glm::vec3 up(0.0f, 1.0f, 0.0f);
                            static constexpr float kDir[8][2] = {{1, 0}, {0.7071f, 0.7071f}, {0, 1}, {-0.7071f, 0.7071f},
                                                                 {-1, 0}, {-0.7071f, -0.7071f}, {0, -1}, {0.7071f, -0.7071f}};
                            glm::vec2 az(kDir[Hash32(h + 6u) & 7u][0], kDir[Hash32(h + 6u) & 7u][1]);
                            auto jitter = [&](uint32_t salt, float amount) {
                                return glm::vec3(HashToFloat(Hash32(salt)) - 0.5f, HashToFloat(Hash32(salt + 1u)) - 0.5f, HashToFloat(Hash32(salt + 2u)) - 0.5f) *
                                       (2.0f * amount);
                            };
                            // Pistol butt: on a slope the lowest 1.2 m leans downhill, then the trunk goes up again.
                            glm::vec3 knee = foot + up * (1.2f * m_Inv);
                            if (slope > 0.15f) {
                                const glm::vec2 down = -grad / slope;
                                const float lean = std::min(slope, 0.9f) * 0.35f * m_Inv;
                                knee += glm::vec3(down.x, 0.0f, down.y) * lean;
                                // Roots down the slope and to its sides, ending in the ground.
                                for (int k = -1; k <= 1; ++k) {
                                    const glm::vec2 d = k == 0 ? down : turn(down, 0.7071f, 0.7071f * float(k));
                                    const float length = 1.2f + 0.8f * HashToFloat(Hash32(h + 60u + uint32_t(k + 1)));
                                    const float ex = tx + d.x * length, ez = tz + d.y * length;
                                    const glm::vec3 end(ex * m_Inv, (m_Sampler.Height(ex, ez) - 0.15f) * m_Inv, ez * m_Inv);
                                    segment(foot + up * (0.4f * m_Inv), end, 0.16f * m_Inv * scale + 0.5f, 0.06f * m_Inv, mat.wood);
                                }
                            }
                            switch (sp) {
                            case Species::Oak: {
                                const float H = (7.0f + 3.5f * r3) * scale, R = (0.22f + 0.1f * r4) * scale;
                                const float split = std::max(2.8f, H * (0.4f + 0.1f * r4));
                                const glm::vec3 fork = glm::vec3(knee.x, vy + split * m_Inv, knee.z) + jitter(h + 7u, 0.25f * m_Inv);
                                segment(foot - up * (0.5f * m_Inv), foot + up * (0.8f * m_Inv), 1.7f * R * m_Inv, R * m_Inv, mat.wood);   // flare
                                segment(foot, knee, R * m_Inv, R * m_Inv, mat.wood);
                                segment(knee, fork, R * m_Inv, 0.85f * R * m_Inv, mat.wood);
                                const int limbs = 3 + int(Hash32(h + 8u) % 3u);
                                for (int b = 0; b < limbs; ++b) {
                                    az = turn(az, kGoldenCos, kGoldenSin);
                                    const uint32_t hb = Hash32(h + 100u + uint32_t(b) * 7u);
                                    const float L = H * 0.42f * (0.7f + 0.3f * HashToFloat(hb)) * m_Inv;
                                    const glm::vec3 dir = unit(glm::vec3(az.x * 0.75f, 0.65f, az.y * 0.75f) + jitter(hb + 1u, 0.2f));
                                    const glm::vec3 mid = fork + dir * (0.5f * L) + jitter(hb + 4u, 0.12f * L);
                                    const glm::vec3 dir2 = unit(dir + up * 0.25f + jitter(hb + 7u, 0.25f));
                                    const glm::vec3 end = mid + dir2 * (0.5f * L);
                                    const float rl = 0.55f * R * m_Inv;
                                    segment(fork, mid, rl, 0.75f * rl, mat.wood);
                                    segment(mid, end, 0.75f * rl, 0.45f * rl, mat.wood);
                                    clump(end, (1.1f + 0.5f * HashToFloat(hb + 10u)) * scale * m_Inv, 0.7f, vy, mat.leaves);
                                    for (int c = 0; c < 2; ++c) {
                                        const glm::vec2 side = turn(az, 0.7071f, c == 0 ? 0.7071f : -0.7071f);
                                        const glm::vec3 cd = unit(glm::vec3(side.x, 0.45f, side.y) + jitter(hb + 20u + uint32_t(c) * 3u, 0.2f));
                                        const glm::vec3 from = fork + (end - fork) * (0.55f + 0.2f * float(c));
                                        const glm::vec3 tip = from + cd * (0.4f * L);
                                        segment(from, tip, 0.35f * rl, 0.2f * rl, mat.wood);
                                        clump(tip, (0.9f + 0.4f * HashToFloat(hb + 30u + uint32_t(c))) * scale * m_Inv, 0.7f, vy,
                                              HashToFloat(hb + 40u + uint32_t(c)) < 0.08f ? mat.leavesAutumn : mat.leaves);
                                    }
                                }
                                const glm::vec3 crown = fork + up * (0.35f * H * m_Inv);
                                segment(fork, crown, 0.4f * R * m_Inv, 0.25f * R * m_Inv, mat.wood);   // the leader to the crown
                                clump(crown, 1.4f * scale * m_Inv, 0.65f, vy, mat.leaves);
                                break;
                            }
                            case Species::Birch: {
                                const float H = (8.0f + 3.0f * r3) * scale, R = (0.12f + 0.04f * r4) * scale;
                                const int stems = r4 > 0.6f ? 2 : 1;
                                for (int st = 0; st < stems; ++st) {
                                    const glm::vec2 off = st == 0 ? glm::vec2(0.0f) : glm::vec2(kDir[Hash32(h + 9u) & 7u][0], kDir[Hash32(h + 9u) & 7u][1]);
                                    const float Hs = H * (st == 0 ? 1.0f : 0.85f);
                                    const glm::vec3 base = foot + glm::vec3(off.x, 0.0f, off.y) * (0.3f * m_Inv);
                                    const glm::vec3 tip = glm::vec3(base.x, vy + Hs * m_Inv, base.z) + glm::vec3(off.x, 0.0f, off.y) * (0.8f * m_Inv) +
                                                          jitter(h + 11u + uint32_t(st), 0.4f * m_Inv);
                                    segment(base - up * (0.4f * m_Inv), tip, R * m_Inv, 0.35f * R * m_Inv, barkPale);
                                    for (int b = 0; b < 7; ++b) {
                                        az = turn(az, kGoldenCos, kGoldenSin);
                                        const float t = 0.45f + 0.5f * float(b) / 7.0f;
                                        const glm::vec3 from = base + (tip - base) * t;
                                        const uint32_t hb = Hash32(h + 200u + uint32_t(st * 16 + b));
                                        const glm::vec3 dir = unit(glm::vec3(az.x * 0.5f, 0.85f, az.y * 0.5f) + jitter(hb, 0.15f));
                                        const glm::vec3 end = from + dir * ((0.7f + 0.7f * HashToFloat(hb + 3u)) * (1.0f - 0.5f * t) * scale * m_Inv);
                                        segment(from, end, 0.3f * R * m_Inv, 0.15f * R * m_Inv, barkPale);
                                        clump(end, (0.5f + 0.3f * HashToFloat(hb + 4u)) * scale * m_Inv, 0.8f, vy, mat.leaves);
                                    }
                                    clump(tip, 0.9f * scale * m_Inv, 0.8f, vy, mat.leaves);
                                }
                                break;
                            }
                            case Species::Spruce: {
                                // Whorls of drooping branches with needle slabs along them; some whorls short or missing.
                                const float H = (9.0f + 3.0f * r3) * scale, R = (0.16f + 0.06f * r4) * scale;
                                const glm::vec3 tip = glm::vec3(knee.x, vy + H * m_Inv, knee.z);
                                segment(foot - up * (0.4f * m_Inv), knee, 1.3f * R * m_Inv, R * m_Inv, mat.wood);
                                segment(knee, tip, R * m_Inv, 0.05f * m_Inv, mat.wood);
                                float y = 2.4f;
                                int whorl = 0;
                                while (y < H - 0.6f) {
                                    const uint32_t hw = Hash32(h + 300u + uint32_t(whorl));
                                    const float t = (y - 2.4f) / std::max(0.1f, H - 2.4f);
                                    if (HashToFloat(hw) >= 0.15f) {
                                        const float L = ((2.2f + 0.6f * r4) * (1.0f - t) * (0.8f + 0.4f * HashToFloat(hw + 1u)) * scale + 0.3f) * m_Inv;
                                        const int n = 4 + int(hw & 1u);
                                        const float c = n == 4 ? 0.0f : 0.30901699f, s = n == 4 ? 1.0f : 0.95105652f;
                                        az = turn(az, kGoldenCos, kGoldenSin);
                                        glm::vec2 u = az;
                                        const glm::vec3 from = knee + (tip - knee) * ((y - 1.2f) / std::max(0.1f, H - 1.2f));
                                        for (int b = 0; b < n; ++b) {
                                            const glm::vec3 end = from + glm::vec3(u.x, -0.3f, u.y) * L + jitter(hw + 10u + uint32_t(b), 0.1f * L);
                                            segment(from, end, 0.06f * m_Inv, 0.03f * m_Inv, mat.wood);
                                            for (int k = 1; k <= 3; ++k) {
                                                const float f = 0.3f * float(k);
                                                clump(from + (end - from) * f, (0.55f - 0.1f * float(k)) * scale * m_Inv + 1.0f, 0.5f, vy, needles);
                                            }
                                            u = turn(u, c, s);
                                        }
                                    }
                                    y += 0.5f + 0.3f * HashToFloat(hw + 2u);
                                    ++whorl;
                                }
                                clump(tip, 0.45f * m_Inv + 1.0f, 1.4f, vy, needles);
                                break;
                            }
                            case Species::Pine: {
                                const float H = (9.0f + 3.0f * r3) * scale, R = (0.2f + 0.06f * r4) * scale;
                                const glm::vec3 bend = glm::vec3(knee.x, vy + 0.55f * H * m_Inv, knee.z) + jitter(h + 12u, 0.35f * m_Inv);
                                const glm::vec3 tip = glm::vec3(bend.x, vy + H * m_Inv, bend.z) + jitter(h + 15u, 0.35f * m_Inv);
                                segment(foot - up * (0.4f * m_Inv), knee, 1.3f * R * m_Inv, R * m_Inv, mat.wood);
                                segment(knee, bend, R * m_Inv, 0.8f * R * m_Inv, mat.wood);
                                segment(bend, tip, 0.8f * R * m_Inv, 0.4f * R * m_Inv, mat.wood);
                                const int branches = 4 + int(Hash32(h + 13u) % 3u);
                                for (int b = 0; b < branches; ++b) {
                                    az = turn(az, kGoldenCos, kGoldenSin);
                                    const uint32_t hb = Hash32(h + 400u + uint32_t(b));
                                    const glm::vec3 from = bend + (tip - bend) * (0.1f + 0.85f * HashToFloat(hb));
                                    const glm::vec3 dir = unit(glm::vec3(az.x * 0.85f, 0.35f, az.y * 0.85f) + jitter(hb + 1u, 0.2f));
                                    const glm::vec3 end = from + dir * ((1.4f + 1.0f * HashToFloat(hb + 4u)) * scale * m_Inv);
                                    segment(from, end, 0.4f * R * m_Inv, 0.2f * R * m_Inv, mat.wood);
                                    clump(end, (1.0f + 0.4f * HashToFloat(hb + 5u)) * scale * m_Inv, 0.45f, vy, needles);
                                }
                                clump(tip, 1.2f * scale * m_Inv, 0.5f, vy, needles);
                                break;
                            }
                            case Species::Snag: {
                                const float H = (4.0f + 3.0f * r3) * scale, R = (0.18f + 0.08f * r4) * scale;
                                const glm::vec3 top = glm::vec3(knee.x, vy + H * m_Inv, knee.z) + jitter(h + 14u, 0.2f * m_Inv);
                                segment(foot - up * (0.4f * m_Inv), knee, 1.2f * R * m_Inv, R * m_Inv, dead);
                                segment(knee, top, R * m_Inv, 0.8f * R * m_Inv, dead);
                                // A jagged break: two splinters above the top.
                                for (int k = 0; k < 2; ++k)
                                    segment(top, top + up * ((0.3f + 0.3f * float(k)) * m_Inv) + jitter(h + 16u + uint32_t(k), 0.15f * m_Inv), 0.4f * R * m_Inv,
                                            0.5f, dead);
                                const int stubs = 1 + int(Hash32(h + 17u) % 3u);
                                for (int b = 0; b < stubs; ++b) {
                                    az = turn(az, kGoldenCos, kGoldenSin);
                                    const glm::vec3 from = knee + (top - knee) * (0.4f + 0.4f * HashToFloat(Hash32(h + 18u + uint32_t(b))));
                                    segment(from, from + glm::vec3(az.x, 0.4f, az.y) * ((0.6f + 0.4f * r4) * m_Inv), 0.4f * R * m_Inv, 0.5f, dead);
                                }
                                break;
                            }
                            }
                        }
                }

                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i) {
                        const uint32_t h = Hash3(i, j, 0x6e61, m_S.seed);
                        const float r1 = HashToFloat(Hash32(h + 1u)), r2 = HashToFloat(Hash32(h + 2u));
                        const float fx = (float(i) + 0.15f + 0.7f * r1) * kCell, fz = (float(j) + 0.15f + 0.7f * r2) * kCell;
                        if (m_Sampler.Surface(fx, fz) != LandSurface::Natural) continue;
                        const uint32_t cell = m_Sampler.NearestCell(fx, fz);
                        const uint8_t biome = map.biomes[cell];
                        const float ground = m_Sampler.Height(fx, fz);
                        const float level = m_Sampler.Water(fx, fz);
                        // Only reeds grow at water: shallow fresh water and its wet bank. In the sea's shallows: rocks.
                        if (level > kNoLandWater && !m_Sampler.FreshWater(fx, fz)) {
                            if (ground < level - 1.5f || HashToFloat(h) >= 0.06f) continue;
                            const float radius = 0.5f + 0.9f * HashToFloat(Hash32(h + 3u));
                            const glm::ivec3 b(int(std::floor(fx * m_Inv)), int(std::floor(ground * m_Inv)), int(std::floor(fz * m_Inv)));
                            blob(glm::vec3(b) + glm::vec3(0.5f), glm::vec3(radius, 0.8f * radius, 0.9f * radius) * m_Inv, 0.3f,
                                 [&](glm::ivec3 w, glm::vec3) { put(w, HashToFloat(Hash32(h + 5u)) < 0.5f ? mat.stone : mat.stoneDark, false); });
                            continue;
                        }
                        if (level > kNoLandWater) {
                            if (ground < level - 0.5f || ground > level + 0.6f) continue;
                            if (HashToFloat(h) >= 0.55f) continue;
                            const float top = std::max(ground, level) + 0.6f + 0.6f * HashToFloat(Hash32(h + 3u));
                            patch(fx, fz, 1.1f, h, [&](int wx, int wz, int g, float n) {
                                if (n >= 0.4f) return;
                                column(wx, wz, g, int(std::floor(top * m_Inv)) - int(n * 4.0f), id(mat.reed, mat.leaves), true, true);
                            });
                            continue;
                        }
                        if (ground < 1.2f) continue;
                        const float density = Noise(m_S.seed + 20u, fx / 40.0f, fz / 40.0f) > 0.55f ? 1.0f : 0.25f;
                        const float slope = std::max(std::abs(m_Sampler.Height(fx + 0.8f, fz) - ground), std::abs(m_Sampler.Height(fx, fz + 0.8f) - ground)) / 0.8f;
                        Odds odds = BiomeOdds(LandBiome(biome));
                        // No soft plants on bare ground (the cover's rule: cliffs, scree below hard rock, mountain rock
                        // above 45 m or steeper than 0.55, snow): rocks only.
                        const bool bare = slope > 1.1f || (map.hardness[cell] >= 9 && slope > 0.3f) ||
                                          (biome == uint8_t(LandBiome::Mountain) && (ground > 43.0f || slope > 0.5f));
                        if (bare) odds = Odds{0, 0, 0, 0, 0, 0, odds.boulder, 0, 0};
                        // The coast: dune grass behind the beach, rocks and driftwood on it.
                        const bool coast = ground < 3.5f && m_Sampler.SeaDistance(fx, fz) < 8.0f;
                        float dune = 0.0f;
                        if (coast) {
                            odds = Odds{0, 0, 0.02f, 0, 0, 0, 0.06f, 0.03f, 0};
                            dune = 0.25f;
                        }
                        float roll = HashToFloat(h);
                        Feature f = Feature::None;
                        for (const auto& [chance, what] : {std::pair{odds.broadleaf, Feature::Broadleaf}, std::pair{odds.conifer, Feature::Conifer},
                                                         std::pair{odds.shrub, Feature::Shrub}, std::pair{odds.fern, Feature::Fern},
                                                         std::pair{odds.flowers, Feature::Flowers}, std::pair{odds.mushrooms, Feature::Mushrooms},
                                                         std::pair{odds.boulder, Feature::Boulder}, std::pair{odds.log, Feature::Log},
                                                         std::pair{odds.deadTree, Feature::DeadTree}}) {
                            const float c = chance * density;
                            if (roll < c) {
                                f = what;
                                break;
                            }
                            roll -= c;
                        }
                        if (f == Feature::None && roll < dune * density) f = Feature::DuneGrass;
                        if (f == Feature::None) continue;
                        const bool tall = f == Feature::Broadleaf || f == Feature::Conifer || f == Feature::DeadTree || f == Feature::Log;
                        if (tall && slope > 0.8f) continue;
                        const float top = ground + (tall ? kMaxTreeHeight : 2.0f);
                        if (top < y0 - 0.5f || ground - 3.0f > y1) continue;   // nothing of it in this chunk's rows

                        const float r3 = HashToFloat(Hash32(h + 3u)), r4 = HashToFloat(Hash32(h + 4u));
                        const float r5 = HashToFloat(Hash32(h + 5u));
                        const glm::ivec3 base(int(std::floor(fx * m_Inv)), int(std::floor(ground * m_Inv)), int(std::floor(fz * m_Inv)));
                        const bool forest = biome == uint8_t(LandBiome::Forest) || biome == uint8_t(LandBiome::DarkForest);
                        switch (f) {
                        case Feature::Shrub: {
                            // 2-4 overlapping ragged lumps, none above 1.1 m.
                            const uint8_t leaf = biome == uint8_t(LandBiome::Desert) || r5 < 0.2f ? mat.leavesAutumn : mat.leaves;
                            const int lumps = 2 + int(Hash32(h + 12u) % 3u);
                            for (int k = 0; k < lumps; ++k) {
                                const uint32_t hk = Hash32(h + 70u + uint32_t(k));
                                const float radius = 0.35f + 0.3f * HashToFloat(hk);
                                const glm::vec3 off((HashToFloat(hk + 1u) - 0.5f) * 1.2f, 0.0f, (HashToFloat(hk + 2u) - 0.5f) * 1.2f);
                                const float tall = std::min(0.5f, 1.05f - radius) * (0.6f + 0.4f * HashToFloat(hk + 3u));
                                blob(glm::vec3(base) + glm::vec3(0.5f) + off * m_Inv + glm::vec3(0.0f, tall * m_Inv, 0.0f),
                                     glm::vec3(radius, 0.8f * radius, radius) * m_Inv, 1.0f, [&](glm::ivec3 w, glm::vec3) { put(w, leaf, true); });
                            }
                            break;
                        }
                        case Feature::DuneGrass:
                            // Tufts of grass on the sand: 1-3 voxels, sparse.
                            patch(fx, fz, 1.8f, h, [&](int wx, int wz, int g, float n) {
                                if (n < 0.3f) column(wx, wz, g, g + 1 + int(n * 8.0f), id(mat.reed, mat.leaves), true);
                            });
                            break;
                        case Feature::Fern:
                            patch(fx, fz, 1.3f, h, [&](int wx, int wz, int g, float n) {
                                if (n < 0.45f) column(wx, wz, g, g + 1 + int(n * 6.0f), id(mat.fern, mat.leaves), true);
                            });
                            break;
                        case Feature::Flowers: {
                            const uint8_t bloom = r5 < 0.5f ? id(mat.flowerYellow, mat.leavesAutumn) : id(mat.flowerViolet, mat.leavesAutumn);
                            patch(fx, fz, 1.6f, h, [&](int wx, int wz, int g, float n) {
                                if (n >= 0.22f) return;
                                put(glm::ivec3(wx, g, wz), id(mat.fern, mat.leaves), true);
                                put(glm::ivec3(wx, g + 1, wz), bloom, true);
                            });
                            break;
                        }
                        case Feature::Mushrooms:
                            patch(fx, fz, 0.8f, h, [&](int wx, int wz, int g, float n) {
                                if (n >= 0.08f) return;
                                put(glm::ivec3(wx, g, wz), id(mat.mushroom, mat.sand), true);
                                put(glm::ivec3(wx, g + 1, wz), id(mat.mushroom, mat.sand), true);
                            });
                            break;
                        case Feature::Boulder: {
                            const float radius = 0.5f + 1.1f * r3 * r3;
                            const uint8_t rock = r5 < 0.5f ? mat.stone : mat.stoneDark;
                            const glm::vec3 c = glm::vec3(base) + glm::vec3(0.5f, 0.5f - 0.3f * radius * m_Inv, 0.5f);
                            blob(c, glm::vec3(radius, 0.75f * radius, 0.9f * radius) * m_Inv, 0.3f, [&](glm::ivec3 w, glm::vec3 d) {
                                put(w, forest && d.y > 0.55f ? id(mat.moss, mat.grass) : rock, false);
                            });
                            break;
                        }
                        case Feature::Log: {
                            // A nurse log: deadwood lying on the ground, moss on its back, fungi and ferns beside it.
                            static constexpr float kDir[8][2] = {{1, 0}, {0.7071f, 0.7071f}, {0, 1}, {-0.7071f, 0.7071f},
                                                                 {-1, 0}, {-0.7071f, -0.7071f}, {0, -1}, {0.7071f, -0.7071f}};
                            const float* dir = kDir[Hash32(h + 7u) & 7u];
                            const float half = (2.0f + 1.5f * r3) * m_Inv, radius = (0.3f + 0.2f * r4) * m_Inv;
                            const glm::vec3 c = glm::vec3(base) + glm::vec3(0.5f, radius - 0.1f * m_Inv, 0.5f);
                            const glm::vec3 a = c - glm::vec3(dir[0], 0.0f, dir[1]) * half, ab = glm::vec3(dir[0], 0.0f, dir[1]) * (2.0f * half);
                            const glm::ivec3 lo = glm::max(glm::ivec3(glm::floor(glm::min(a, a + ab) - radius)), origin);
                            const glm::ivec3 hi = glm::min(glm::ivec3(glm::ceil(glm::max(a, a + ab) + radius)) + 1, origin + glm::ivec3(kChunkSize));
                            const float len2 = ab.x * ab.x + ab.z * ab.z;
                            for (int z = lo.z; z < hi.z; ++z)
                                for (int y = lo.y; y < hi.y; ++y)
                                    for (int x = lo.x; x < hi.x; ++x) {
                                        const glm::vec3 p = glm::vec3(float(x), float(y), float(z)) + 0.5f;
                                        const glm::vec3 ap = p - a;
                                        const float t = std::clamp((ap.x * ab.x + ap.z * ab.z) / len2, 0.0f, 1.0f);
                                        const glm::vec3 d = p - (a + ab * t);
                                        if (d.x * d.x + d.y * d.y + d.z * d.z > radius * radius) continue;
                                        put(glm::ivec3(x, y, z), !coast && d.y > 0.45f * radius ? id(mat.moss, mat.grass) : id(mat.deadwood, mat.wood), false);
                                    }
                            const float sx = fx + dir[1] * 1.2f, sz = fz - dir[0] * 1.2f;
                            if (!coast) patch(sx, sz, 1.0f, h + 9u, [&](int wx, int wz, int g, float n) {
                                if (n < 0.1f) {
                                    put(glm::ivec3(wx, g, wz), id(mat.mushroom, mat.sand), true);
                                } else if (n < 0.4f) {
                                    column(wx, wz, g, g + 1 + int(n * 5.0f), id(mat.fern, mat.leaves), true);
                                }
                            });
                            break;
                        }
                        default: break;
                        }
                    }

                // Cairns beside the roads: stacked stones, the widest at the foot.
                for (const glm::vec3& c : m_Cairns) {
                    if (c.x < x0 - 1.5f || c.x > x1 + 1.5f || c.z < z0 - 1.5f || c.z > z1 + 1.5f || c.y - 1.0f > y1 || c.y + 2.0f < y0) continue;
                    const glm::ivec3 base(int(std::floor(c.x * m_Inv)), int(std::floor(c.y * m_Inv)), int(std::floor(c.z * m_Inv)));
                    float y = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        const float radius = (0.55f - 0.1f * float(k)) * m_Inv;
                        const float tall = (0.35f - 0.03f * float(k)) * m_Inv;
                        blob(glm::vec3(base) + glm::vec3(0.5f, y + 0.5f * tall, 0.5f), glm::vec3(radius, 0.5f * tall + 0.5f, radius), 0.2f,
                             [&](glm::ivec3 w, glm::vec3) { put(w, k % 2 == 0 ? mat.stone : mat.stoneDark, false); });
                        y += tall;
                    }
                }

                // Road bridges: a 3.4 m wide plank deck 0.3 m thick, posts and a rail along both sides, piles down to the
                // river bed.
                for (const BridgeDeck& d : m_Bridges) {
                    if (d.p.x < x0 - 2.5f || d.p.x > x1 + 2.5f || d.p.z < z0 - 2.5f || d.p.z > z1 + 2.5f || d.p.y - 8.0f > y1 ||
                        d.p.y + 1.5f < y0)
                        continue;
                    const int top = int(std::floor(d.p.y * m_Inv));
                    const int thick = std::max(1, int(0.3f * m_Inv + 0.5f));
                    const int r = int(std::ceil(1.8f * m_Inv));
                    const glm::ivec2 c(int(std::floor(d.p.x * m_Inv)), int(std::floor(d.p.z * m_Inv)));
                    const glm::vec2 side(-d.dir.y, d.dir.x);
                    for (int dz = -r; dz <= r; ++dz)
                        for (int dx = -r; dx <= r; ++dx) {
                            const float across = std::abs(float(dx) * side.x + float(dz) * side.y) * m_S.voxelSize;
                            const float along = std::abs(float(dx) * d.dir.x + float(dz) * d.dir.y) * m_S.voxelSize;
                            if (across > 1.7f || along > 0.75f) continue;
                            for (int y = top - thick; y < top; ++y) put(glm::ivec3(c.x + dx, y, c.y + dz), mat.planks, false, true);
                            if (across > 1.55f) {
                                // The rail, and a post every 2 m.
                                const int railTop = top + int(0.9f * m_Inv);
                                put(glm::ivec3(c.x + dx, railTop, c.y + dz), mat.wood, false);
                                if (d.index % 2 == 0 && along < 0.15f)
                                    for (int y = top; y < railTop; ++y) put(glm::ivec3(c.x + dx, y, c.y + dz), mat.wood, false);
                            }
                        }
                    if (d.index % 4 == 0)
                        for (const float s : {-1.2f, 1.2f}) {
                            const float px = d.p.x + side.x * s, pz = d.p.z + side.y * s;
                            const int pile = int(std::floor(m_Sampler.Height(px, pz) * m_Inv)) - 2;
                            const glm::ivec2 pc(int(std::floor(px * m_Inv)), int(std::floor(pz * m_Inv)));
                            for (int y = pile; y < top - thick; ++y)
                                for (int q = 0; q < 4; ++q) put(glm::ivec3(pc.x + (q & 1), y, pc.y + (q >> 1)), mat.wood, false, true);
                        }
                }
            }

            std::shared_ptr<const LandMap> m_Map;
            LandGeneratorSettings m_S;
            Sampler m_Sampler;
            float m_Inv;
            HeightBounds m_Bounds;
            FortPlan m_Fort;
            glm::vec3 m_CaveA[2]{}, m_CaveMin{0.0f}, m_CaveMax{0.0f};
            std::vector<glm::vec3> m_Cairns;              // world metres, the ground under each
            struct BridgeDeck {
                glm::vec3 p;                              // world metres: the deck's top over a road point
                glm::vec2 dir;                            // the road's direction there
                uint32_t index;                           // the road point's index (posts every 2, piles every 4)
            };
            std::vector<BridgeDeck> m_Bridges;
            mutable std::mutex m_CacheMutex;
            mutable std::unordered_map<uint64_t, std::shared_ptr<const LandTile>> m_Cache;
        };
    }

    glm::vec2 CenteredLandOrigin(const LandMap& map) {
        return glm::vec2(-0.5f * float(map.width) * map.cellSize, -0.5f * float(map.height) * map.cellSize);
    }

    glm::vec2 LandWorldOrigin(const LandMap& map) {
        for (const LandPoint& p : map.points)
            if (p.kind == LandPointKind::Village) return glm::vec2(-p.x, -p.z);
        return CenteredLandOrigin(map);
    }

    std::shared_ptr<IWorldGenerator> CreateLandGenerator(std::shared_ptr<const LandMap> map, const LandGeneratorSettings& settings) {
        return std::make_shared<LandGenerator>(std::move(map), settings);
    }

    float LandCoarseHeight(const LandMap& map, float mx, float mz) { return CoarseHeight(map, mx, mz); }

    float LandFineDelta(const LandMap& map, float mx, float mz) {
        return map.fine.Empty() ? 0.0f : FineBilinear(map.fine, map.fine.delta, mx, mz, 0.0f);
    }

    float LandHeight(const LandMap& map, const LandGeneratorSettings& settings, float x, float z) {
        return Sampler(map, settings).Height(x, z);
    }

    float LandWaterLevel(const LandMap& map, const LandGeneratorSettings& settings, float x, float z) {
        return Sampler(map, settings).Water(x, z);
    }

    glm::vec2 LandSpawn(const LandMap& m, const LandGeneratorSettings& settings, int region) {
        if (region == 0) {
            // The starter region's village, when the bake placed one.
            for (const LandPoint& p : m.points)
                if (p.kind == LandPointKind::Village) return settings.origin + glm::vec2(p.x, p.z);
            region = m.starterRegion;
        }
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
