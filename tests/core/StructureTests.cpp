// docs/design/shred.md stage 4: the cell graph cached per terrain chunk (voxel:structure) and the
// island search on it, against LabelComponents and the stage-1 voxel search (ExtractFloatingIslands
// + ProveFaceComponents) that it replaces in sim::Blast.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    constexpr uint8_t kStone = 1, kPlanks = 2, kWater = 3, kIron = 4;

    /// Hand-made tables: water ignored, distinct densities so masses can be checked.
    GraphMaterials TestMaterials() {
        GraphMaterials m;
        for (uint32_t id = 1; id < 256; ++id) m.densityQ[id] = 1000;
        m.densityQ[kStone] = 2600;
        m.densityQ[kPlanks] = 600;
        m.densityQ[kIron] = 7800;
        m.ignored[kWater] = true;
        m.densityQ[kWater] = 0;
        return m;
    }

    /// A block of chunks filled from a volume (no World): what the graph of each chunk is built from.
    struct ChunkBlock {
        explicit ChunkBlock(const glm::ivec3& chunks) : count(chunks), volume(chunks * kChunkSize) {
            data.resize(static_cast<size_t>(chunks.x * chunks.y * chunks.z));
        }
        size_t Index(const glm::ivec3& c) const { return static_cast<size_t>(c.x + count.x * (c.y + count.y * c.z)); }
        void Bake() {
            for (int cz = 0; cz < count.z; ++cz)
                for (int cy = 0; cy < count.y; ++cy)
                    for (int cx = 0; cx < count.x; ++cx) {
                        Chunk& chunk = data[Index({cx, cy, cz})];
                        chunk.Clear();
                        for (int z = 0; z < kChunkSize; ++z)
                            for (int y = 0; y < kChunkSize; ++y)
                                for (int x = 0; x < kChunkSize; ++x)
                                    chunk.ids[Chunk::VoxelIndex(x, y, z)] = volume.Get(glm::ivec3(cx, cy, cz) * kChunkSize + glm::ivec3(x, y, z));
                        chunk.RebuildOccupancy();
                    }
        }
        glm::ivec3 count;
        VoxelVolume volume;
        std::vector<Chunk> data;
    };

    /// Per voxel of the block: the graph component (union-find over every node of every chunk
    /// graph, links inside chunks and across chunk faces), 0 for air and ignored voxels.
    struct GraphComponents {
        std::vector<uint32_t> ofVoxel;
        uint32_t overfullCells = 0;
        uint32_t nodes = 0;
    };

    GraphComponents ComponentsOnGraph(const ChunkBlock& block, const GraphMaterials& materials) {
        std::vector<ChunkGraph> graphs(block.data.size());
        std::vector<uint32_t> base(block.data.size() + 1, 0);
        for (size_t i = 0; i < block.data.size(); ++i) {
            const glm::ivec3 c(static_cast<int>(i) % block.count.x, (static_cast<int>(i) / block.count.x) % block.count.y,
                               static_cast<int>(i) / (block.count.x * block.count.y));
            BuildChunkGraph(graphs[i], c, &block.data[i], 0, materials);
            base[i + 1] = base[i] + static_cast<uint32_t>(graphs[i].nodes.size());
        }
        std::vector<uint32_t> parent(base.back());
        std::iota(parent.begin(), parent.end(), 0u);
        const auto Find = [&parent](uint32_t i) {
            while (parent[i] != i) i = parent[i] = parent[parent[i]];
            return i;
        };
        const auto Union = [&](uint32_t a, uint32_t b) {
            a = Find(a);
            b = Find(b);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        };
        GraphComponents out;
        out.ofVoxel.assign(block.volume.Data().size(), 0u);
        for (size_t i = 0; i < graphs.size(); ++i) {
            const ChunkGraph& g = graphs[i];
            out.overfullCells += g.overfullCells;
            out.nodes += static_cast<uint32_t>(g.nodes.size());
            for (uint32_t n = 0; n < g.nodes.size(); ++n) {
                const GraphNode& node = g.nodes[n];
                for (uint32_t k = node.firstLink; k < node.firstLink + node.linkCount; ++k) {
                    // Links carry the bond the face masks give, are sorted by (face, node) and
                    // symmetric: the search follows them from the node it expands only.
                    const GraphLink& link = g.links[k];
                    EXPECT_EQ(link.bond, BondVoxels(node, g.nodes[link.node], link.face));
                    if (k > node.firstLink) {
                        const GraphLink& prev = g.links[k - 1];
                        EXPECT_TRUE(prev.face < link.face || (prev.face == link.face && prev.node < link.node)) << "links out of order";
                    }
                    const GraphNode& other = g.nodes[link.node];
                    uint32_t back = 0;
                    for (uint32_t j = other.firstLink; j < other.firstLink + other.linkCount; ++j)
                        back += (g.links[j].node == n && g.links[j].face == (link.face ^ 1u) && g.links[j].bond == link.bond) ? 1u : 0u;
                    EXPECT_EQ(back, 1u) << "a link without its reverse";
                    Union(base[i] + n, base[i] + link.node);
                }
                const glm::ivec3 cell(node.cell & 3, (node.cell >> 2) & 3, node.cell >> 4);
                for (uint8_t f = 1; f < 6; f += 2) {            // + faces at the chunk face: the neighbour chunk
                    const int axis = f >> 1;
                    if (cell[axis] != kCellsPerChunkAxis - 1 || node.face[f] == 0) continue;
                    glm::ivec3 next = g.chunk;
                    next[axis] += 1;
                    if (next[axis] >= block.count[axis]) continue;
                    glm::ivec3 across = cell;
                    across[axis] = 0;
                    const uint32_t ac = static_cast<uint32_t>(across.x + 4 * (across.y + 4 * across.z));
                    const size_t j = block.Index(next);
                    for (uint32_t m = graphs[j].cellFirst[ac]; m < graphs[j].cellFirst[ac + 1]; ++m) {
                        if (BondVoxels(node, graphs[j].nodes[m], f) != 0) Union(base[i] + n, base[j] + m);
                    }
                }
                // Each node's voxels, re-labelled from the chunk: count, first voxel and mass agree.
                const std::array<uint64_t, 8> mask = NodeVoxels(&block.data[i], 0, node, materials);
                uint32_t count = 0, mass = 0;
                int first = -1;
                const glm::ivec3 o = g.chunk * kChunkSize + cell * kCellSize;
                for (int z = 0; z < 8; ++z)
                    for (uint64_t bits = mask[static_cast<size_t>(z)]; bits != 0; bits &= bits - 1) {
                        const int b = std::countr_zero(bits);
                        if (first < 0) first = b + 64 * z;
                        const glm::ivec3 v = o + glm::ivec3(b & 7, b >> 3, z);
                        const size_t index = block.volume.Index(v.x, v.y, v.z);
                        EXPECT_EQ(out.ofVoxel[index], 0u) << "a voxel in two nodes";
                        out.ofVoxel[index] = base[i] + n + 1;
                        mass += materials.densityQ[block.volume.Get(v)];
                        ++count;
                    }
                EXPECT_EQ(count, node.voxels);
                EXPECT_EQ(first, node.firstVoxel);
                EXPECT_EQ(mass, node.mass);
            }
        }
        for (uint32_t& c : out.ofVoxel)
            if (c != 0) c = Find(c - 1) + 1;
        return out;
    }

    /// The graph's components equal LabelComponents' on the same voxels (ignored ones are air):
    /// the same partition of the solid voxels.
    void ExpectSamePartition(const ChunkBlock& block, const GraphMaterials& materials, const GraphComponents& graph) {
        VoxelVolume solid = block.volume;
        for (uint8_t& id : solid.Data()) id = (id != 0 && !materials.ignored[id]) ? 1 : 0;
        const ComponentLabels labels = LabelComponents(solid);
        std::unordered_map<uint32_t, uint32_t> labelOf, componentOf;
        uint32_t mismatches = 0;
        for (size_t i = 0; i < solid.Data().size(); ++i) {
            const uint32_t l = labels.labels[i], c = graph.ofVoxel[i];
            if ((l == 0) != (c == 0)) {
                ++mismatches;
                continue;
            }
            if (l == 0) continue;
            const auto [a, newA] = labelOf.emplace(c, l);
            const auto [b, newB] = componentOf.emplace(l, c);
            if (a->second != l || b->second != c) ++mismatches;
        }
        EXPECT_EQ(mismatches, 0u);
        EXPECT_EQ(labelOf.size(), labels.count);
    }

    /// Random axis-aligned boxes of stone, planks, iron and water, at least 2 voxels thick.
    void RandomBoxes(VoxelVolume& v, std::mt19937& rng, int count) {
        std::uniform_int_distribution<int> pos(0, v.Size().x - 1), len(2, 20), mat(1, 4);
        for (int i = 0; i < count; ++i) {
            const glm::ivec3 lo(pos(rng), pos(rng), pos(rng));
            const glm::ivec3 hi = glm::min(lo + glm::ivec3(len(rng), len(rng), len(rng)), v.Size());
            v.FillBox(lo, hi, static_cast<uint8_t>(mat(rng)));
        }
    }

    // --- worlds -----------------------------------------------------------------------------------

    /// The built-in terrain at 0.5 m (Dense, Uniform, Packed and Empty chunks and water), full height.
    struct TerrainWorld {
        explicit TerrainWorld(bool pack) : store(4096), jobs(3) {
            WorldConfig config;
            config.voxelSize = 0.5f;
            config.windowSize = {8, 4, 8};
            config.fullHeight = true;
            config.packHidden = pack;
            config.nearRadius = 4.0f;
            TerrainSettings terrain;
            terrain.seed = 1337;
            terrain.voxelSize = 0.5f;
            terrain.materials = ids = RegisterDefaultWorldMaterials(materials);
            world = std::make_unique<World>(config, store, jobs, CreateTerrainGenerator(terrain), &materials);
        }
        ChunkStore store;
        JobSystem jobs;
        MaterialLibrary materials;
        WorldMaterials ids;
        std::unique_ptr<World> world;
    };

    /// Flat ground (voxels y < 80) at 0.1 m, window 6 x 6 x 6 chunks.
    struct FlatWorld {
        explicit FlatWorld(unsigned threads = 3) : store(1024), jobs(threads) {
            WorldConfig config;
            config.voxelSize = 0.1f;
            config.windowSize = {6, 6, 6};
            ids = RegisterDefaultWorldMaterials(materials);
            world = std::make_unique<World>(config, store, jobs, CreateFlatGenerator(80, ids.grass, ids.stone), &materials);
            world->LoadBlocking(glm::dvec3(0.0, 8.0, 0.0));
        }
        ChunkStore store;
        JobSystem jobs;
        MaterialLibrary materials;
        WorldMaterials ids;
        std::unique_ptr<World> world;
    };

    /// sim::IslandBox / sim::FloodBox (not reachable from VortexCoreTests): +-(r/16 + 24), +-96.
    struct SearchBoxes {
        glm::ivec3 boxMin, boxMax, floodMin, floodMax;
    };
    SearchBoxes BoxesOf(const SphereQ& sphere) {
        const glm::ivec3 c = math::FloorDiv(sphere.center, kQuantaPerVoxel);
        const glm::ivec3 half(static_cast<int>(sphere.radius / kQuantaPerVoxel) + 24);
        return {c - half, c + half, c - glm::ivec3(kFloodHalfExtent), c + glm::ivec3(kFloodHalfExtent)};
    }

    struct Islands {
        bool searched = false, complete = true;
        std::vector<FloatingIsland> box, face;
        uint32_t budgetSpent = 0;
    };

    /// The stage-1 search, as sim::Blast ran it.
    Islands VoxelSearch(World& world, const MaterialLibrary& materials, const SearchBoxes& b) {
        Islands out;
        IslandBoxScan scan;
        out.box = ExtractFloatingIslands(world, &materials, b.boxMin, b.boxMax, 60000, false, MaterialFlag_Liquid, &out.searched, &scan);
        if (!out.searched) return out;
        FloodLimits limits;
        limits.floodMin = b.floodMin;
        limits.floodMax = b.floodMax;
        limits.maxVoxels = 60000;
        limits.maxVisitsPerSearch = 240000;
        FloodResult flood = ProveFaceComponents(world, &materials, scan.faceComponents, scan.boxMin, scan.boxMax, scan.labels, MaterialFlag_Liquid, limits);
        out.complete = flood.complete;
        out.budgetSpent = flood.attachedByBudget;
        if (flood.complete) out.face = std::move(flood.islands);
        return out;
    }

    Islands GraphSearch(const World& world, TerrainGraphCache& cache, const MaterialLibrary& materials, const SearchBoxes& b,
                        uint32_t budget = 32768, GraphSearchResult* raw = nullptr) {
        GraphSearchLimits limits;
        limits.boxMin = b.boxMin;
        limits.boxMax = b.boxMax;
        limits.floodMin = b.floodMin;
        limits.floodMax = b.floodMax;
        limits.maxIslandVoxels = 60000;
        limits.maxNodeVisits = budget;
        GraphSearchResult r = SearchIslandsOnGraph(world, cache, MakeGraphMaterials(&materials, MaterialFlag_Liquid), limits);
        EXPECT_EQ(r.relabelMismatch, 0u);
        Islands out;
        out.searched = r.searched;
        out.complete = r.complete;
        out.budgetSpent = r.attachedByBudget;
        out.box = r.boxIslands;
        out.face = r.faceIslands;
        if (raw) *raw = std::move(r);
        return out;
    }

    bool SameIslands(const std::vector<FloatingIsland>& a, const std::vector<FloatingIsland>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].origin != b[i].origin || a[i].voxelCount != b[i].voxelCount || a[i].volume.Data() != b[i].volume.Data()) return false;
        return true;
    }

    uint64_t VoxelKey(const glm::ivec3& v) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(v.x) & 0x1FFFFFu)) | (static_cast<uint64_t>(static_cast<uint32_t>(v.y) & 0x1FFFFFu) << 21) |
               (static_cast<uint64_t>(static_cast<uint32_t>(v.z) & 0x1FFFFFu) << 42);
    }
    void AddVoxels(const std::vector<FloatingIsland>& islands, std::unordered_set<uint64_t>& out) {
        for (const FloatingIsland& island : islands) {
            const glm::ivec3 s = island.volume.Size();
            for (int z = 0; z < s.z; ++z)
                for (int y = 0; y < s.y; ++y)
                    for (int x = 0; x < s.x; ++x)
                        if (island.volume.Get(x, y, z) != 0) out.insert(VoxelKey(island.origin + glm::ivec3(x, y, z)));
        }
    }

    bool SameGraph(const ChunkGraph& a, const ChunkGraph& b) {
        if (a.chunk != b.chunk || a.nodes.size() != b.nodes.size() || a.links.size() != b.links.size() || a.cellFirst != b.cellFirst ||
            a.overfullCells != b.overfullCells || a.solidVoxels != b.solidVoxels)
            return false;
        for (size_t i = 0; i < a.nodes.size(); ++i) {
            const GraphNode& x = a.nodes[i];
            const GraphNode& y = b.nodes[i];
            for (int f = 0; f < 6; ++f)
                if (x.face[f] != y.face[f]) return false;
            if (x.mass != y.mass || x.voxels != y.voxels || x.firstVoxel != y.firstVoxel || x.firstLink != y.firstLink ||
                x.linkCount != y.linkCount || x.cell != y.cell)
                return false;
            for (int k = 0; k < 3; ++k)
                if (x.boxMin[k] != y.boxMin[k] || x.boxMax[k] != y.boxMax[k]) return false;
        }
        for (size_t i = 0; i < a.links.size(); ++i)
            if (a.links[i].node != b.links[i].node || a.links[i].face != b.links[i].face || a.links[i].bond != b.links[i].bond) return false;
        return true;
    }
}

// --- the graph of a chunk -------------------------------------------------------------------------

// collapse.md's claim, made exact: graph connectivity equals voxel 6-connectivity. Random walls,
// pillars and slabs (no cell holds more than 8 components), liquids ignored, over 2 x 2 x 2 chunks.
TEST(Structure, GraphEqualsLabelComponentsWithoutOverfullCells) {
    const GraphMaterials materials = TestMaterials();
    uint32_t nodes = 0;
    for (uint32_t seed = 1; seed <= 12; ++seed) {
        std::mt19937 rng(seed);
        ChunkBlock block({2, 2, 2});
        RandomBoxes(block.volume, rng, 6 + static_cast<int>(seed) * 3);
        block.Bake();
        const GraphComponents graph = ComponentsOnGraph(block, materials);
        EXPECT_EQ(graph.overfullCells, 0u) << seed;
        ExpectSamePartition(block, materials, graph);
        nodes += graph.nodes;
    }
    EXPECT_GT(nodes, 500u);
}

// Over-full cells (more than kMaxCellNodes components: collapse.md merged them into node 7 and
// over-connected) are exact here: every component is its own node.
TEST(Structure, GraphEqualsLabelComponentsInOverfullCells) {
    const GraphMaterials materials = TestMaterials();
    uint32_t overfull = 0;
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        std::mt19937 rng(100 + seed);
        ChunkBlock block({2, 2, 2});
        // Noise from sparse (many single voxels) to dense (one big component with holes), water mixed in.
        std::bernoulli_distribution solid(0.15 + 0.12 * seed);
        std::uniform_int_distribution<int> mat(1, 4);
        for (uint8_t& id : block.volume.Data()) id = solid(rng) ? static_cast<uint8_t>(mat(rng)) : uint8_t{0};
        block.Bake();
        const GraphComponents graph = ComponentsOnGraph(block, materials);
        ExpectSamePartition(block, materials, graph);
        overfull += graph.overfullCells;
    }
    EXPECT_GT(overfull, 100u);
}

TEST(Structure, UniformAndEmptyChunks) {
    const GraphMaterials materials = TestMaterials();
    ChunkGraph g;
    BuildChunkGraph(g, {0, 0, 0}, nullptr, kStone, materials);
    ASSERT_EQ(g.nodes.size(), 64u);
    EXPECT_EQ(g.solidVoxels, 32768u);
    EXPECT_EQ(g.nodes[0].mass, 512u * 2600u);
    EXPECT_EQ(g.links.size(), 2u * 3u * 48u);          // 48 cell pairs per axis, both directions
    for (const GraphLink& link : g.links) EXPECT_EQ(link.bond, 64u);
    BuildChunkGraph(g, {0, 0, 0}, nullptr, kWater, materials);
    EXPECT_TRUE(g.nodes.empty());                       // a uniform liquid chunk holds nothing
    BuildChunkGraph(g, {0, 0, 0}, nullptr, 0, materials);
    EXPECT_TRUE(g.nodes.empty());
    // A Dense chunk equal to the uniform one has the same graph.
    Chunk full;
    full.Fill(kStone);
    ChunkGraph dense;
    BuildChunkGraph(g, {0, 0, 0}, nullptr, kStone, materials);
    BuildChunkGraph(dense, {0, 0, 0}, &full, 0, materials);
    EXPECT_TRUE(SameGraph(g, dense));
}

// The forward requirement (shred.md stage 7, owner: "a whole tower on a 4-voxel bridge must not
// hold"): a node knows its voxels and mass, a link its bond, without rebuilding anything.
TEST(Structure, NodesCarryMassAndLinksTheirBond) {
    const GraphMaterials materials = TestMaterials();
    Chunk chunk;
    // A 2 x 2 neck of iron (y 0..7) under an 8 x 8 x 8 stone block (y 8..15), cell column (0, *, 0).
    for (int y = 0; y < 8; ++y)
        for (int z = 3; z < 5; ++z)
            for (int x = 3; x < 5; ++x) chunk.ids[Chunk::VoxelIndex(x, y, z)] = kIron;
    for (int y = 8; y < 16; ++y)
        for (int z = 0; z < 8; ++z)
            for (int x = 0; x < 8; ++x) chunk.ids[Chunk::VoxelIndex(x, y, z)] = kStone;
    chunk.RebuildOccupancy();
    ChunkGraph g;
    BuildChunkGraph(g, {0, 0, 0}, &chunk, 0, materials);
    ASSERT_EQ(g.nodes.size(), 2u);
    const GraphNode& neck = g.nodes[0];
    const GraphNode& block = g.nodes[1];
    EXPECT_EQ(neck.cell, 0u);
    EXPECT_EQ(block.cell, 4u);
    EXPECT_EQ(neck.voxels, 32u);
    EXPECT_EQ(neck.mass, 32u * 7800u);
    EXPECT_EQ(block.voxels, 512u);
    EXPECT_EQ(block.mass, 512u * 2600u);
    ASSERT_EQ(neck.linkCount, 1u);
    EXPECT_EQ(g.links[neck.firstLink].node, 1u);
    EXPECT_EQ(g.links[neck.firstLink].face, kFacePosY);
    EXPECT_EQ(g.links[neck.firstLink].bond, 4u);       // the 2 x 2 neck
    EXPECT_EQ(BondVoxels(block, neck, kFaceNegY), 4u);
    EXPECT_EQ(neck.boxMin[0], 3u);
    EXPECT_EQ(neck.boxMax[0], 4u);
    EXPECT_EQ(neck.boxMin[1], 0u);
    EXPECT_EQ(neck.boxMax[1], 7u);
}

// --- the cache --------------------------------------------------------------------------------------

TEST(Structure, ChunkRevisionChangesWithEveryContentChange) {
    FlatWorld f;
    World& w = *f.world;
    const glm::ivec3 chunk(0, 2, 0);                   // the surface row (y 64..95)
    const uint64_t r0 = w.ChunkRevision(chunk);
    EXPECT_GT(r0, 0u);
    EXPECT_EQ(w.ChunkRevision(chunk), r0);
    EXPECT_EQ(w.ChunkRevision({0, 40, 0}), 0u);        // outside the window: not loaded
    EXPECT_TRUE(w.SetVoxel({1, 85, 1}, f.ids.stone));
    const uint64_t r1 = w.ChunkRevision(chunk);
    EXPECT_NE(r1, r0);
    EXPECT_EQ(w.FillBox({0, 85, 0}, {1, 86, 1}, f.ids.stone), 1u);
    EXPECT_NE(w.ChunkRevision(chunk), r1);
    const uint64_t r2 = w.ChunkRevision(chunk);
    EXPECT_EQ(w.FillBox({0, 85, 0}, {1, 86, 1}, f.ids.stone), 0u);  // nothing changed: same revision
    EXPECT_EQ(w.ChunkRevision(chunk), r2);
    // Leaving the window and coming back is a new revision too.
    w.LoadBlocking(glm::dvec3(400.0, 8.0, 0.0));
    EXPECT_EQ(w.ChunkRevision(chunk), 0u);
    w.LoadBlocking(glm::dvec3(0.0, 8.0, 0.0));
    EXPECT_GT(w.ChunkRevision(chunk), r2);
}

// A cache kept across edits gives the graphs a fresh build gives, and rebuilds only what changed:
// random carves, fills, liquids, ClearMasks over real terrain with Packed chunks.
TEST(Structure, RefreshEqualsRebuild) {
    for (const bool pack : {true, false}) {
        TerrainWorld t(pack);
        World& w = *t.world;
        w.LoadBlocking(glm::dvec3(20.0, 20.0, -10.0));
        if (pack) ASSERT_GT(w.Stats().packedChunks, 0u);
        const GraphMaterials materials = MakeGraphMaterials(&t.materials, MaterialFlag_Liquid);
        TerrainGraphCache kept;
        const glm::ivec3 cmin = w.WindowMinChunk() + glm::ivec3(2, 0, 2), cmax = cmin + glm::ivec3(3, w.WindowSize().y - 1, 3);
        const auto ForEach = [&](auto&& fn) {
            for (int z = cmin.z; z <= cmax.z; ++z)
                for (int y = cmin.y; y <= cmax.y; ++y)
                    for (int x = cmin.x; x <= cmax.x; ++x) fn(glm::ivec3(x, y, z));
        };
        kept.BeginPass(w, materials);
        ForEach([&](const glm::ivec3& c) { ASSERT_NE(kept.Get(w, c), nullptr); });
        (void)w.TakeChangedChunks();
        std::mt19937 rng(pack ? 99u : 7u);
        const glm::ivec3 lo = ChunkToVoxel(cmin), hi = ChunkToVoxel(cmax + glm::ivec3(1));
        std::uniform_int_distribution<int> px(lo.x, hi.x - 1), py(lo.y, hi.y - 1), pz(lo.z, hi.z - 1), r(2, 10);
        uint32_t rebuiltSeen = 0;
        for (int round = 0; round < 6; ++round) {
            for (int i = 0; i < 3; ++i) {
                const glm::ivec3 c(px(rng), py(rng), pz(rng));
                switch ((round + i) % 4) {
                    case 0: w.CarveSphereQ(QuantizeSphere(glm::dvec3(c), r(rng)), QuantizePower(100.0f)); break;
                    case 1: w.FillBox(c, c + glm::ivec3(r(rng), r(rng), 2), t.ids.planks); break;
                    case 2: w.FillBox(c, c + glm::ivec3(r(rng), 3, r(rng)), t.ids.water); break;
                    default: {
                        VoxelMask mask(glm::ivec3(6));
                        for (int k = 0; k < 40; ++k) mask.Set(glm::ivec3(static_cast<int>(rng() % 6), static_cast<int>(rng() % 6), static_cast<int>(rng() % 6)), true);
                        w.ClearMask(c, mask);
                        break;
                    }
                }
            }
            std::unordered_set<uint64_t> changed;
            for (const glm::ivec3& c : w.TakeChangedChunks()) changed.insert(VoxelKey(c));
            const uint64_t builtBefore = kept.Stats().built;
            kept.BeginPass(w, materials);
            uint32_t expected = 0;
            TerrainGraphCache fresh;
            fresh.BeginPass(w, materials);
            ForEach([&](const glm::ivec3& c) {
                expected += changed.count(VoxelKey(c)) ? 1u : 0u;
                const ChunkGraph* a = kept.Get(w, c);
                const ChunkGraph* b = fresh.Get(w, c);
                ASSERT_TRUE(a != nullptr && b != nullptr);
                EXPECT_TRUE(SameGraph(*a, *b)) << c.x << ", " << c.y << ", " << c.z << " round " << round;
            });
            EXPECT_EQ(kept.Stats().built - builtBefore, expected) << "round " << round;
            rebuiltSeen += expected;
        }
        EXPECT_GT(rebuiltSeen, 6u);
        EXPECT_GT(kept.Stats().reused, 0u);
    }
}

// --- the search -------------------------------------------------------------------------------------

// The acceptance test of shred.md stage 4: on random volumes over real terrain (Dense, Uniform,
// Packed, Empty chunks, water from the generator and placed liquids, rows below and above a
// full-height window), with a blast cutting each structure, the graph search never detaches a voxel
// the voxel search keeps - and finds exactly its islands in its order whenever neither ran out of budget.
TEST(Structure, GraphNeverDetachesWhatTheVoxelSearchKeeps) {
    uint32_t compared = 0, islands = 0, faceIslands = 0, overfull = 0, budgetTrials = 0;
    for (const bool pack : {true, false}) {
        TerrainWorld t(pack);
        World& w = *t.world;
        w.LoadBlocking(glm::dvec3(20.0, 20.0, -10.0));
        TerrainGraphCache cache;
        const glm::ivec3 mid = ChunkToVoxel(w.WindowMinChunk()) + w.WindowSize() * (kChunkSize / 2);
        const int top = ChunkToVoxel(w.WindowMinChunk()).y + w.WindowSize().y * kChunkSize;
        std::mt19937 rng(pack ? 2024u : 11u);
        std::uniform_int_distribution<int> off(-28, 28), height(6, 70), radius(2, 7);
        for (int trial = 0; trial < 40; ++trial) {
            // A structure on the ground at a random column near the middle (the flood box stays in the window).
            const glm::ivec3 col(mid.x + off(rng), 0, mid.z + off(rng));
            int ground = top - 1;
            while (ground > ChunkToVoxel(w.WindowMinChunk()).y && (w.GetVoxel({col.x, ground, col.z}) == 0 ||
                                                                   w.GetVoxel({col.x, ground, col.z}) == t.ids.water))
                --ground;
            const int h = std::min(height(rng), top - ground - 12);
            if (h < 4) continue;
            const glm::ivec3 base(col.x, ground + 1, col.z);
            const int kind = trial % 4;
            const uint8_t mat = (trial % 3 == 0) ? t.ids.planks : t.ids.stone;
            w.FillBox(base, base + glm::ivec3(3, h, 3), mat);
            if (kind == 1) w.FillBox(base + glm::ivec3(3, h - 3, 0), base + glm::ivec3(3 + 40, h, 3), t.ids.wood);          // an arm
            if (kind == 2) w.FillBox(base + glm::ivec3(-6, h, -6), base + glm::ivec3(9, h + 2, 9), t.ids.brick);           // a roof
            if (kind == 3) {
                // Noise on top (many tiny components: over-full cells) and water around the pillar.
                for (int k = 0; k < 400; ++k)
                    w.SetVoxel(base + glm::ivec3(static_cast<int>(rng() % 10) - 3, h + static_cast<int>(rng() % 8), static_cast<int>(rng() % 10) - 3),
                               (rng() & 1) ? t.ids.stone : t.ids.water);
            }
            if (trial % 5 == 4) w.FillBox(base + glm::ivec3(-4, 2, -4), base + glm::ivec3(7, 5, 7), t.ids.water);
            // The blast: carve the base (or high up, power 0) as sim::Blast does, then search.
            const glm::dvec3 at(base.x + 1.5, base.y + (trial % 6 == 5 ? h / 2 : 2) + 0.5, base.z + 1.5);
            const SphereQ sphere = QuantizeSphere(at, radius(rng));
            w.CarveSphereQ(sphere, trial % 6 == 5 ? uint16_t{0} : QuantizePower(100.0f));
            const SearchBoxes boxes = BoxesOf(sphere);
            const Islands voxel = VoxelSearch(w, t.materials, boxes);
            GraphSearchResult raw;
            const Islands graph = GraphSearch(w, cache, t.materials, boxes, 32768, &raw);
            ASSERT_EQ(voxel.searched, graph.searched) << trial;
            if (!voxel.searched) continue;
            overfull += raw.overfullCells;
            budgetTrials += (voxel.budgetSpent != 0 || graph.budgetSpent != 0) ? 1u : 0u;
            // Never more than the voxel search (the design's floor), unless the voxel search ran out
            // of budget: then the graph may still prove a later candidate it gave up on.
            std::unordered_set<uint64_t> kept, freed;
            AddVoxels(voxel.box, kept);
            AddVoxels(voxel.face, kept);
            AddVoxels(graph.box, freed);
            AddVoxels(graph.face, freed);
            uint32_t extra = 0;
            for (const uint64_t k : freed) extra += kept.count(k) ? 0u : 1u;
            if (voxel.budgetSpent == 0) EXPECT_EQ(extra, 0u) << "trial " << trial << " (pack " << pack << ")";
            // Exactly the same where neither budget ran out.
            if (voxel.budgetSpent == 0 && graph.budgetSpent == 0) {
                EXPECT_TRUE(SameIslands(voxel.box, graph.box)) << trial;
                EXPECT_EQ(voxel.complete, graph.complete) << trial;
                EXPECT_TRUE(SameIslands(voxel.face, graph.face)) << trial;
                ++compared;
            }
            islands += static_cast<uint32_t>(graph.box.size() + graph.face.size());
            faceIslands += static_cast<uint32_t>(graph.face.size());
            // Apply what was found (as the blast would), so later trials see a changed world.
            for (const std::vector<FloatingIsland>* list : {&graph.box, &graph.face})
                for (const FloatingIsland& island : *list) w.ClearMask(island.origin, VoxelMask::FromVolume(island.volume));
        }
    }
    std::printf("compared %u searches (%u with a budget spent): %u islands (%u across the island box face), over-full cells summed per "
                "search %u (a cell repeats across searches)\n",
                compared, budgetTrials, islands, faceIslands, overfull);
    EXPECT_GT(compared, 50u);
    EXPECT_GT(faceIslands, 5u);
    EXPECT_GT(islands, faceIslands);
    EXPECT_GT(overfull, 0u);
}

// The work budget counts node visits (R5): with a small budget the small island is still proven
// first (smallest box component first), the rest is attached by budget. The counters a budget may
// read are the same with a cold and a warm cache (same world, same workers) and with one and three
// workers (both cold); only `graphsBuilt` depends on the cache.
TEST(Structure, GraphBudgetIsCountedInNodeVisits) {
    const auto Counters = [](const GraphSearchResult& r) {
        return std::array<uint32_t, 11>{r.nodeVisits, r.floods, r.attachedByBudget, r.attachedByLimit, r.attachedByFace, r.attachedByKnown,
                                        r.merged, r.seedNodes, r.boxComponents, r.chunkGraphs, static_cast<uint32_t>(r.faceIslands.size())};
    };
    GraphSearchResult cold[2], warm;
    for (int run = 0; run < 2; ++run) {
        FlatWorld f(run == 0 ? 1u : 3u);
        World& w = *f.world;
        // Four floating slabs of 50 x 25 x 50 with 1-voxel rods into the island box, and a small
        // island crossing its top face (FaceFloodTests' SlabsAndASmallIsland).
        w.FillBox({30, 100, -25}, {80, 125, 25}, f.ids.stone);
        w.FillBox({10, 112, 0}, {30, 113, 1}, f.ids.stone);
        w.FillBox({-80, 100, -25}, {-30, 125, 25}, f.ids.stone);
        w.FillBox({-30, 112, 0}, {-10, 113, 1}, f.ids.stone);
        w.FillBox({-25, 100, 30}, {25, 125, 80}, f.ids.stone);
        w.FillBox({0, 112, 10}, {1, 113, 30}, f.ids.stone);
        w.FillBox({-25, 100, -80}, {25, 125, -30}, f.ids.stone);
        w.FillBox({0, 112, -30}, {1, 113, -10}, f.ids.stone);
        w.FillBox({20, 136, 20}, {21, 141, 21}, f.ids.wood);
        w.FillBox({19, 141, 19}, {22, 144, 22}, f.ids.wood);
        TerrainGraphCache cache;
        const SearchBoxes boxes = BoxesOf(SphereQ{glm::ivec3(8, 112 * 16 + 8, 8), 64});
        for (int pass = 0; pass < (run == 1 ? 2 : 1); ++pass) {
            GraphSearchResult& r = pass == 0 ? cold[run] : warm;
            const Islands graph = GraphSearch(w, cache, f.materials, boxes, 150, &r);
            ASSERT_TRUE(graph.searched);
            ASSERT_EQ(graph.face.size(), 1u);
            EXPECT_EQ(graph.face[0].voxelCount, 32u);
            EXPECT_GE(r.attachedByBudget, 1u);
            EXPECT_EQ(r.nodeVisits, 151u);
        }
    }
    EXPECT_EQ(Counters(cold[1]), Counters(warm));      // cold vs warm cache, 3 workers
    EXPECT_GT(cold[1].graphsBuilt, 0u);
    EXPECT_EQ(warm.graphsBuilt, 0u);
    EXPECT_EQ(Counters(cold[0]), Counters(cold[1]));   // 1 vs 3 workers, cold
}

// A stale graph (an edit path that missed World::ChunkRevision, simulated by a test hook) must
// never turn into islands: a 1-voxel bridge joins a floating block to a pillar on the ground, but
// the cache holds the graph from before the bridge. The search sees the re-labelled voxels differ,
// drops the cache and searches again on fresh graphs: the block is attached.
TEST(Structure, StaleGraphNeverDetachesAttachedVoxels) {
    FlatWorld f;
    World& w = *f.world;
    // Cell y 96..103: the block (y 100..103) and the pillar's top (y 96..98), a gap at y 99.
    w.FillBox({10, 100, 10}, {14, 104, 14}, f.ids.stone);
    w.FillBox({10, 80, 10}, {11, 99, 11}, f.ids.stone);
    const glm::ivec3 chunk = VoxelToChunk(glm::ivec3(10, 100, 10));
    const GraphMaterials materials = MakeGraphMaterials(&f.materials, MaterialFlag_Liquid);
    TerrainGraphCache cache;
    cache.BeginPass(w, materials);
    const ChunkGraph before = *cache.Get(w, chunk);
    const SearchBoxes boxes = BoxesOf(QuantizeSphere(glm::dvec3(12.0, 101.0, 12.0), 2.0));
    {
        // Fresh graphs: the block floats.
        const Islands islands = GraphSearch(w, cache, f.materials, boxes);
        ASSERT_EQ(islands.box.size(), 1u);
        EXPECT_EQ(islands.box[0].voxelCount, 64u);
    }
    ASSERT_TRUE(w.SetVoxel({10, 99, 10}, f.ids.stone));    // the bridge
    cache.BeginPass(w, materials);
    cache.InjectForTesting(w, chunk, before);              // ... that the cache did not see
    GraphSearchLimits limits;
    limits.boxMin = boxes.boxMin;
    limits.boxMax = boxes.boxMax;
    limits.floodMin = boxes.floodMin;
    limits.floodMax = boxes.floodMax;
    limits.maxIslandVoxels = 60000;
    limits.maxNodeVisits = 32768;
    const GraphSearchResult r = SearchIslandsOnGraph(w, cache, materials, limits);
    ASSERT_TRUE(r.searched);
    EXPECT_GT(r.relabelMismatch, 0u);
    EXPECT_EQ(r.staleGraphRetries, 1u);
    EXPECT_TRUE(r.boxIslands.empty()) << "a stale graph detached " << (r.boxIslands.empty() ? 0u : r.boxIslands[0].voxelCount) << " voxels";
    EXPECT_TRUE(r.faceIslands.empty());
    // The retry rebuilt the cache: the next search is consistent.
    const Islands again = GraphSearch(w, cache, f.materials, boxes);
    EXPECT_TRUE(again.box.empty() && again.face.empty());
}

// The cache's cap: passes over a 256-chunk window with a cache of 64 evict (changed chunks first,
// then the least recently used) and every graph still equals a fresh build; the byte count equals
// what the graphs hold.
TEST(Structure, EvictionNeverChangesAGraph) {
    TerrainWorld t(true);
    World& w = *t.world;
    w.LoadBlocking(glm::dvec3(20.0, 20.0, -10.0));
    const GraphMaterials materials = MakeGraphMaterials(&t.materials, MaterialFlag_Liquid);
    TerrainGraphCache small(64), big;
    const glm::ivec3 lo = w.WindowMinChunk(), size = w.WindowSize();
    ASSERT_GT(size.x * size.y * size.z, 128);
    std::mt19937 rng(5);
    for (int pass = 0; pass < 4; ++pass) {
        if (pass > 0) {
            // Edits between passes: some cached graphs go stale.
            const glm::ivec3 v0 = ChunkToVoxel(lo);
            for (int k = 0; k < 6; ++k) {
                const glm::ivec3 c = v0 + glm::ivec3(static_cast<int>(rng() % (size.x * kChunkSize)), static_cast<int>(rng() % (size.y * kChunkSize)),
                                                     static_cast<int>(rng() % (size.z * kChunkSize)));
                w.FillBox(c, c + glm::ivec3(5, 3, 5), (k & 1) ? t.ids.planks : uint8_t{0});
            }
        }
        small.BeginPass(w, materials);
        TerrainGraphCache fresh;
        fresh.BeginPass(w, materials);
        // A different order each pass, so the least recently used are not simply the first.
        std::vector<glm::ivec3> chunks;
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x) chunks.push_back(lo + glm::ivec3(x, y, z));
        std::shuffle(chunks.begin(), chunks.end(), rng);
        for (const glm::ivec3& c : chunks) {
            const ChunkGraph* a = small.Get(w, c);
            const ChunkGraph* b = fresh.Get(w, c);
            ASSERT_TRUE(a != nullptr && b != nullptr);
            EXPECT_TRUE(SameGraph(*a, *b)) << c.x << ", " << c.y << ", " << c.z << " pass " << pass;
        }
    }
    EXPECT_GT(small.Stats().evicted, 0u);
    EXPECT_GT(small.Stats().reused, 0u);
    // Bytes: own graphs of Dense / Packed chunks, each shared graph once, 64 per entry (TerrainGraphStats).
    big.BeginPass(w, materials);
    uint64_t expected = 0;
    std::unordered_set<const ChunkGraph*> shared;
    for (int z = 0; z < size.z; ++z)
        for (int y = 0; y < size.y; ++y)
            for (int x = 0; x < size.x; ++x) {
                const glm::ivec3 c = lo + glm::ivec3(x, y, z);
                const ChunkGraph* g = big.Get(w, c);
                ASSERT_NE(g, nullptr);
                const CellState state = w.StateOf(c);
                if (state == CellState::Dense || state == CellState::Packed) expected += ChunkGraphBytes(*g);
                else if (shared.insert(g).second) expected += ChunkGraphBytes(*g);
            }
    expected += 64u * big.Size();
    EXPECT_EQ(big.Stats().bytes, expected);
    EXPECT_GT(big.Stats().shared, 0u);
    std::printf("%zu chunks: %.1f KB cached (%.1f KB per chunk)\n", big.Size(), big.Stats().bytes / 1024.0,
                big.Stats().bytes / 1024.0 / static_cast<double>(big.Size()));
}

// Not a gate: what a chunk graph costs to build on the game's terrain (0.125 m, the built-in
// generator, Packed chunks on), by chunk kind, in a quiet process. Run with
//   VortexCoreTests --gtest_also_run_disabled_tests --gtest_filter='*GraphBuildCost*'
TEST(Structure, DISABLED_GraphBuildCost) {
    ChunkStore store(8192);
    JobSystem jobs(3);
    MaterialLibrary materials;
    WorldConfig config;
    config.windowSize = {12, 8, 12};
    config.fullHeight = true;
    config.packHidden = true;
    TerrainSettings terrain;
    terrain.materials = RegisterDefaultWorldMaterials(materials);
    World w(config, store, jobs, CreateTerrainGenerator(terrain), &materials);
    w.LoadBlocking(glm::dvec3(10.0, 10.0, 10.0));
    const GraphMaterials gm = MakeGraphMaterials(&materials, MaterialFlag_Liquid);
    for (int pass = 0; pass < 3; ++pass) {
        double ms[6] = {};
        uint32_t count[6] = {}, nodes[6] = {}, overfull = 0;
        uint64_t bytes[6] = {};
        TerrainGraphCache cache(1 << 16);
        cache.BeginPass(w, gm);
        const glm::ivec3 lo = w.WindowMinChunk(), size = w.WindowSize();
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x) {
                    const glm::ivec3 c = lo + glm::ivec3(x, y, z);
                    const int kind = static_cast<int>(w.StateOf(c));
                    const double before = cache.Stats().buildMs;
                    const ChunkGraph* g = cache.Get(w, c);
                    if (g == nullptr) continue;
                    ms[kind] += cache.Stats().buildMs - before;
                    ++count[kind];
                    nodes[kind] += static_cast<uint32_t>(g->nodes.size());
                    bytes[kind] += ChunkGraphBytes(*g);
                    overfull += g->overfullCells;
                }
        std::printf("pass %d: graph bytes per chunk: dense %.1f KB, packed %.1f KB (Uniform / empty share one graph per id); cache %.1f MB for "
                    "%zu chunks (%.2f KB per chunk, entries included)\n",
                    pass, count[4] ? bytes[4] / 1024.0 / count[4] : 0.0, count[5] ? bytes[5] / 1024.0 / count[5] : 0.0,
                    cache.Stats().bytes / 1048576.0, cache.Size(), cache.Stats().bytes / 1024.0 / static_cast<double>(cache.Size()));
        std::printf("pass %d: dense %u chunks %.1f us each (%.1f nodes), packed %u %.1f us (%.1f nodes), uniform %u %.1f us, empty %u %.1f us; "
                    "%u over-full cells\n",
                    pass, count[4], count[4] ? 1000.0 * ms[4] / count[4] : 0.0, count[4] ? double(nodes[4]) / count[4] : 0.0, count[5],
                    count[5] ? 1000.0 * ms[5] / count[5] : 0.0, count[5] ? double(nodes[5]) / count[5] : 0.0, count[3],
                    count[3] ? 1000.0 * ms[3] / count[3] : 0.0, count[2], count[2] ? 1000.0 * ms[2] / count[2] : 0.0, overfull);
    }
    // The build alone on data in the CPU cache (a copy of each Dense chunk, built 20 times).
    Chunk copy;
    ChunkGraph g;
    double hotMs = 0.0;
    uint32_t hot = 0;
    const glm::ivec3 lo = w.WindowMinChunk(), size = w.WindowSize();
    for (int z = 0; z < size.z; ++z)
        for (int y = 0; y < size.y; ++y)
            for (int x = 0; x < size.x; ++x) {
                const glm::ivec3 c = lo + glm::ivec3(x, y, z);
                if (w.StateOf(c) != CellState::Dense || !w.ReadChunk(c, copy)) continue;
                BuildChunkGraph(g, c, &copy, 0, gm);
                const auto start = std::chrono::steady_clock::now();
                for (int k = 0; k < 20; ++k) BuildChunkGraph(g, c, &copy, 0, gm);
                hotMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20.0;
                ++hot;
            }
    std::printf("dense, data in cache: %.1f us per chunk (%u chunks)\n", 1000.0 * hotMs / hot, hot);
}
