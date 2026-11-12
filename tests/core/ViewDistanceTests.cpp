// View distance stage 1 (docs/design/view-distance.md): full-height window, column pass,
// Packed cells, column-top skip.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    TerrainSettings GameTerrain() {
        TerrainSettings s;
        s.voxelSize = 0.1f;
        return s;
    }
}

// Not a gate: measures how many chunks per second the built-in generator produces on this
// machine, by kind, with the worker count the game uses. Run with
//   VortexCoreTests --gtest_also_run_disabled_tests --gtest_filter='*GenerationThroughput*'
TEST(ViewDistance, DISABLED_GenerationThroughput) {
    const TerrainSettings settings = GameTerrain();
    JobSystem jobs(0);
    std::printf("workers: %u\n", jobs.ThreadCount());
    // Terrain extremes within 70 m of the origin.
    float lo = 1.0e9f, hi = -1.0e9f;
    glm::vec2 loAt(0.0f), hiAt(0.0f);
    for (float z = -70.0f; z <= 70.0f; z += 1.0f)
        for (float x = -70.0f; x <= 70.0f; x += 1.0f) {
            if (x * x + z * z > 70.0f * 70.0f) continue;
            const float h = TerrainHeight(settings, x, z);
            if (h < lo) { lo = h; loAt = {x, z}; }
            if (h > hi) { hi = h; hiAt = {x, z}; }
        }
    std::printf("lowest %.2f m at (%.0f, %.0f), highest %.2f m at (%.0f, %.0f)\n", static_cast<double>(lo),
                static_cast<double>(loAt.x), static_cast<double>(loAt.y), static_cast<double>(hi), static_cast<double>(hiAt.x),
                static_cast<double>(hiAt.y));

    // Every chunk of a 16 x 16 column block, rows -15..39 (-48..128 m), from a fresh generator.
    for (int pass = 0; pass < 2; ++pass) {
        const auto generator = CreateTerrainGenerator(settings);
        std::atomic<uint32_t> counts[3] = {0, 0, 0};
        const int columns = 16, rows = 55, rowMin = -15;
        const auto start = std::chrono::steady_clock::now();
        jobs.ParallelFor(static_cast<size_t>(columns * columns * rows), 1, [&](size_t begin, size_t end) {
            auto chunk = std::make_unique<Chunk>();
            for (size_t i = begin; i < end; ++i) {
                const int r = static_cast<int>(i % rows);
                const int c = static_cast<int>(i / rows);
                chunk->Clear();
                const glm::ivec3 coord(c % columns + 20 * pass, rowMin + r, c / columns);
                const ChunkContent content = generator->Generate(coord, *chunk);
                if (content.kind == ChunkKind::Dense) chunk->RebuildOccupancy();
                counts[static_cast<int>(content.kind)].fetch_add(1);
            }
        });
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const uint32_t total = counts[0] + counts[1] + counts[2];
        std::printf("pass %d: %u chunks (%u empty, %u uniform, %u dense) in %.3f s: %.0f chunks/s, %.0f dense chunks/s\n", pass, total,
                    counts[0].load(), counts[1].load(), counts[2].load(), seconds, total / seconds, counts[2] / seconds);
    }
}

namespace {
    /// A round hill, 100 m high at the origin, flat valley floor at 0 m beyond about 200 m.
    /// No column pass (the default ClassifyColumn): every row of the band gets a job.
    class HillGenerator final : public IWorldGenerator {
    public:
        explicit HillGenerator(float voxelSize) : m_Voxel(voxelSize) {}
        static float Height(float x, float z) { return 100.0f * std::exp(-(x * x + z * z) / (80.0f * 80.0f)); }

        ChunkContent Generate(const glm::ivec3& coord, Chunk& chunk) const override {
            bool any = false;
            for (int z = 0; z < kChunkSize; ++z)
                for (int x = 0; x < kChunkSize; ++x) {
                    const float wx = (static_cast<float>(coord.x * kChunkSize + x) + 0.5f) * m_Voxel;
                    const float wz = (static_cast<float>(coord.z * kChunkSize + z) + 0.5f) * m_Voxel;
                    const int surface = static_cast<int>(std::floor(Height(wx, wz) / m_Voxel));
                    for (int y = 0; y < kChunkSize; ++y) {
                        if (coord.y * kChunkSize + y >= surface) break;
                        chunk.ids[Chunk::VoxelIndex(x, y, z)] = 1;
                        any = true;
                    }
                }
            return {any ? ChunkKind::Dense : ChunkKind::Empty, 0};
        }
        HeightBounds VerticalBounds() const override { return {-8.0f, 101.0f}; }

    private:
        float m_Voxel;
    };

    struct HillWorld {
        explicit HillWorld(bool fullHeight, int rows = 4) : store(2048), jobs(3) {
            WorldConfig config;
            config.voxelSize = 0.5f;                       // 16 m chunks
            config.windowSize = {8, rows, 8};
            config.fullHeight = fullHeight;
            world = std::make_unique<World>(config, store, jobs, std::make_shared<HillGenerator>(config.voxelSize));
        }
        ChunkStore store;
        JobSystem jobs;
        std::unique_ptr<World> world;
    };

    TerrainSettings CoarseTerrain(uint32_t seed = 1337) {
        TerrainSettings s;
        s.seed = seed;
        s.voxelSize = 0.5f;                                // 16 m chunks: a small window spans real relief
        return s;
    }

    /// Two worlds on the built-in terrain, full height, one of them packing hidden chunks.
    struct TerrainWorld {
        explicit TerrainWorld(bool pack, uint32_t seed = 1337, const std::function<void(WorldConfig&)>& tweak = {}) : store(4096), jobs(3) {
            WorldConfig config;
            config.voxelSize = 0.5f;
            config.windowSize = {8, 4, 8};
            config.fullHeight = true;
            config.packHidden = pack;
            config.nearRadius = 4.0f;
            if (tweak) tweak(config);
            TerrainSettings terrain = CoarseTerrain(seed);
            terrain.materials = RegisterDefaultWorldMaterials(materials);
            generator = CreateTerrainGenerator(terrain);
            world = std::make_unique<World>(config, store, jobs, generator, &materials);
        }
        WorldHashResult Hash() const {
            return WorldHash(*world, world->WindowMinChunk(), world->WindowMinChunk() + world->WindowSize());
        }
        ChunkStore store;
        JobSystem jobs;
        MaterialLibrary materials;
        std::shared_ptr<IWorldGenerator> generator;
        std::unique_ptr<World> world;
    };

    /// Index of a chunk's cell in Grid() / GridWords() (column-major, toroidal).
    size_t GridIndex(const World& w, const glm::ivec3& c) {
        const glm::ivec3 s = w.WindowSize();
        return static_cast<size_t>((math::FloorMod(c.x, s.x) + math::FloorMod(c.z, s.z) * s.x) * s.y + math::FloorMod(c.y, s.y));
    }

    /// Calls `fn(chunk)` for every chunk of the window.
    void ForEachWindowChunk(const World& w, const std::function<void(const glm::ivec3&)>& fn) {
        const glm::ivec3 min = w.WindowMinChunk(), size = w.WindowSize();
        for (int y = 0; y < size.y; ++y)
            for (int z = 0; z < size.z; ++z)
                for (int x = 0; x < size.x; ++x) fn(min + glm::ivec3(x, y, z));
    }

    /// The geometric rule, recomputed from the generator: no Packed chunk lies at or above the
    /// lowest surface voxel of a 3 x 3 neighbour column inside the window.
    int PackedButVisible(const TerrainWorld& t) {
        const World& w = *t.world;
        const glm::ivec3 min = w.WindowMinChunk(), size = w.WindowSize();
        int bad = 0;
        ForEachWindowChunk(w, [&](const glm::ivec3& c) {
            if (w.StateOf(c) != CellState::Packed) return;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = c.x + dx, nz = c.z + dz;
                    if (nx < min.x || nx >= min.x + size.x || nz < min.z || nz >= min.z + size.z) continue;
                    if (c.y >= t.generator->ClassifyColumn(nx, nz).surfaceFrom + 1) {
                        ++bad;
                        return;
                    }
                }
        });
        return bad;
    }

    /// Rays from the top of the window down at every angle: how many of the terrain hits are
    /// on a Packed chunk.
    void SkyRays(World& w, uint32_t seed, int& hits, int& packed) {
        Rng rng(seed);
        const glm::dvec3 origin = w.WindowOrigin();
        const glm::dvec3 extent = glm::dvec3(w.WindowSize()) * (kChunkSize * static_cast<double>(w.VoxelSize()));
        hits = packed = 0;
        for (int i = 0; i < 20000; ++i) {
            const glm::dvec3 from = origin + glm::dvec3(rng.NextFloat() * extent.x, extent.y - 1.0, rng.NextFloat() * extent.z);
            const glm::vec3 d = glm::normalize(glm::vec3(rng.Range(-1.0f, 1.0f), -rng.Range(0.05f, 1.0f), rng.Range(-1.0f, 1.0f)));
            const WorldRayHit hit = w.Raycast(from, d, 2000.0f);
            if (!hit.hit) continue;
            ++hits;
            // The voxel just in front of the hit face tells which chunk the ray saw.
            if (w.StateOf(VoxelToChunk(hit.voxel)) == CellState::Packed) ++packed;
        }
    }

    /// Column tops, content top and region tops in GridWords() agree with the cells.
    void ExpectTopsConsistent(const World& w, const char* when) {
        SCOPED_TRACE(when);
        const glm::ivec3 s = w.WindowSize();
        const std::span<const uint32_t> words = w.GridWords();
        const size_t cells = w.Grid().size();
        const int regionsX = (s.x + 3) / 4, regionsZ = (s.z + 3) / 4;
        ASSERT_EQ(words.size(), cells + static_cast<size_t>(s.x * s.z) + 1 + static_cast<size_t>(regionsX * regionsZ));
        std::vector<uint32_t> regionTops(static_cast<size_t>(regionsX * regionsZ), 0u);
        uint32_t maxTop = 0;
        int wrong = 0;
        for (int wz = 0; wz < s.z; ++wz)
            for (int wx = 0; wx < s.x; ++wx) {
                uint32_t top = 0;
                for (int rel = 0; rel < s.y; ++rel) {
                    const int wy = math::FloorMod(w.WindowMinChunk().y + rel, s.y);
                    if (words[static_cast<size_t>((wx + wz * s.x) * s.y + wy)] != 0u) top = static_cast<uint32_t>(rel + 1);
                }
                if (words[cells + static_cast<size_t>(wx + wz * s.x)] != top) ++wrong;
                maxTop = std::max(maxTop, top);
                const int cx = math::FloorMod(wx - w.WindowMinChunk().x, s.x), cz = math::FloorMod(wz - w.WindowMinChunk().z, s.z);
                uint32_t& region = regionTops[static_cast<size_t>(cx / 4 + (cz / 4) * regionsX)];
                region = std::max(region, top);
            }
        EXPECT_EQ(wrong, 0) << "column tops";
        EXPECT_EQ(words[cells + static_cast<size_t>(s.x * s.z)], maxTop) << "content top";
        for (size_t r = 0; r < regionTops.size(); ++r)
            EXPECT_EQ(words[cells + static_cast<size_t>(s.x * s.z) + 1 + r], regionTops[r]) << "region " << r;
        for (const uint64_t page : w.GridPageRevisions()) EXPECT_LE(page, w.GridRevision());
    }
}

// The hill bug: with the camera-centred window, walking up the hill re-targets the bottom rows
// (unloads inside the view radius); the full-height window keeps its rows and unloads nothing
// that is still inside the window horizontally.
TEST(ViewDistance, ClimbingKeepsTheWindowRowsAndUnloadsNothing) {
    HillWorld full(true);
    World& w = *full.world;
    ASSERT_TRUE(w.FullHeight());
    // -8 .. 101 + 32 m headroom, snapped to 16 m: -16 .. 144 m = rows -1 .. 9.
    EXPECT_EQ(w.WindowSize().y, 10);
    EXPECT_DOUBLE_EQ(w.BuildCeiling(), 144.0);

    auto walk = [](World& world, glm::dvec3 from, glm::dvec3 to) {
        for (int i = 0; i <= 40; ++i) {
            const glm::dvec3 p = glm::mix(from, to, i / 40.0);
            // Loaded at every step: a cell that is re-targeted held a chunk.
            world.LoadBlocking(glm::dvec3(p.x, HillGenerator::Height(static_cast<float>(p.x), static_cast<float>(p.z)) + 1.7, p.z));
        }
    };
    const glm::dvec3 valley(300.0, 0.0, 0.0), peak(0.0, 0.0, 0.0);
    w.LoadBlocking(glm::dvec3(valley.x, 1.7, valley.z));
    const int row = w.WindowMinChunk().y;
    walk(w, valley, peak);
    EXPECT_EQ(w.WindowMinChunk().y, row);
    walk(w, peak, valley);
    w.LoadBlocking(glm::dvec3(valley.x, 1.7, valley.z));
    EXPECT_EQ(w.WindowMinChunk().y, row);
    EXPECT_EQ(w.Stats().unloadsInsideRadius, 0u);
    // At the peak the valley floor and the peak are both in the window.
    w.LoadBlocking(glm::dvec3(0.0, 101.7, 0.0));
    EXPECT_TRUE(w.IsSolidAt(glm::dvec3(0.0, 99.0, 0.0)));
    EXPECT_TRUE(w.IsSolidAt(glm::dvec3(60.0, 1.0, 0.0)));
    EXPECT_EQ(w.Stats().unloadsInsideRadius, 0u);
    EXPECT_EQ(w.Stats().storeFullLoads, 0u);
    EXPECT_EQ(w.Stats().storeFullDecodes, 0u);

    // The old window, 4 rows around the camera: the same walk unloads rows inside the radius
    // (the counter measures what is lost, by distance: it can fail).
    HillWorld old(false);
    World& o = *old.world;
    o.LoadBlocking(glm::dvec3(valley.x, 1.7, valley.z));
    walk(o, valley, peak);
    EXPECT_GT(o.Stats().unloadsInsideRadius, 0u);
    EXPECT_NE(o.WindowMinChunk().y, row);
}

TEST(ViewDistance, ClassifyColumnAgreesWithGenerate) {
    auto chunk = std::make_unique<Chunk>();
    for (const uint32_t seed : {1337u, 77u, 0x9e3779b9u}) {
        TerrainSettings settings;
        settings.seed = seed;
        settings.materials = RegisterDefaultWorldMaterials(*std::make_unique<MaterialLibrary>());
        const auto generator = CreateTerrainGenerator(settings);
        // The same terrain without the early-out: every row runs the voxel loop (caves, soil,
        // sea, trees), so the rows the column pass skips are checked voxel by voxel.
        TerrainSettings fullSettings = settings;
        fullSettings.skipClassifiedRows = false;
        const auto full = CreateTerrainGenerator(fullSettings);
        const HeightBounds bounds = generator->VerticalBounds();
        ASSERT_TRUE(bounds.Known());
        const float chunkMetres = kChunkSize * settings.voxelSize;
        int checked = 0, fullChecked = 0;
        for (int i = 0; i < 64; ++i) {
            // Columns near the origin and out to 1 km.
            const int cx = (i % 8 - 4) * (i < 32 ? 3 : 41), cz = (i / 8 - 4) * (i < 32 ? 5 : 37);
            const ColumnClass c = generator->ClassifyColumn(cx, cz);
            ASSERT_TRUE(c.known);
            ASSERT_LT(c.solidBelow, c.emptyFrom);
            EXPECT_GE(c.surfaceFrom, c.solidBelow);
            EXPECT_LT(c.surfaceFrom, c.emptyFrom);
            // The band lies inside the declared bounds (rows of the bounds, rounded outwards).
            EXPECT_GE(c.solidBelow, static_cast<int>(std::floor(bounds.min / chunkMetres)) - 1);
            EXPECT_LE(c.emptyFrom, static_cast<int>(std::ceil(bounds.max / chunkMetres)) + 1);
            for (int row = c.solidBelow - 2; row <= c.emptyFrom + 1; ++row) {
                chunk->Clear();
                const ChunkContent content = generator->Generate(glm::ivec3(cx, row, cz), *chunk);
                if (row >= c.emptyFrom) {
                    EXPECT_EQ(content.kind, ChunkKind::Empty) << seed << " " << cx << " " << row << " " << cz;
                } else if (row < c.solidBelow) {
                    EXPECT_EQ(content.kind, ChunkKind::Uniform);
                    EXPECT_EQ(content.uniformId, c.solidId);
                } else {
                    EXPECT_EQ(content.kind, ChunkKind::Dense);
                }
                ++checked;
                if (row >= c.solidBelow && row < c.emptyFrom) continue;
                chunk->Clear();
                const ChunkContent generated = full->Generate(glm::ivec3(cx, row, cz), *chunk);
                if (generated.kind == ChunkKind::Dense) chunk->RebuildOccupancy();
                const bool empty = generated.kind == ChunkKind::Empty || (generated.kind == ChunkKind::Dense && chunk->IsEmpty());
                const int uniform = generated.kind == ChunkKind::Uniform ? generated.uniformId
                                    : generated.kind == ChunkKind::Dense ? chunk->UniformId() : -1;
                if (row >= c.emptyFrom) EXPECT_TRUE(empty) << "voxels in a skipped sky row " << seed << " " << cx << " " << row << " " << cz;
                else EXPECT_EQ(uniform, static_cast<int>(c.solidId)) << "a skipped rock row is not all dark stone " << seed << " " << cx << " " << row << " " << cz;
                ++fullChecked;
            }
        }
        EXPECT_GT(checked, 64 * 4);
        EXPECT_EQ(fullChecked, 64 * 4);
    }
    // The flat generator's column pass.
    const auto flat = CreateFlatGenerator(40, 2, 3);
    const ColumnClass c = flat->ClassifyColumn(5, -7);
    for (int row = -3; row < 4; ++row) {
        auto scratch = std::make_unique<Chunk>();
        const ChunkContent content = flat->Generate(glm::ivec3(5, row, -7), *scratch);
        if (row >= c.emptyFrom) EXPECT_EQ(content.kind, ChunkKind::Empty);
        else if (row < c.solidBelow) EXPECT_EQ(content.kind, ChunkKind::Uniform);
        else EXPECT_EQ(content.kind, ChunkKind::Dense);
    }
}

TEST(ViewDistance, TerrainHeightsStayInsideTheDeclaredBounds) {
    for (const uint32_t seed : {1337u, 77u}) {
        TerrainSettings settings;
        settings.seed = seed;
        const HeightBounds surface = TerrainSurfaceBounds(settings);
        const HeightBounds bounds = CreateTerrainGenerator(settings)->VerticalBounds();
        EXPECT_LT(bounds.min, surface.min - settings.caveMaxDepth);
        EXPECT_GT(bounds.max, surface.max + 7.0f);
        Rng rng(seed);
        float lo = 1.0e9f, hi = -1.0e9f;
        for (int i = 0; i < 500000; ++i) {
            const float x = rng.Range(-10000.0f, 10000.0f), z = rng.Range(-10000.0f, 10000.0f);
            const float h = TerrainHeight(settings, x, z);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
            ASSERT_GE(h, surface.min);
            ASSERT_LE(h, surface.max);
        }
        std::printf("seed %u: %.2f .. %.2f m sampled, bounds %.2f .. %.2f m\n", seed, static_cast<double>(lo), static_cast<double>(hi),
                    static_cast<double>(surface.min), static_cast<double>(surface.max));
        // The clamp never engaged on these samples (a clamped height equals a bound exactly), with
        // a margin of a few voxels: bounds that were too tight would flatten peaks into plateaus.
        const float margin = 4.0f * settings.voxelSize;
        EXPECT_GT(lo, surface.min + margin);
        EXPECT_LT(hi, surface.max - margin);
    }
}

namespace {
    void ExpectSameEdits(const std::vector<VoxelEdit>& a, const std::vector<VoxelEdit>& b, const EditReport& ra, const EditReport& rb) {
        ASSERT_EQ(a.size(), b.size());
        for (size_t k = 0; k < a.size(); ++k) {
            EXPECT_EQ(a[k].voxel, b[k].voxel);
            EXPECT_EQ(a[k].oldId, b[k].oldId);
            EXPECT_EQ(a[k].newId, b[k].newId);
        }
        EXPECT_EQ(ra.skipped, rb.skipped);
        EXPECT_EQ(ra.unsynced, rb.unsynced);
        EXPECT_EQ(ra.outOfBounds, rb.outOfBounds);
    }
}

// Packing is not a content change: the same edits on a world that packs hidden chunks and on
// one that does not give the same voxels, the same edit lists and the same WorldHash.
TEST(ViewDistance, PackedAndUnpackedWorldsTakeTheSameEdits) {
    TerrainWorld plain(false), packed(true);
    const glm::dvec3 focus(20.0, 20.0, -10.0);
    plain.world->LoadBlocking(focus);
    packed.world->LoadBlocking(focus);
    const WorldStats ps = packed.world->Stats();
    ASSERT_GT(ps.packedChunks, 0u);
    EXPECT_GT(ps.packedBytes, 0u);
    EXPECT_LT(ps.denseChunks, plain.world->Stats().denseChunks);
    ASSERT_TRUE(plain.Hash().Complete());
    EXPECT_EQ(plain.Hash().hash, packed.Hash().hash);

    // Edit inside Packed chunks: find some.
    std::vector<glm::ivec3> packedChunks;
    const glm::ivec3 min = packed.world->WindowMinChunk(), size = packed.world->WindowSize();
    for (int y = 0; y < size.y; ++y)
        for (int z = 0; z < size.z; ++z)
            for (int x = 0; x < size.x; ++x) {
                const glm::ivec3 c = min + glm::ivec3(x, y, z);
                if (packed.world->StateOf(c) == CellState::Packed) packedChunks.push_back(c);
            }
    ASSERT_GE(packedChunks.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        const glm::ivec3 c = packedChunks[i * packedChunks.size() / 3];
        const glm::dvec3 centre = glm::dvec3(ChunkToVoxel(c)) + glm::dvec3(16.3, 9.6, 20.1);
        const SphereQ sphere = QuantizeSphere(centre, 9.0);
        std::vector<VoxelEdit> a, b;
        EditReport ra, rb;
        const WorldStats statsBefore = packed.world->Stats();
        EXPECT_EQ(plain.world->CarveSphereQ(sphere, QuantizePower(100.0f), &a, &ra),
                  packed.world->CarveSphereQ(sphere, QuantizePower(100.0f), &b, &rb));
        const WorldStats statsAfter = packed.world->Stats();
        std::printf("carve into a packed chunk: %zu voxels, %u decodes in %.3f ms\n", b.size(), statsAfter.decodes - statsBefore.decodes,
                    statsAfter.decodeMs - statsBefore.decodeMs);
        ExpectSameEdits(a, b, ra, rb);
        // The edited chunk and its six face neighbours are resident now.
        EXPECT_NE(packed.world->StateOf(c), CellState::Packed);
        for (const glm::ivec3 f : {glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 1, 0), glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1),
                                   glm::ivec3(0, 0, -1)}) {
            EXPECT_NE(packed.world->StateOf(c + f), CellState::Packed);
        }
        // A fill and a swap that reach into packed neighbours as well.
        const SphereQ fill = QuantizeSphere(centre + glm::dvec3(30.0, -20.0, 0.0), 6.0);
        EXPECT_EQ(plain.world->FillSphereQ(fill, 5), packed.world->FillSphereQ(fill, 5));
        EXPECT_EQ(plain.world->SwapSphereQ(fill, 5, 7), packed.world->SwapSphereQ(fill, 5, 7));
    }
    // Edits across chunk seams: centred on a corner of a Packed chunk, each touches up to 8
    // chunks in mixed states (Packed, Dense, Uniform, Empty) inside one EditRegion call.
    int corners = 0;
    for (size_t i = 0; i < packedChunks.size() && corners < 4; i += 5) {
        const glm::ivec3 c = packedChunks[i];
        if (packed.world->StateOf(c) != CellState::Packed) continue;    // made resident by an earlier edit
        ++corners;
        const glm::dvec3 corner = glm::dvec3(ChunkToVoxel(c)) + glm::dvec3(31.5, 0.5, 31.5);
        std::vector<VoxelEdit> a, b;
        EditReport ra, rb;
        EXPECT_EQ(plain.world->CarveSphereQ(QuantizeSphere(corner, 6.0), QuantizePower(100.0f), &a, &ra),
                  packed.world->CarveSphereQ(QuantizeSphere(corner, 6.0), QuantizePower(100.0f), &b, &rb));
        ExpectSameEdits(a, b, ra, rb);
        std::vector<VoxelEdit> fa, fb;
        EditReport rfa, rfb;
        const SphereQ fill = QuantizeSphere(corner + glm::dvec3(0.0, 0.0, -32.0), 5.0);
        EXPECT_EQ(plain.world->FillSphereQ(fill, 5, &fa, &rfa), packed.world->FillSphereQ(fill, 5, &fb, &rfb));
        ExpectSameEdits(fa, fb, rfa, rfb);
        std::vector<VoxelEdit> sa, sb;
        EditReport rsa, rsb;
        EXPECT_EQ(plain.world->SwapSphereQ(fill, 5, 7, &sa, &rsa), packed.world->SwapSphereQ(fill, 5, 7, &sb, &rsb));
        ExpectSameEdits(sa, sb, rsa, rsb);
    }
    EXPECT_GE(corners, 2);
    // A packed chunk read through GetVoxel / ReadChunk gives the voxels the plain world has.
    auto ca = std::make_unique<Chunk>(), cb = std::make_unique<Chunk>();
    for (size_t i = 0; i < packedChunks.size(); i += 7) {
        const glm::ivec3 c = packedChunks[i];
        ASSERT_TRUE(plain.world->ReadChunk(c, *ca));
        ASSERT_TRUE(packed.world->ReadChunk(c, *cb));
        EXPECT_EQ(0, std::memcmp(ca->ids, cb->ids, sizeof(ca->ids)));
        for (int k = 0; k < 64; ++k) {
            const glm::ivec3 v = ChunkToVoxel(c) + glm::ivec3((k * 7) % 32, (k * 13) % 32, (k * 29) % 32);
            EXPECT_EQ(plain.world->GetVoxel(v), packed.world->GetVoxel(v));
        }
    }
    EXPECT_EQ(plain.Hash().hash, packed.Hash().hash);
    EXPECT_GT(packed.world->Stats().decodes, 0u);
}

// Above the build ceiling is outside the world: an edit that would put voxels there is refused
// whole, one that only removes (nothing is there) is not.
TEST(ViewDistance, EditsAboveTheBuildCeilingAreRefusedWhole) {
    TerrainWorld t(false);
    World& w = *t.world;
    w.LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    const WorldHashResult before = t.Hash();
    const double ceiling = w.BuildCeiling();
    ASSERT_TRUE(std::isfinite(ceiling));
    const int ceilingVoxel = static_cast<int>(std::lround(ceiling / w.VoxelSize()));

    // A model-sized box 1 m above the ceiling, and one straddling it.
    EditReport above;
    EXPECT_EQ(w.FillBox({0, ceilingVoxel + 2, 0}, {4, ceilingVoxel + 6, 4}, 5, nullptr, &above), 0u);
    EXPECT_TRUE(above.outOfBounds);
    EditReport straddle;
    EXPECT_EQ(w.FillBox({0, ceilingVoxel - 4, 0}, {4, ceilingVoxel + 1, 4}, 5, nullptr, &straddle), 0u);
    EXPECT_TRUE(straddle.outOfBounds);
    EXPECT_EQ(w.GetVoxel({1, ceilingVoxel - 2, 1}), 0);
    EditReport sphere;
    EXPECT_EQ(w.FillSphereQ(QuantizeSphere(glm::dvec3(2.0, ceilingVoxel - 1.0, 2.0), 3.0), 5, nullptr, &sphere), 0u);
    EXPECT_TRUE(sphere.outOfBounds);
    EXPECT_EQ(t.Hash().hash, before.hash);

    // Carving the sky above the ceiling changes nothing and is not refused.
    EditReport carve;
    EXPECT_EQ(w.CarveSphereQ(QuantizeSphere(glm::dvec3(2.0, ceilingVoxel + 2.0, 2.0), 5.0), 1000, nullptr, &carve), 0u);
    EXPECT_FALSE(carve.outOfBounds);
    // Just below the ceiling is fine.
    EditReport below;
    EXPECT_EQ(w.FillBox({0, ceilingVoxel - 4, 0}, {4, ceilingVoxel, 4}, 5, nullptr, &below), 64u);
    EXPECT_FALSE(below.outOfBounds);
}

// The column tops in GridWords() say where each column turns into sky, and the content top is
// their maximum; band cells not generated yet are VX_GRID_PENDING (uniform flag, id 0). Checked
// after a load, while streaming, after edits and after a window move, where World maintains them
// itself (raised at once, lowered by a search, region and content tops lazily).
TEST(ViewDistance, ColumnTopsAndPendingCellsDescribeTheGrid) {
    TerrainWorld t(true);
    World& w = *t.world;
    const glm::dvec3 focus(0.0, 20.0, 0.0);
    w.LoadBlocking(focus);
    const glm::ivec3 s = w.WindowSize();
    const std::span<const uint32_t> words = w.GridWords();
    const size_t cells = w.Grid().size();
    ExpectTopsConsistent(w, "after the load");
    EXPECT_LT(words[cells + static_cast<size_t>(s.x * s.z)], static_cast<uint32_t>(s.y));      // the headroom is sky

    // Raise: a fill in the first sky row of the focus column.
    const glm::ivec3 fc = VoxelToChunk(w.WorldToVoxel(focus));
    const uint32_t top = words[cells + static_cast<size_t>(math::FloorMod(fc.x, s.x) + math::FloorMod(fc.z, s.z) * s.x)];
    ASSERT_LT(top + 1, static_cast<uint32_t>(s.y));
    const glm::ivec3 sky(fc.x, w.WindowMinChunk().y + static_cast<int>(top), fc.z);
    ASSERT_EQ(w.StateOf(sky), CellState::Empty);
    const glm::ivec3 v = ChunkToVoxel(sky) + glm::ivec3(10, 3, 10);
    EXPECT_EQ(w.FillBox(v, v + glm::ivec3(4), 5), 64u);
    EXPECT_EQ(words[cells + static_cast<size_t>(math::FloorMod(fc.x, s.x) + math::FloorMod(fc.z, s.z) * s.x)], top + 1);
    ExpectTopsConsistent(w, "after a fill above the column top");
    // Carving it away again keeps the (now empty) dense chunk: the top stays, which only costs steps.
    EXPECT_EQ(w.FillBox(v, v + glm::ivec3(4), 0), 64u);
    w.Update(focus);
    ExpectTopsConsistent(w, "after carving the fill away");

    // Move far away: new columns get classified; band cells are pending until generated, and
    // the tops count them.
    const glm::dvec3 far(2000.0, 20.0, 0.0);
    w.Update(far);
    int pending = 0;
    for (int i = 0; i < 400 && pending == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        w.Update(far);
        for (size_t k = 0; k < cells; ++k) pending += words[k] == 0x80000000u ? 1 : 0;
    }
    EXPECT_GT(pending, 0);
    ExpectTopsConsistent(w, "while streaming");
    // A ray that meets a pending cell stops there: the tracer reports it with material 0, and
    // World::Raycast reports no hit. Cast straight down onto a pending cell that is its column's top.
    int rays = 0;
    ForEachWindowChunk(w, [&](const glm::ivec3& c) {
        if (rays >= 4 || words[GridIndex(w, c)] != 0x80000000u) return;
        const uint32_t columnTop = words[cells + static_cast<size_t>(math::FloorMod(c.x, s.x) + math::FloorMod(c.z, s.z) * s.x)];
        if (c.y - w.WindowMinChunk().y + 1 != static_cast<int>(columnTop)) return;
        const glm::dvec3 above = w.VoxelCenter(ChunkToVoxel(c) + glm::ivec3(16, 0, 16)) +
                                 glm::dvec3(0.0, (s.y - (c.y - w.WindowMinChunk().y)) * kChunkSize * static_cast<double>(w.VoxelSize()), 0.0);
        const RayHit hit = TraceWorld(w.MakeTraceScene(), glm::vec3(above - w.WindowOrigin()), glm::vec3(0.0f, -1.0f, 0.0f), 0.0f, 1.0e4f);
        EXPECT_TRUE(hit.hit);
        EXPECT_EQ(hit.material, 0);
        EXPECT_EQ(VoxelToChunk(hit.voxel), c);
        EXPECT_FALSE(w.Raycast(above, glm::vec3(0.0f, -1.0f, 0.0f), 1.0e4f).hit);
        ++rays;
    });
    EXPECT_GT(rays, 0);
    w.LoadBlocking(far);
    for (size_t k = 0; k < cells; ++k) EXPECT_NE(words[k], 0x80000000u);
    ExpectTopsConsistent(w, "after the move and the load");
}

// What a renderer shows for a dense cell whose slot is not on the GPU yet: what the cell showed
// before (pending, or the packed stand-in), never a hole. Every World has its own instance id.
TEST(ViewDistance, GridFallbackIsWhatTheCellShowedBefore) {
    TerrainWorld t(true);
    World& w = *t.world;
    w.LoadBlocking(glm::dvec3(20.0, 20.0, -10.0));
    const std::span<const uint32_t> words = w.GridWords();
    int dense = 0, packedSeen = 0;
    glm::ivec3 packedChunk(0);
    ForEachWindowChunk(w, [&](const glm::ivec3& c) {
        const uint32_t word = words[GridIndex(w, c)];
        if (w.StateOf(c) == CellState::Packed) {
            packedChunk = c;
            ++packedSeen;
        }
        if (word == 0u || (word & 0x80000000u) != 0u) return;
        ++dense;
        // Every band cell of a full-height window was pending (or packed) before it loaded.
        EXPECT_NE(w.GridFallback(static_cast<uint32_t>(GridIndex(w, c))) & 0x80000000u, 0u);
    });
    EXPECT_GT(dense, 0);
    ASSERT_GT(packedSeen, 0);
    const uint32_t packedWord = words[GridIndex(w, packedChunk)];
    ASSERT_NE(packedWord & 0xFFu, 0u);
    EXPECT_EQ(w.EnsureResident(packedChunk, packedChunk), 0u);
    ASSERT_EQ(w.StateOf(packedChunk), CellState::Dense);
    EXPECT_EQ(w.GridFallback(static_cast<uint32_t>(GridIndex(w, packedChunk))), packedWord);

    TerrainWorld other(true);
    EXPECT_NE(other.world->InstanceId(), w.InstanceId());
}

TEST(ViewDistance, SurfaceLoadLeavesCavesForLater) {
    TerrainWorld t(true);
    World& w = *t.world;
    const glm::dvec3 focus(0.0, 20.0, 0.0);
    w.LoadBlocking(focus, LoadScope::Surface);
    EXPECT_TRUE(w.IsFilled(LoadScope::Surface));
    w.LoadBlocking(focus, LoadScope::Everything);
    EXPECT_TRUE(w.IsFilled(LoadScope::Everything));
    EXPECT_EQ(w.Stats().pendingChunks, 0u);
    EXPECT_EQ(w.UnfilledColumns(), 0u);
    EXPECT_EQ(w.Stats().storeFullLoads, 0u);
    EXPECT_EQ(w.Stats().storeFullDecodes, 0u);
}

// A one-chunk step re-targets one slab of columns, not the window; measured on the game's
// window size (48 x 55 x 48 at 0.1 m).
TEST(ViewDistance, RecenterTouchesOnlyTheEnteringColumns) {
    ChunkStore store(8192);
    JobSystem jobs(3);
    WorldConfig config;
    config.voxelSize = 0.1f;
    config.windowSize = {48, 8, 48};
    config.fullHeight = true;
    config.heightRange = {-48.0f, 128.0f};
    World w(config, store, jobs, CreateFlatGenerator(45, 2, 3));
    ASSERT_EQ(w.WindowSize().y, 55);
    w.LoadBlocking(glm::dvec3(0.0, 5.0, 0.0));
    const uint32_t dense = w.Stats().denseChunks;
    EXPECT_EQ(dense, 48u * 48u);
    for (int i = 1; i <= 24; ++i) {
        w.Update(glm::dvec3(3.2 * i + 0.1, 5.0, 1.6 * (i / 2)));
    }
    const WorldStats s = w.Stats();
    std::printf("recentre: %u moves, %.3f ms max per step of one chunk, %.3f ms last\n", s.recenters,
                static_cast<double>(s.maxRecenterMs), static_cast<double>(s.lastRecenterMs));
    EXPECT_EQ(s.unloadsInsideRadius, 0u);
    EXPECT_EQ(s.recenters, 25u);        // timing is printed, not gated here (ctest runs in parallel; the gate is the headless run)
    glm::dvec3 focus(3.2 * 24 + 0.1, 5.0, 1.6 * 12);
    w.LoadBlocking(focus);
    EXPECT_EQ(w.Stats().denseChunks, dense);

    // One step in x, then one diagonal step: exactly the entering slab is re-targeted (48, then
    // 48 + 48 - 1 columns), every other column keeps its chunks where they were.
    const auto step = [&](const glm::dvec3& delta, uint32_t expectColumns) {
        const glm::ivec3 oldMin = w.WindowMinChunk();
        const int groundRow = 1;                        // flat ground at 45 voxels: row 1
        std::vector<std::pair<glm::ivec3, const Chunk*>> kept;
        for (int z = 0; z < 48; ++z)
            for (int x = 0; x < 48; ++x) {
                const glm::ivec3 c(oldMin.x + x, groundRow, oldMin.z + z);
                const Chunk* chunk = w.GetChunk(c);
                ASSERT_NE(chunk, nullptr);
                kept.emplace_back(c, chunk);
            }
        const uint32_t before = w.Stats().retargetedColumns;
        focus += delta;
        w.Update(focus);
        const glm::ivec3 newMin = w.WindowMinChunk();
        EXPECT_EQ(w.Stats().retargetedColumns - before, expectColumns);
        int stayed = 0;
        for (const auto& [c, chunk] : kept) {
            if (c.x < newMin.x || c.x >= newMin.x + 48 || c.z < newMin.z || c.z >= newMin.z + 48) continue;
            EXPECT_EQ(w.GetChunk(c), chunk);
            ++stayed;
        }
        EXPECT_EQ(stayed, 48 * 48 - static_cast<int>(expectColumns));
        w.LoadBlocking(focus);
    };
    step(glm::dvec3(3.2, 0.0, 0.0), 48u);
    step(glm::dvec3(3.2, 0.0, 3.2), 95u);
    EXPECT_EQ(w.Stats().unloadsInsideRadius, 0u);
}

namespace {
    /// A window of the game's size (48 x 55 x 48 chunks of 32 voxels, voxel = 1 ray unit) filled
    /// like terrain: rock below a surface row per column, dense chunks around it (shared random
    /// content), some floating chunks above, and some columns half loaded (pending cells).
    struct ColumnScene {
        static constexpr glm::ivec3 kSize{48, 55, 48};
        ChunkStore store{16};
        std::vector<uint32_t> grid, tops, regions;
        glm::ivec3 gridMin{-7, -15, 13};
        uint32_t contentTop = 0;

        ColumnScene() {
            Rng rng(99);
            for (int k = 0; k < 8; ++k) {
                const uint32_t slot = store.Alloc();
                Chunk& c = store.Get(slot);
                for (int i = 0; i < 3000; ++i) {
                    const glm::ivec3 p(static_cast<int>(rng.NextBounded(32)), static_cast<int>(rng.NextBounded(32)), static_cast<int>(rng.NextBounded(32)));
                    c.Set(p.x, p.y, p.z, static_cast<uint8_t>(1 + rng.NextBounded(9)));
                }
                for (int x = 0; x < 32; ++x)
                    for (int z = 0; z < 32; ++z)
                        for (int y = 0; y < (x + z) % 9; ++y) c.Set(x, y, z, 4);
            }
            grid.assign(static_cast<size_t>(kSize.x * kSize.y * kSize.z), 0u);
            tops.assign(static_cast<size_t>(kSize.x * kSize.z), 0u);
            for (int cz = 0; cz < kSize.z; ++cz)
                for (int cx = 0; cx < kSize.x; ++cx) {
                    const int surface = 14 + static_cast<int>(rng.NextBounded(12));
                    const bool halfLoaded = rng.NextBounded(7) == 0;
                    for (int cy = 0; cy < kSize.y; ++cy) {
                        uint32_t entry = 0;
                        if (cy < surface - 3) entry = 0x80000000u | 3u;
                        else if (cy <= surface) entry = rng.NextBounded(4) == 0 ? 0u : 1u + rng.NextBounded(8);
                        else if (cy == surface + 6 && rng.NextBounded(5) == 0) entry = 1u + rng.NextBounded(8);
                        if (halfLoaded && cy >= surface - 3 && cy <= surface + 2) entry = 0x80000000u;   // pending
                        const glm::ivec3 w = glm::ivec3(math::FloorMod(gridMin.x + cx, kSize.x), math::FloorMod(gridMin.y + cy, kSize.y),
                                                        math::FloorMod(gridMin.z + cz, kSize.z));
                        grid[static_cast<size_t>((w.x + w.z * kSize.x) * kSize.y + w.y)] = entry;
                        uint32_t& top = tops[static_cast<size_t>(w.x + w.z * kSize.x)];
                        if (entry != 0u) top = std::max(top, static_cast<uint32_t>(cy + 1));
                    }
                }
            for (uint32_t t : tops) contentTop = std::max(contentTop, t);
            // Region tops: per 4 x 4 columns of the window, window-relative.
            regions.assign(12 * 12, 0u);
            for (int cz = 0; cz < kSize.z; ++cz)
                for (int cx = 0; cx < kSize.x; ++cx) {
                    const glm::ivec2 w(math::FloorMod(gridMin.x + cx, kSize.x), math::FloorMod(gridMin.z + cz, kSize.z));
                    uint32_t& r = regions[static_cast<size_t>(cx / 4 + (cz / 4) * 12)];
                    r = std::max(r, tops[static_cast<size_t>(w.x + w.y * kSize.x)]);
                }
        }

        TraceScene Scene(bool skip, bool useRegions = true) const {
            TraceScene s;
            s.store = &store;
            s.grid = grid.data();
            s.gridSize = kSize;
            s.gridMinChunk = gridMin;
            s.voxelSize = 1.0f;
            if (skip) {
                s.columnTops = tops.data();
                s.contentTop = contentTop;
                if (useRegions) s.regionTops = regions.data();
            }
            return s;
        }

        /// Independent reference: a voxel-by-voxel DDA in double precision; a pending cell stops
        /// the ray like a uniform one (material 0).
        bool Reference(glm::dvec3 ro, glm::dvec3 rd, glm::ivec3& voxel, uint8_t& material) const {
            const glm::ivec3 extent = kSize * kChunkSize;
            double t0 = 0.0, t1 = 1.0e9;
            for (int a = 0; a < 3; ++a) {
                if (std::abs(rd[a]) < 1e-12) {
                    if (ro[a] < 0.0 || ro[a] > extent[a]) return false;
                    continue;
                }
                double ta = (0.0 - ro[a]) / rd[a], tb = (extent[a] - ro[a]) / rd[a];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
            }
            if (t0 > t1) return false;
            const glm::dvec3 p = ro + rd * (t0 + 1e-9);
            glm::ivec3 v = glm::clamp(glm::ivec3(glm::floor(p)), glm::ivec3(0), extent - glm::ivec3(1));
            const glm::ivec3 step(rd.x >= 0 ? 1 : -1, rd.y >= 0 ? 1 : -1, rd.z >= 0 ? 1 : -1);
            glm::dvec3 tNext, tDelta;
            for (int a = 0; a < 3; ++a) {
                tDelta[a] = std::abs(rd[a]) < 1e-12 ? 1e30 : std::abs(1.0 / rd[a]);
                const double boundary = step[a] > 0 ? v[a] + 1.0 : v[a];
                tNext[a] = std::abs(rd[a]) < 1e-12 ? 1e30 : (boundary - ro[a]) / rd[a];
            }
            for (int i = 0; i < 20000; ++i) {
                const glm::ivec3 cc = v / kChunkSize;
                const glm::ivec3 w = glm::ivec3(math::FloorMod(gridMin.x + cc.x, kSize.x), math::FloorMod(gridMin.y + cc.y, kSize.y),
                                                math::FloorMod(gridMin.z + cc.z, kSize.z));
                const uint32_t entry = grid[static_cast<size_t>((w.x + w.z * kSize.x) * kSize.y + w.y)];
                if (entry != 0u) {
                    if (entry & 0x80000000u) {
                        voxel = v;
                        material = static_cast<uint8_t>(entry & 0xFFu);
                        return true;
                    }
                    const glm::ivec3 l = v - cc * kChunkSize;
                    const uint8_t id = store.Get(entry - 1u).Get(l.x, l.y, l.z);
                    if (id != 0) {
                        voxel = v;
                        material = id;
                        return true;
                    }
                }
                const int a = (tNext.x < tNext.y) ? (tNext.x < tNext.z ? 0 : 2) : (tNext.y < tNext.z ? 1 : 2);
                v[a] += step[a];
                if (v[a] < 0 || v[a] >= extent[a]) return false;
                tNext[a] += tDelta[a];
            }
            return false;
        }
    };
}

// The column-top skip must not change what a ray hits: checked against the same traversal
// without the table and against a voxel DDA, on a 48 x 55 x 48 window with half-loaded columns.
TEST(ViewDistance, ColumnSkipMatchesTheTraversalWithoutIt) {
    const auto scene = std::make_unique<ColumnScene>();
    const TraceScene withSkip = scene->Scene(true), without = scene->Scene(false), columnsOnly = scene->Scene(true, false);
    const glm::vec3 extent = glm::vec3(ColumnScene::kSize * kChunkSize);
    Rng rng(7);
    int hits = 0, pendingHits = 0, mismatches = 0, refChecked = 0, refMismatches = 0;
    long stepsSkip = 0, stepsPlain = 0, stepsColumns = 0;
    for (int i = 0; i < 6000; ++i) {
        // From above the terrain (sky and sun rays), from inside, and from outside the window.
        glm::vec3 o(rng.Range(-200.0f, extent.x + 200.0f), rng.Range(0.0f, extent.y * 1.6f), rng.Range(-200.0f, extent.z + 200.0f));
        if (i % 3 == 0) o = glm::vec3(rng.Range(0.0f, extent.x), rng.Range(extent.y * 0.4f, extent.y), rng.Range(0.0f, extent.z));
        glm::vec3 d(rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f));
        if (i % 4 == 1) d.y = std::abs(d.y);            // rising rays
        if (i % 5 == 2) d = glm::vec3(d.x, -0.05f, d.z); // grazing
        if (glm::dot(d, d) < 1.0e-4f) continue;
        d = glm::normalize(d);
        const RayHit a = TraceWorld(withSkip, o, d, 0.0f, 1.0e9f);
        const RayHit b = TraceWorld(without, o, d, 0.0f, 1.0e9f);
        const RayHit c = TraceWorld(columnsOnly, o, d, 0.0f, 1.0e9f);
        stepsColumns += c.steps;
        if (c.hit != b.hit || (c.hit && (c.voxel != b.voxel || c.material != b.material))) ++mismatches;
        stepsSkip += a.steps;
        stepsPlain += b.steps;
        if (a.hit != b.hit || (a.hit && (a.voxel != b.voxel || a.material != b.material || std::abs(a.t - b.t) > 1.0e-2f * std::max(1.0f, b.t * 1.0e-3f)))) {
            ++mismatches;
            continue;
        }
        if (a.hit) {
            ++hits;
            if (a.material == 0) ++pendingHits;
        }
        if (i % 10 == 0) {
            glm::ivec3 rv(0);
            uint8_t rm = 0;
            const bool r = scene->Reference(glm::dvec3(o), glm::dvec3(d), rv, rm);
            ++refChecked;
            if (r != a.hit || (r && (rv + scene->gridMin * kChunkSize != a.voxel || rm != a.material))) ++refMismatches;
        }
    }
    std::printf("column skip: %d hits (%d pending), %d mismatches, steps per ray %.1f (regions) %.1f (columns) %.1f (none); "
                "reference %d of %d differ\n", hits, pendingHits, mismatches, static_cast<double>(stepsSkip) / 6000.0,
                static_cast<double>(stepsColumns) / 6000.0, static_cast<double>(stepsPlain) / 6000.0, refMismatches, refChecked);
    EXPECT_EQ(mismatches, 0);
    EXPECT_GT(hits, 1000);
    EXPECT_GT(pendingHits, 10);
    EXPECT_LT(stepsSkip, stepsColumns);
    EXPECT_LT(stepsColumns, stepsPlain);
    EXPECT_LE(refMismatches, refChecked / 100);       // ties on cell boundaries in float vs double
}

// The geometric rule: rays from the sky (down at every angle) hit Packed chunks only through cave
// mouths (accepted until the exact visibility stage); measured and bounded here.
TEST(ViewDistance, SkyRaysRarelyMeetPackedChunks) {
    for (const uint32_t seed : {1337u, 77u}) {
        TerrainWorld t(true, seed);
        World& w = *t.world;
        const glm::dvec3 focus(0.0, 20.0, 0.0);
        w.LoadBlocking(focus);
        int hits = 0, packed = 0;
        SkyRays(w, seed, hits, packed);
        std::printf("seed %u: %d sky rays hit the terrain, %d of them a Packed chunk (%.3f %%)\n", seed, hits, packed,
                    100.0 * packed / std::max(hits, 1));
        EXPECT_GT(hits, 1000);
        EXPECT_LE(packed * 200, hits);      // at most 0.5 %
        EXPECT_EQ(PackedButVisible(t), 0);
    }
}

// A chunk at the window's edge is judged without its neighbours outside the window. When the
// window moves on and they enter, its Packed chunks are judged again: after travelling, the rule
// holds as after a fill in one go, and the sky rays see no more Packed chunks than then.
TEST(ViewDistance, PackedChunksAreJudgedAgainWhenTheWindowMoves) {
    for (const uint32_t seed : {1337u, 77u}) {
        TerrainWorld t(true, seed);
        World& w = *t.world;
        glm::dvec3 focus(0.0, 20.0, 0.0);
        w.LoadBlocking(focus);
        // One chunk (16 m) per step: +x, then +z, then diagonally.
        for (int i = 0; i < 9; ++i) {
            focus += i < 3 ? glm::dvec3(16.0, 0.0, 0.0) : i < 6 ? glm::dvec3(0.0, 0.0, 16.0) : glm::dvec3(16.0, 0.0, 16.0);
            w.LoadBlocking(focus);
            EXPECT_EQ(PackedButVisible(t), 0) << "step " << i;
        }
        int hits = 0, packed = 0;
        SkyRays(w, seed, hits, packed);
        std::printf("seed %u after travel: %d sky rays hit the terrain, %d of them a Packed chunk, %u revealed\n", seed, hits, packed,
                    w.Stats().reveals);
        EXPECT_GT(hits, 1000);
        EXPECT_LE(packed * 200, hits);
    }
}

// A crater exposes the faces of its neighbours. After the crater's chunk was saved (the player
// walked away) and loads again, none of its face neighbours may be Packed, whichever loaded
// first: the saved chunk promotes neighbours that are already there, and a neighbour that
// arrives later sees the saved chunk next to it.
TEST(ViewDistance, EditedChunksNeverBorderPackedOnesAfterAReload) {
    TerrainWorld t(true);
    World& w = *t.world;
    const glm::dvec3 focus(20.0, 20.0, -10.0);
    w.LoadBlocking(focus);
    std::vector<glm::ivec3> packed;
    ForEachWindowChunk(w, [&](const glm::ivec3& c) {
        if (w.StateOf(c) == CellState::Packed) packed.push_back(c);
    });
    ASSERT_GE(packed.size(), 4u);
    std::vector<glm::ivec3> craters;
    for (size_t i = 0; i < 4; ++i) {
        const glm::ivec3 c = packed[i * packed.size() / 4];
        EditReport report;
        const glm::dvec3 centre = glm::dvec3(ChunkToVoxel(c)) + glm::dvec3(16.0);
        EXPECT_GT(w.CarveSphereQ(QuantizeSphere(centre, 10.0), QuantizePower(100.0f), nullptr, &report), 0u);
        craters.push_back(c);
    }
    const glm::ivec3 kFaces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int visit = 0; visit < 3; ++visit) {
        // Away (everything unloads, the craters are saved in memory) and back.
        w.LoadBlocking(focus + glm::dvec3(2000.0, 0.0, 0.0));
        w.LoadBlocking(focus);
        for (const glm::ivec3& c : craters) {
            EXPECT_NE(w.StateOf(c), CellState::Packed);
            for (const glm::ivec3& f : kFaces) EXPECT_NE(w.StateOf(c + f), CellState::Packed) << "visit " << visit;
        }
    }
    // Rays into the craters see their real walls.
    EXPECT_EQ(PackedButVisible(t), 0);
}

// Buried chunks decoded near the focus are packed again once the focus has moved on (with
// hysteresis): the store does not fill up over a session that stays in one area.
TEST(ViewDistance, ResidentBuriedChunksArePackedAgainBehindTheFocus) {
    TerrainWorld t(true);
    World& w = *t.world;
    const glm::dvec3 start(20.0, 20.0, -10.0);
    w.LoadBlocking(start);
    const uint32_t dense = w.Stats().denseChunks;
    const uint32_t used = t.store.UsedCount();
    // Down through the rock and back up, inside one column of chunks (no window move).
    uint32_t deepest = 0;
    for (int i = 0; i <= 40; ++i) {
        w.LoadBlocking(start - glm::dvec3(0.0, 2.0 * i, 0.0));
        deepest = std::max(deepest, w.Stats().denseChunks);
    }
    for (int i = 40; i >= 0; --i) w.LoadBlocking(start - glm::dvec3(0.0, 2.0 * i, 0.0));
    for (int i = 0; i < 50; ++i) w.Update(start);
    const WorldStats s = w.Stats();
    std::printf("walk down and back: %u dense at the start, %u at most, %u after; %u decodes, %u packed again\n", dense, deepest,
                s.denseChunks, s.decodes, s.repacks);
    EXPECT_GT(s.decodes, 0u);
    EXPECT_GT(s.repacks, 0u);
    EXPECT_LE(s.denseChunks, dense);
    EXPECT_LE(t.store.UsedCount(), used);
    // Packing again is not a content change.
    TerrainWorld plain(false);
    plain.world->LoadBlocking(start);
    EXPECT_EQ(plain.Hash().hash, t.Hash().hash);
}

// Everything within nearRadius of the focus is resident while the focus moves inside one chunk
// (16 m here): the near scan follows the focus, not chunk boundaries.
TEST(ViewDistance, EverythingNearTheFocusIsResidentWhileItMovesInsideAChunk) {
    TerrainWorld t(true);
    World& w = *t.world;
    const double r = w.Config().nearRadius;
    // Deep under the surface: everything around is buried.
    glm::dvec3 focus(16.5, -25.0, 16.5);
    w.LoadBlocking(focus);
    int checked = 0;
    for (int i = 0; i < 30; ++i) {
        focus = glm::dvec3(16.5 + 0.5 * i, -25.0 - 0.2 * i, 16.5 + 0.4 * i);
        w.Update(focus);
        const glm::ivec3 lo = VoxelToChunk(w.WorldToVoxel(focus - glm::dvec3(r))), hi = VoxelToChunk(w.WorldToVoxel(focus + glm::dvec3(r)));
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const glm::ivec3 c(x, y, z);
                    const glm::dvec3 min = glm::dvec3(ChunkToVoxel(c)) * static_cast<double>(w.VoxelSize());
                    const glm::dvec3 nearest = glm::clamp(focus, min, min + glm::dvec3(kChunkSize * static_cast<double>(w.VoxelSize())));
                    if (glm::length(focus - nearest) > r) continue;
                    ++checked;
                    EXPECT_NE(w.StateOf(c), CellState::Packed) << "step " << i << " chunk " << x << " " << y << " " << z;
                }
    }
    EXPECT_GT(checked, 30);
    EXPECT_EQ(VoxelToChunk(w.WorldToVoxel(focus)), VoxelToChunk(w.WorldToVoxel(glm::dvec3(16.5, -25.0, 16.5))));
}

// A full store never turns a loaded chunk into Empty: a saved edit is kept (Packed, drawn as
// rock), an edit there is refused as unsynced, and the save is not overwritten.
TEST(ViewDistance, AFullStoreNeverDropsASavedChunk) {
    TerrainWorld t(false);
    World& w = *t.world;
    const glm::dvec3 focus(20.0, 20.0, -10.0);
    w.LoadBlocking(focus);
    // An edit in the highest dense chunk of the focus column.
    const glm::ivec3 fc = VoxelToChunk(w.WorldToVoxel(focus));
    glm::ivec3 crater(fc.x, w.WindowMinChunk().y + w.WindowSize().y - 1, fc.z);
    while (crater.y > w.WindowMinChunk().y && !w.GetChunk(crater)) --crater.y;
    ASSERT_NE(w.GetChunk(crater), nullptr);
    EXPECT_GT(w.CarveSphereQ(QuantizeSphere(glm::dvec3(ChunkToVoxel(crater)) + glm::dvec3(16.0), 9.0), QuantizePower(100.0f)), 0u);
    auto edited = std::make_unique<Chunk>(), read = std::make_unique<Chunk>();
    ASSERT_TRUE(w.ReadChunk(crater, *edited));

    // Away (saved), then every free slot taken, then back.
    w.LoadBlocking(focus + glm::dvec3(2000.0, 0.0, 0.0));
    w.LoadBlocking(focus + glm::dvec3(4000.0, 0.0, 0.0));        // nothing of the first area is loaded
    w.Update(focus);            // the window jumps back: the slots of the far window are free, nothing is installed yet
    std::vector<uint32_t> taken;
    for (uint32_t slot; (slot = t.store.Alloc()) != ChunkStore::kInvalidSlot;) taken.push_back(slot);
    w.LoadBlocking(focus);
    EXPECT_GT(w.Stats().storeFullLoads, 0u);
    EXPECT_EQ(w.StateOf(crater), CellState::Packed);
    ASSERT_TRUE(w.ReadChunk(crater, *read));
    EXPECT_EQ(0, std::memcmp(edited->ids, read->ids, sizeof(read->ids)));
    // An edit there cannot be made resident: refused, nothing changes.
    EditReport report;
    EXPECT_EQ(w.FillSphereQ(QuantizeSphere(glm::dvec3(ChunkToVoxel(crater)) + glm::dvec3(16.0), 4.0), 5, nullptr, &report), 0u);
    EXPECT_EQ(report.unsynced, std::vector<glm::ivec3>{crater});

    // Slots back, away and back again: the save is still the edited chunk.
    for (const uint32_t slot : taken) t.store.Free(slot);
    w.LoadBlocking(focus + glm::dvec3(2000.0, 0.0, 0.0));
    w.LoadBlocking(focus + glm::dvec3(4000.0, 0.0, 0.0));
    w.LoadBlocking(focus);
    EXPECT_EQ(w.StateOf(crater), CellState::Dense);
    ASSERT_TRUE(w.ReadChunk(crater, *read));
    EXPECT_EQ(0, std::memcmp(edited->ids, read->ids, sizeof(read->ids)));
}

// RequestLoad: a box far from the focus (a blast's island search) is loaded before the rest of
// the window's surface.
TEST(ViewDistance, RequestedBoxesLoadBeforeTheRestOfTheWindow) {
    TerrainWorld t(true, 1337, [](WorldConfig& c) { c.maxJobsInFlight = 4; });
    World& w = *t.world;
    const glm::dvec3 focus(8.0, 20.0, 8.0);
    w.Update(focus);
    // The window's corner column: the farthest from the focus, so the last one by distance.
    const glm::ivec3 m = w.WindowMinChunk();
    const glm::ivec3 boxMin(m.x, m.y, m.z), boxMax(m.x, m.y + w.WindowSize().y - 1, m.z);
    w.RequestLoad(boxMin, boxMax);
    const auto boxLoaded = [&] {
        for (int y = boxMin.y; y <= boxMax.y; ++y)
            if (!w.IsChunkLoaded(glm::ivec3(boxMin.x, y, boxMin.z))) return false;
        return true;
    };
    int updates = 0;
    for (; updates < 5000 && !boxLoaded(); ++updates) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        w.Update(focus);
    }
    EXPECT_TRUE(boxLoaded());
    // Without the request the corner column comes last (its surface after every other column's
    // classification and surface): here most columns are not even classified yet.
    EXPECT_GT(w.Stats().columnsPending, 8u) << "the box should come before the rest of the window";
    std::printf("requested box loaded after %d updates, %u columns still unclassified\n", updates, w.Stats().columnsPending);
    w.LoadBlocking(focus);
    EXPECT_EQ(PackedButVisible(t), 0);
}

// --- island search (docs/design/shred.md stages 0-1) ---------------------------------------------

namespace {
    /// ExtractFloatingIslands as it was before the gather by chunk: one World::GetVoxel per voxel,
    /// then the same labelling and face rule. The reference the gather must equal.
    std::vector<FloatingIsland> IslandsPerVoxel(const World& world, const MaterialLibrary& materials, const glm::ivec3& lo,
                                                const glm::ivec3& hi, uint32_t maxVoxels, uint32_t& faceComponents) {
        const glm::ivec3 size = hi - lo;
        VoxelVolume local(size);
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x) {
                    const uint8_t id = world.GetVoxel(lo + glm::ivec3(x, y, z));
                    if (id != 0 && (materials.Get(kWorldPalette, id).flags & MaterialFlag_Liquid) == 0) local.Set(x, y, z, id);
                }
        const ComponentLabels labels = LabelComponents(local);
        std::vector<FloatingIsland> out;
        faceComponents = 0;
        for (uint32_t c = 1; c <= labels.count; ++c) {
            const glm::ivec3 mn = labels.boundsMin[c], mx = labels.boundsMax[c];
            const bool face = mn.x == 0 || mn.y == 0 || mn.z == 0 || mx.x == size.x || mx.y == size.y || mx.z == size.z;
            if (face && labels.voxelCounts[c] <= maxVoxels) ++faceComponents;
            if (face || labels.voxelCounts[c] > maxVoxels) continue;
            FloatingIsland island;
            island.origin = lo + mn;
            island.volume = VoxelVolume(mx - mn);
            island.voxelCount = labels.voxelCounts[c];
            for (int z = mn.z; z < mx.z; ++z)
                for (int y = mn.y; y < mx.y; ++y)
                    for (int x = mn.x; x < mx.x; ++x) {
                        if (labels.labels[local.Index(x, y, z)] == c) island.volume.Set(x - mn.x, y - mn.y, z - mn.z, local.Get(x, y, z));
                    }
            out.push_back(std::move(island));
        }
        return out;
    }
}

// Stage 0's speed-up must not change a single island: random boxes over real terrain with Dense,
// Uniform, Packed and Empty chunks (and water, which is ignored), with and without carved holes.
TEST(FloatingIslands, GatherByChunkEqualsPerVoxel) {
    uint32_t seen[6] = {};
    uint32_t islandsSeen = 0, faceSeen = 0;
    for (const bool pack : {true, false}) {
        TerrainWorld t(pack);
        World& w = *t.world;
        const glm::dvec3 focus(20.0, 20.0, -10.0);
        w.LoadBlocking(focus);
        if (pack) ASSERT_GT(w.Stats().packedChunks, 0u);
        std::mt19937 rng(pack ? 4242u : 777u);
        // Boxes inside the window, around the surface (rows near the terrain's height range).
        const glm::ivec3 wmin = ChunkToVoxel(w.WindowMinChunk()), wmax = wmin + w.WindowSize() * kChunkSize;
        std::uniform_int_distribution<int> extent(8, 64), radius(2, 9);
        std::uniform_int_distribution<int> axisX(wmin.x, wmax.x - 65), axisZ(wmin.z, wmax.z - 65);
        std::uniform_int_distribution<int> height(wmin.y, std::min(wmax.y - 65, 80));
        for (int trial = 0; trial < 40; ++trial) {
            const glm::ivec3 lo(axisX(rng), height(rng), axisZ(rng));
            const glm::ivec3 hi = lo + glm::ivec3(extent(rng), extent(rng), extent(rng));
            // Holes make floating pieces; every third trial searches untouched terrain (Packed stays packed).
            if (trial % 3 != 0) {
                for (int i = 0; i < 4; ++i) {
                    const glm::dvec3 c = glm::dvec3(lo) + glm::dvec3(hi - lo) * glm::dvec3(rng() % 100, rng() % 100, rng() % 100) / 100.0;
                    w.CarveSphereQ(QuantizeSphere(c, radius(rng)), QuantizePower(100.0f));
                }
            }
            const glm::ivec3 cmin = VoxelToChunk(lo), cmax = VoxelToChunk(hi - glm::ivec3(1));
            for (int z = cmin.z; z <= cmax.z; ++z)
                for (int y = cmin.y; y <= cmax.y; ++y)
                    for (int x = cmin.x; x <= cmax.x; ++x) ++seen[static_cast<int>(w.StateOf({x, y, z}))];
            uint32_t faceReference = 0;
            const std::vector<FloatingIsland> reference = IslandsPerVoxel(w, t.materials, lo, hi, 60000, faceReference);
            bool searched = false;
            IslandBoxScan scan;
            const std::vector<FloatingIsland> islands = ExtractFloatingIslands(w, &t.materials, lo, hi, 60000, false, MaterialFlag_Liquid, &searched, &scan);
            ASSERT_TRUE(searched) << trial;
            ASSERT_EQ(islands.size(), reference.size()) << trial;
            for (size_t i = 0; i < islands.size(); ++i) {
                EXPECT_EQ(islands[i].origin, reference[i].origin) << trial;
                EXPECT_EQ(islands[i].voxelCount, reference[i].voxelCount) << trial;
                EXPECT_EQ(islands[i].volume.Data(), reference[i].volume.Data()) << trial;
            }
            EXPECT_EQ(scan.faceComponents.size(), faceReference) << trial;
            EXPECT_EQ(scan.boxVoxels, static_cast<uint64_t>(hi.x - lo.x) * static_cast<uint64_t>(hi.y - lo.y) * static_cast<uint64_t>(hi.z - lo.z));
            for (const FaceComponent& f : scan.faceComponents) {
                EXPECT_NE(w.GetVoxel(f.firstVoxel), 0) << trial;
                EXPECT_EQ(scan.labels.labels[scan.labels.firstIndex[f.label]], f.label) << trial;
            }
            islandsSeen += static_cast<uint32_t>(islands.size());
            faceSeen += faceReference;
        }
    }
    std::printf("chunks seen: %u empty, %u uniform, %u dense, %u packed; %u islands, %u face components\n",
                seen[static_cast<int>(CellState::Empty)], seen[static_cast<int>(CellState::Uniform)], seen[static_cast<int>(CellState::Dense)],
                seen[static_cast<int>(CellState::Packed)], islandsSeen, faceSeen);
    EXPECT_GT(seen[static_cast<int>(CellState::Packed)], 0u);
    EXPECT_GT(seen[static_cast<int>(CellState::Uniform)], 0u);
    EXPECT_GT(seen[static_cast<int>(CellState::Dense)], 0u);
    EXPECT_GT(islandsSeen, 0u);
    EXPECT_GT(faceSeen, 0u);
}

// Above the build ceiling nothing can exist: an island box and a face flood that reach past it
// see air there, not "not loaded" (shred.md stage 1, rule 7).
TEST(FloatingIslands, RowsAboveTheBuildCeilingAreAir) {
    HillWorld full(true);
    World& w = *full.world;
    w.LoadBlocking(glm::dvec3(300.0, 1.7, 0.0));
    const int ceiling = (w.WindowMinChunk().y + w.WindowSize().y) * kChunkSize;     // 288 voxels = 144 m
    ASSERT_EQ(ceiling, 288);
    const glm::ivec3 at(600, 0, 0);
    // A floating blob right under the ceiling; the box reaches above it.
    w.FillBox(at + glm::ivec3(0, 280, 0), at + glm::ivec3(3, 283, 3), 1);
    bool searched = false;
    std::vector<FloatingIsland> islands = ExtractFloatingIslands(w, nullptr, at + glm::ivec3(-8, 270, -8), at + glm::ivec3(8, 300, 8), 1000, false,
                                                                 MaterialFlag_Liquid, &searched);
    EXPECT_TRUE(searched);
    ASSERT_EQ(islands.size(), 1u);
    EXPECT_EQ(islands[0].voxelCount, 27u);

    // A piece touching the ceiling row, cut by the island box's top face: the flood closes on air.
    w.FillBox(at + glm::ivec3(20, 284, 0), at + glm::ivec3(22, 288, 2), 1);
    const glm::ivec3 lo = at + glm::ivec3(10, 270, -10), hi = at + glm::ivec3(30, 286, 10);
    IslandBoxScan scan;
    islands = ExtractFloatingIslands(w, nullptr, lo, hi, 1000, false, MaterialFlag_Liquid, &searched, &scan);
    EXPECT_TRUE(searched);
    EXPECT_TRUE(islands.empty());
    ASSERT_EQ(scan.faceComponents.size(), 1u);
    FloodLimits limits;
    limits.floodMin = at + glm::ivec3(-76, 182, -86);
    limits.floodMax = at + glm::ivec3(116, 374, 106);
    limits.maxVoxels = 1000;
    limits.maxVisitsPerSearch = 4000;
    const FloodResult flood = ProveFaceComponents(w, nullptr, scan.faceComponents, lo, hi, scan.labels, MaterialFlag_Liquid, limits);
    EXPECT_TRUE(flood.complete);
    ASSERT_EQ(flood.islands.size(), 1u);
    EXPECT_EQ(flood.islands[0].voxelCount, 16u);
    EXPECT_EQ(flood.islands[0].origin, at + glm::ivec3(20, 284, 0));
}

// Below the bottom row of a full-height window nothing is ever loaded: those rows are the world's
// ground. A box reaching below it is searched over the window's rows (its bottom face is the
// window's), never refused as "not loaded".
TEST(FloatingIslands, RowsBelowTheBottomOfAFullHeightWindowAreGround) {
    HillWorld full(true);
    World& w = *full.world;
    w.LoadBlocking(glm::dvec3(300.0, 1.7, 0.0));
    const int bottom = w.WindowMinChunk().y * kChunkSize;
    const glm::ivec3 at(600, 0, 0);
    bool searched = false;
    IslandBoxScan scan;
    const std::vector<FloatingIsland> islands = ExtractFloatingIslands(w, nullptr, at + glm::ivec3(-8, bottom - 10, -8),
                                                                       at + glm::ivec3(8, bottom + 20, 8), 100000, false,
                                                                       MaterialFlag_Liquid, &searched, &scan);
    EXPECT_TRUE(searched);
    EXPECT_TRUE(islands.empty());
    EXPECT_EQ(scan.boxMin, at + glm::ivec3(-8, bottom, -8));
    EXPECT_EQ(scan.boxMax, at + glm::ivec3(8, bottom + 20, 8));
    EXPECT_EQ(scan.boxVoxels, 16u * 20u * 16u);

    // Entirely below the window: nothing can be there, searched and empty.
    searched = false;
    EXPECT_TRUE(ExtractFloatingIslands(w, nullptr, at + glm::ivec3(0, bottom - 40, 0), at + glm::ivec3(8, bottom - 8, 8), 1000, false,
                                       MaterialFlag_Liquid, &searched).empty());
    EXPECT_TRUE(searched);
}
