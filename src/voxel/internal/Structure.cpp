module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

module vortex.voxel;

import vortex.core;

namespace vortex::voxel {

    namespace {
        using Slices = std::array<uint64_t, 8>;      // a cell: z slices, bit x + 8y
        constexpr uint64_t kColumnX0 = 0x0101010101010101ull;
        constexpr uint64_t kColumnX7 = 0x8080808080808080ull;
        constexpr uint64_t kFull = ~0ull;

        /// Bits at 8k (k = 0..7) -> bit k. Every partial product lands on its own bit: no carries.
        constexpr uint64_t Gather8(uint64_t column) { return (column * 0x0102040810204080ull) >> 56; }

        /// Grows `v` (a subset of `s`) inside one 8 x 8 slice to the 4-connected components of `s` it
        /// touches. Each direction is an occluded fill (Kogge-Stone: a run of up to 7 in 3 shifts);
        /// the four are repeated until nothing changes (once or twice for most shapes).
        uint64_t FillSlice(uint64_t v, uint64_t s) {
            const uint64_t east = s & ~kColumnX0, west = s & ~kColumnX7;
            for (;;) {
                const uint64_t old = v;
                uint64_t p = east;
                v |= p & (v << 1);
                p &= p << 1;
                v |= p & (v << 2);
                p &= p << 2;
                v |= p & (v << 4);
                p = west;
                v |= p & (v >> 1);
                p &= p >> 1;
                v |= p & (v >> 2);
                p &= p >> 2;
                v |= p & (v >> 4);
                p = s;
                v |= p & (v << 8);
                p &= p << 8;
                v |= p & (v << 16);
                p &= p << 16;
                v |= p & (v << 32);
                p = s;
                v |= p & (v >> 8);
                p &= p >> 8;
                v |= p & (v >> 16);
                p &= p >> 16;
                v |= p & (v >> 32);
                if (v == old) return v;
            }
        }

        /// Grows `c` (a subset of `solid`) to the union of the 6-connected components of `solid` it
        /// touches: a worklist of slices, a slice is filled again only when a neighbour slice grew.
        Slices FloodCell(Slices c, const Slices& solid) {
            uint32_t dirty = 0;
            for (int z = 0; z < 8; ++z) dirty |= c[z] != 0 ? (1u << z) : 0u;
            while (dirty != 0) {
                const int z = std::countr_zero(dirty);
                dirty &= dirty - 1;
                uint64_t v = c[z];
                if (z > 0) v |= c[z - 1] & solid[z];
                if (z < 7) v |= c[z + 1] & solid[z];
                v = FillSlice(v, solid[z]);
                c[z] = v;
                // A neighbour slice with solid voxels next to this one that it does not hold yet grows.
                if (z > 0 && (v & solid[z - 1] & ~c[z - 1]) != 0) dirty |= 1u << (z - 1);
                if (z < 7 && (v & solid[z + 1] & ~c[z + 1]) != 0) dirty |= 1u << (z + 1);
            }
            return c;
        }

        /// Per id: its density in the low 32 bits, bit 32 set for an ignored id. Summed over a
        /// block in one pass: the low word is the mass, the high word says whether anything was ignored.
        using DensityTable = std::array<uint64_t, 256>;
        DensityTable MakeDensityTable(const GraphMaterials& materials) {
            DensityTable t{};
            for (uint32_t id = 1; id < 256; ++id)
                t[id] = materials.ignored[id] ? (uint64_t{1} << 32) : static_cast<uint64_t>(materials.densityQ[id]);
            return t;
        }

        /// The solid (not ignored) voxels of a 4^3 block (bit lx + 4 ly + 16 lz) and their density sum.
        uint64_t BlockSolid(const Chunk& chunk, uint32_t b, const GraphMaterials& materials, const DensityTable* table, uint32_t& mass) {
            const uint64_t occupied = chunk.occ.voxelMask[b];
            mass = 0;
            if (occupied == 0) return 0;
            const uint8_t* ids = chunk.ids + static_cast<size_t>(b) * 64u;
            // One id for the whole block (most blocks): no per-voxel work.
            uint64_t first;
            std::memcpy(&first, ids, 8);
            bool same = (first == 0x0101010101010101ull * ids[0]);
            for (size_t k = 8; same && k < 64; k += 8) {
                uint64_t word;
                std::memcpy(&word, ids + k, 8);
                same = word == first;
            }
            if (same) {
                if (materials.ignored[ids[0]]) return 0;
                mass = 64u * materials.densityQ[ids[0]];
                return occupied;
            }
            if (table != nullptr) {
                uint64_t sum = 0;
                for (uint32_t l = 0; l < 64; ++l) sum += (*table)[ids[l]];
                mass = static_cast<uint32_t>(sum);
                if ((sum >> 32) == 0) return occupied;        // nothing ignored: the occupancy is the answer
            }
            uint64_t m = 0;
            for (uint32_t l = 0; l < 64; ++l) {
                const uint8_t id = ids[l];
                m |= static_cast<uint64_t>(id != 0 && !materials.ignored[id]) << l;
            }
            return m;
        }

        /// The solid (not ignored) voxels of cell `cell` of a chunk, and the sum of their densities.
        void CellSolid(const Chunk& chunk, uint32_t cell, const GraphMaterials& materials, const DensityTable* table, Slices& out,
                       uint32_t* mass) {
            out = {};
            uint32_t sum = 0;
            const uint32_t cx = cell & 3u, cy = (cell >> 2) & 3u, cz = cell >> 4;
            for (uint32_t dz = 0; dz < 2; ++dz)
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        const uint32_t bx = cx * 2 + dx, by = cy * 2 + dy, bz = cz * 2 + dz;
                        const uint32_t b = bx + (by << 3) + (bz << 6);
                        uint32_t blockMass = 0;
                        const uint64_t m = BlockSolid(chunk, b, materials, table, blockMass);
                        sum += blockMass;
                        if (m == 0) continue;
                        const uint32_t xo = dx * 4, yo = dy * 4, zo = dz * 4;
                        for (uint32_t lz = 0; lz < 4; ++lz) {
                            // Four rows of 4 bits -> four rows of the slice, at x offset xo.
                            const uint64_t rows = (m >> (16 * lz)) & 0xFFFFu;
                            const uint64_t spread = (rows & 0xFu) | ((rows & 0xF0u) << 4) | ((rows & 0xF00u) << 8) | ((rows & 0xF000u) << 12);
                            out[zo + lz] |= spread << (xo + 8 * yo);
                        }
                    }
            if (mass) *mass = sum;
        }

        void Faces(const Slices& n, GraphNode& node) {
            uint64_t negX = 0, posX = 0, negY = 0, posY = 0;
            for (int z = 0; z < 8; ++z) {
                negX |= Gather8(n[z] & kColumnX0) << (8 * z);
                posX |= Gather8((n[z] >> 7) & kColumnX0) << (8 * z);
                negY |= (n[z] & 0xFFu) << (8 * z);
                posY |= (n[z] >> 56) << (8 * z);
            }
            node.face[kFaceNegX] = negX;
            node.face[kFacePosX] = posX;
            node.face[kFaceNegY] = negY;
            node.face[kFacePosY] = posY;
            node.face[kFaceNegZ] = n[0];
            node.face[kFacePosZ] = n[7];
            // Bounds: x from the OR of all rows, y from non-empty rows, z from non-empty slices.
            uint64_t all = 0;
            int zMin = 8, zMax = -1;
            for (int z = 0; z < 8; ++z) {
                all |= n[z];
                if (n[z] != 0) {
                    zMin = std::min(zMin, z);
                    zMax = z;
                }
            }
            uint64_t xs = all | (all >> 32);
            xs |= xs >> 16;
            xs |= xs >> 8;
            xs &= 0xFFu;
            uint32_t ys = 0;
            for (int y = 0; y < 8; ++y) ys |= ((all >> (8 * y)) & 0xFFu) != 0 ? (1u << y) : 0u;
            node.boxMin[0] = static_cast<uint8_t>(std::countr_zero(xs));
            node.boxMax[0] = static_cast<uint8_t>(63 - std::countl_zero(xs));
            node.boxMin[1] = static_cast<uint8_t>(std::countr_zero(ys));
            node.boxMax[1] = static_cast<uint8_t>(31 - std::countl_zero(ys));
            node.boxMin[2] = static_cast<uint8_t>(zMin);
            node.boxMax[2] = static_cast<uint8_t>(zMax);
        }

        /// Cell index -> its neighbour across `face` in the same chunk, or -1 at the chunk face.
        int NeighbourCell(uint32_t cell, uint8_t face) {
            const int c[3] = {static_cast<int>(cell & 3u), static_cast<int>((cell >> 2) & 3u), static_cast<int>(cell >> 4)};
            const int axis = face >> 1;
            const int step = (face & 1u) ? 1 : -1;
            const int v = c[axis] + step;
            if (v < 0 || v >= kCellsPerChunkAxis) return -1;
            int n[3] = {c[0], c[1], c[2]};
            n[axis] = v;
            return n[0] + kCellsPerChunkAxis * (n[1] + kCellsPerChunkAxis * n[2]);
        }

        /// The same cell index on the other side of a chunk face (the neighbour chunk's cell).
        uint32_t AcrossChunkFace(uint32_t cell, uint8_t face) {
            int c[3] = {static_cast<int>(cell & 3u), static_cast<int>((cell >> 2) & 3u), static_cast<int>(cell >> 4)};
            c[face >> 1] = (face & 1u) ? 0 : kCellsPerChunkAxis - 1;
            return static_cast<uint32_t>(c[0] + kCellsPerChunkAxis * (c[1] + kCellsPerChunkAxis * c[2]));
        }

        glm::ivec3 CellOrigin(uint32_t cell) {
            return glm::ivec3(static_cast<int>(cell & 3u), static_cast<int>((cell >> 2) & 3u), static_cast<int>(cell >> 4)) * kCellSize;
        }

        void AddNode(ChunkGraph& out, uint32_t cell, const Slices& n, uint32_t mass) {
            GraphNode node;
            Faces(n, node);
            uint32_t voxels = 0;
            int first = -1;
            for (int z = 0; z < 8; ++z) {
                voxels += static_cast<uint32_t>(std::popcount(n[z]));
                if (first < 0 && n[z] != 0) first = std::countr_zero(n[z]) + 64 * z;
            }
            node.voxels = static_cast<uint16_t>(voxels);
            node.firstVoxel = static_cast<uint16_t>(first);
            node.mass = mass;
            node.cell = static_cast<uint8_t>(cell);
            out.nodes.push_back(node);
            out.solidVoxels += voxels;
        }

        uint32_t NodeMass(const Chunk& chunk, uint32_t cell, const Slices& n, const GraphMaterials& materials) {
            const glm::ivec3 o = CellOrigin(cell);
            uint32_t sum = 0;
            for (int z = 0; z < 8; ++z)
                for (uint64_t bits = n[z]; bits != 0; bits &= bits - 1) {
                    const int i = std::countr_zero(bits);
                    sum += materials.densityQ[chunk.Get(o.x + (i & 7), o.y + (i >> 3), o.z + z)];
                }
            return sum;
        }

        void Link(ChunkGraph& out) {
            out.links.clear();
            for (size_t a = 0; a < out.nodes.size(); ++a) {
                GraphNode& node = out.nodes[a];
                node.firstLink = static_cast<uint16_t>(out.links.size());
                for (uint8_t f = 0; f < 6; ++f) {
                    if (node.face[f] == 0) continue;
                    const int nc = NeighbourCell(node.cell, f);
                    if (nc < 0) continue;
                    for (uint32_t b = out.cellFirst[static_cast<size_t>(nc)]; b < out.cellFirst[static_cast<size_t>(nc) + 1]; ++b) {
                        const uint32_t bond = BondVoxels(node, out.nodes[b], f);
                        if (bond != 0) out.links.push_back({static_cast<uint16_t>(b), f, 0, static_cast<uint16_t>(bond)});
                    }
                }
                node.linkCount = static_cast<uint16_t>(out.links.size() - node.firstLink);
            }
        }

        uint64_t ChunkKey(const glm::ivec3& c) {
            constexpr int64_t kBias = int64_t{1} << 20;
            const auto part = [](int v) { return static_cast<uint64_t>(static_cast<int64_t>(v) + kBias) & 0x1FFFFFull; };
            return part(c.x) | (part(c.y) << 21) | (part(c.z) << 42);
        }
    }

    GraphMaterials MakeGraphMaterials(const MaterialLibrary* materials, uint32_t ignoreFlags) {
        GraphMaterials out;
        for (uint32_t id = 1; id < 256; ++id) {
            if (materials == nullptr) {
                out.densityQ[id] = 1000;
                continue;
            }
            const Material& m = materials->Get(kWorldPalette, static_cast<uint8_t>(id));
            out.ignored[id] = (m.flags & ignoreFlags) != 0;
            const float d = std::isfinite(m.density) ? std::clamp(m.density, 0.0f, 65535.0f) : 0.0f;
            out.densityQ[id] = out.ignored[id] ? uint16_t{0} : static_cast<uint16_t>(std::lround(d));
        }
        return out;
    }

    void BuildChunkGraph(ChunkGraph& out, const glm::ivec3& coord, const Chunk* chunk, uint8_t uniformId, const GraphMaterials& materials) {
        out.chunk = coord;
        out.nodes.clear();
        out.links.clear();
        out.cellFirst.fill(0);
        out.overfullCells = 0;
        out.solidVoxels = 0;
        if (chunk == nullptr) {
            if (uniformId == 0 || materials.ignored[uniformId]) {
                out.nodes.shrink_to_fit();
                out.links.shrink_to_fit();
                return;
            }
            // Every cell full: one node each, every face complete. Built once, then copied.
            static const ChunkGraph kFullChunk = [] {
                ChunkGraph g;
                Slices full;
                full.fill(kFull);
                for (uint32_t c = 0; c < static_cast<uint32_t>(kCellsPerChunk); ++c) {
                    g.cellFirst[c] = static_cast<uint16_t>(g.nodes.size());
                    AddNode(g, c, full, 0);
                }
                g.cellFirst[kCellsPerChunk] = static_cast<uint16_t>(g.nodes.size());
                Link(g);
                return g;
            }();
            out.nodes = kFullChunk.nodes;
            out.links = kFullChunk.links;
            out.cellFirst = kFullChunk.cellFirst;
            out.solidVoxels = kFullChunk.solidVoxels;
            const uint32_t mass = 512u * materials.densityQ[uniformId];
            for (GraphNode& node : out.nodes) node.mass = mass;
            out.nodes.shrink_to_fit();
            out.links.shrink_to_fit();
            return;
        }
        const DensityTable table = MakeDensityTable(materials);
        for (uint32_t c = 0; c < static_cast<uint32_t>(kCellsPerChunk); ++c) {
            out.cellFirst[c] = static_cast<uint16_t>(out.nodes.size());
            if (((chunk->occ.cellMask >> c) & 1u) == 0) continue;
            Slices solid;
            uint32_t cellMass = 0;
            CellSolid(*chunk, c, materials, &table, solid, &cellMass);
            bool full = true, any = false;
            for (const uint64_t s : solid) {
                full = full && s == kFull;
                any = any || s != 0;
            }
            if (!any) continue;
            if (full) {
                AddNode(out, c, solid, cellMass);
                continue;
            }
            Slices left = solid;
            uint32_t nodes = 0;
            for (int z = 0; z < 8; ++z) {
                while (left[z] != 0) {
                    Slices seed{};
                    seed[static_cast<size_t>(z)] = left[z] & (~left[z] + 1);      // lowest bit: the first voxel in scan order
                    const Slices component = FloodCell(seed, solid);
                    bool last = true;
                    for (int k = 0; k < 8; ++k) {
                        left[k] &= ~component[k];
                        last = last && left[k] == 0;
                    }
                    // The only node of its cell has the whole cell's mass.
                    const uint32_t mass = (nodes == 0 && last) ? cellMass : NodeMass(*chunk, c, component, materials);
                    AddNode(out, c, component, mass);
                    ++nodes;
                }
            }
            if (nodes > kMaxCellNodes) ++out.overfullCells;
        }
        out.cellFirst[kCellsPerChunk] = static_cast<uint16_t>(out.nodes.size());
        Link(out);
        // No growth slack is kept (a graph lives in the cache until its chunk changes).
        if (out.nodes.capacity() != out.nodes.size()) out.nodes.shrink_to_fit();
        if (out.links.capacity() != out.links.size()) out.links.shrink_to_fit();
    }

    size_t ChunkGraphBytes(const ChunkGraph& graph) {
        return sizeof(ChunkGraph) + graph.nodes.capacity() * sizeof(GraphNode) + graph.links.capacity() * sizeof(GraphLink);
    }

    std::array<uint64_t, 8> NodeVoxels(const Chunk* chunk, uint8_t uniformId, const GraphNode& node, const GraphMaterials& materials) {
        Slices solid{};
        if (chunk == nullptr) {
            if (uniformId != 0 && !materials.ignored[uniformId]) solid.fill(kFull);
        } else {
            CellSolid(*chunk, node.cell, materials, nullptr, solid, nullptr);
        }
        Slices seed{};
        const uint32_t z = node.firstVoxel >> 6u;
        seed[z] = (uint64_t{1} << (node.firstVoxel & 63u)) & solid[z];
        if (seed[z] == 0) return seed;
        return FloodCell(seed, solid);
    }

    // --- cache ----------------------------------------------------------------------------------

    namespace {
        /// A map entry's own cost besides its graph (node, key, bucket pointer): an estimate.
        constexpr int64_t kEntryBytes = 64;
    }

    TerrainGraphCache::TerrainGraphCache(size_t maxChunks) : m_MaxChunks(std::max<size_t>(maxChunks, 64)) {}
    TerrainGraphCache::~TerrainGraphCache() = default;

    void TerrainGraphCache::AddBytes(int64_t delta) {
        m_Stats.bytes = static_cast<uint64_t>(static_cast<int64_t>(m_Stats.bytes) + delta);
        m_Stats.peakBytes = std::max(m_Stats.peakBytes, m_Stats.bytes);
    }

    void TerrainGraphCache::Clear() {
        m_Entries.clear();
        for (std::unique_ptr<ChunkGraph>& g : m_Shared) g.reset();
        m_Stats.bytes = 0;
        m_WorldId = 0;
    }

    void TerrainGraphCache::BeginPass(const World& world, const GraphMaterials& materials) {
        if (m_WorldId != world.InstanceId() || !(m_Materials == materials)) {
            Clear();
            m_WorldId = world.InstanceId();
            m_Materials = materials;
        }
        ++m_Pass;
        if (m_Entries.size() > m_MaxChunks) Evict(world);
    }

    void TerrainGraphCache::Evict(const World& world) {
        const auto Drop = [this](std::unordered_map<uint64_t, Entry>::iterator it) {
            AddBytes(-(kEntryBytes + (it->second.own ? static_cast<int64_t>(ChunkGraphBytes(*it->second.own)) : 0)));
            ++m_Stats.evicted;
            return m_Entries.erase(it);
        };
        // Entries the next lookup would rebuild anyway (changed or unloaded chunks) go first.
        for (auto it = m_Entries.begin(); it != m_Entries.end();) {
            if (world.ChunkRevision(it->second.chunk) != it->second.revision) it = Drop(it);
            else ++it;
        }
        const size_t target = m_MaxChunks * 3 / 4;
        if (m_Entries.size() <= target) return;
        // Then the least recently used, down to 3/4 of the cap, in one partial sort.
        std::vector<std::pair<uint64_t, uint64_t>> uses;
        uses.reserve(m_Entries.size());
        for (const auto& [key, entry] : m_Entries) uses.emplace_back(entry.lastUse, key);
        const size_t drop = m_Entries.size() - target;
        std::nth_element(uses.begin(), uses.begin() + static_cast<std::ptrdiff_t>(drop), uses.end());
        for (size_t i = 0; i < drop; ++i) Drop(m_Entries.find(uses[i].second));
    }

    const ChunkGraph* TerrainGraphCache::Shared(uint8_t uniformId) {
        if (uniformId != 0 && m_Materials.ignored[uniformId]) uniformId = 0;
        std::unique_ptr<ChunkGraph>& g = m_Shared[uniformId];
        if (!g) {
            g = std::make_unique<ChunkGraph>();
            BuildChunkGraph(*g, glm::ivec3(0), nullptr, uniformId, m_Materials);
            AddBytes(static_cast<int64_t>(ChunkGraphBytes(*g)));
        }
        return g.get();
    }

    void TerrainGraphCache::SetOwn(Entry& entry, std::unique_ptr<ChunkGraph> graph) {
        if (entry.own) AddBytes(-static_cast<int64_t>(ChunkGraphBytes(*entry.own)));
        entry.own = std::move(graph);
        if (entry.own) AddBytes(static_cast<int64_t>(ChunkGraphBytes(*entry.own)));
        entry.graph = entry.own.get();
    }

    const ChunkGraph* TerrainGraphCache::Get(const World& world, const glm::ivec3& chunk) {
        const uint64_t revision = world.ChunkRevision(chunk);
        const uint64_t key = ChunkKey(chunk);
        if (revision == 0) {
            const auto it = m_Entries.find(key);
            if (it != m_Entries.end()) {
                AddBytes(-(kEntryBytes + (it->second.own ? static_cast<int64_t>(ChunkGraphBytes(*it->second.own)) : 0)));
                m_Entries.erase(it);
            }
            return nullptr;
        }
        const auto [it, inserted] = m_Entries.try_emplace(key);
        Entry& entry = it->second;
        if (inserted) AddBytes(kEntryBytes);
        entry.lastUse = m_Pass;
        if (entry.graph != nullptr && entry.revision == revision && entry.chunk == chunk) {
            ++m_Stats.reused;
            return entry.graph;
        }
        entry.chunk = chunk;
        entry.revision = revision;
        uint8_t uniformId = 0;
        const Chunk* data = nullptr;
        switch (world.StateOf(chunk)) {
            case CellState::Uniform: world.GetChunk(chunk, &uniformId); break;
            case CellState::Dense: data = world.GetChunk(chunk); break;
            case CellState::Packed:
                if (!m_Decoded) m_Decoded = std::make_unique<Chunk>();
                if (world.ReadChunk(chunk, *m_Decoded)) {
                    data = m_Decoded.get();
                    ++m_Stats.decoded;
                }
                break;
            default: break;
        }
        if (data == nullptr) {
            // Uniform or empty: the graph shared by its id (no copy per chunk).
            SetOwn(entry, nullptr);
            entry.graph = Shared(uniformId);
            ++m_Stats.shared;
            return entry.graph;
        }
        const auto start = std::chrono::steady_clock::now();
        // A rebuilt chunk reuses its graph's allocation (BuildChunkGraph trims it to size).
        std::unique_ptr<ChunkGraph> graph;
        if (entry.own) {
            AddBytes(-static_cast<int64_t>(ChunkGraphBytes(*entry.own)));
            graph = std::move(entry.own);
        } else {
            graph = std::make_unique<ChunkGraph>();
        }
        BuildChunkGraph(*graph, chunk, data, 0, m_Materials);
        SetOwn(entry, std::move(graph));
        ++m_Stats.built;
        m_Stats.buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return entry.graph;
    }

    void TerrainGraphCache::InjectForTesting(const World& world, const glm::ivec3& chunk, ChunkGraph graph) {
        const auto [it, inserted] = m_Entries.try_emplace(ChunkKey(chunk));
        if (inserted) AddBytes(kEntryBytes);
        Entry& entry = it->second;
        entry.chunk = chunk;
        entry.revision = world.ChunkRevision(chunk);
        entry.lastUse = m_Pass;
        SetOwn(entry, std::make_unique<ChunkGraph>(std::move(graph)));
    }

    // --- the search -----------------------------------------------------------------------------

    namespace {
        constexpr int kMaxBoxAxis = 192;                       // as ExtractFloatingIslands / ProveFaceComponents
        constexpr int kTableAxis = kMaxBoxAxis / kChunkSize + 1;
        constexpr uint8_t kUnvisited = 0, kVisiting = 1, kIsland = 2, kAttached = 3;
        constexpr uint32_t kNoSeed = std::numeric_limits<uint32_t>::max();

        struct NodeRef {
            uint16_t entry = 0;      // chunk table index
            uint16_t node = 0;
        };

        /// The chunks of the flood box: each resolved once per search to its graph, with the
        /// search's per-node state.
        class GraphTable {
        public:
            enum class Kind : uint8_t { Unresolved, Air, Unloaded, Graph };
            struct Entry {
                Kind kind = Kind::Unresolved;
                const ChunkGraph* graph = nullptr;
                uint32_t base = 0;              // into state / seed
                glm::ivec3 chunk{0};
            };
            GraphTable(const World& world, TerrainGraphCache& cache, const glm::ivec3& floodMin, const glm::ivec3& floodMax,
                       GraphSearchResult& result)
                : m_World(world), m_Cache(cache), m_Result(result), m_Min(VoxelToChunk(floodMin)), m_Max(VoxelToChunk(floodMax - glm::ivec3(1))) {
                m_Entries.resize(static_cast<size_t>(kTableAxis) * kTableAxis * kTableAxis);
                if (world.FullHeight()) {
                    m_BottomRow = world.WindowMinChunk().y;
                    m_CeilingRow = m_BottomRow + world.WindowSize().y;
                }
            }
            bool Contains(const glm::ivec3& chunk) const { return glm::all(glm::greaterThanEqual(chunk, m_Min)) && glm::all(glm::lessThanEqual(chunk, m_Max)); }
            uint16_t IndexOf(const glm::ivec3& chunk) const {
                const glm::ivec3 r = chunk - m_Min;
                return static_cast<uint16_t>(r.x + kTableAxis * (r.y + kTableAxis * r.z));
            }
            /// Resolves (once) the chunk's entry. Rows above the build ceiling or below the bottom
            /// of a fixed-height window hold nothing.
            Entry& Resolve(uint16_t index, const glm::ivec3& chunk) {
                Entry& e = m_Entries[index];
                if (e.kind != Kind::Unresolved) return e;
                e.chunk = chunk;
                if (chunk.y >= m_CeilingRow || chunk.y < m_BottomRow) {
                    e.kind = Kind::Air;
                    return e;
                }
                const uint64_t builtBefore = m_Cache.Stats().built;
                const ChunkGraph* graph = m_Cache.Get(m_World, chunk);
                m_Result.graphsBuilt += m_Cache.Stats().built - builtBefore;
                if (graph == nullptr) {
                    e.kind = Kind::Unloaded;
                    return e;
                }
                if (graph->nodes.empty()) {
                    e.kind = Kind::Air;
                    return e;
                }
                e.kind = Kind::Graph;
                e.graph = graph;
                e.base = static_cast<uint32_t>(state.size());
                state.resize(state.size() + graph->nodes.size(), kUnvisited);
                seed.resize(state.size(), kNoSeed);
                ++m_Result.chunkGraphs;
                m_Result.overfullCells += graph->overfullCells;
                return e;
            }
            Entry& Resolve(const glm::ivec3& chunk) { return Resolve(IndexOf(chunk), chunk); }
            Entry& At(uint16_t index) { return m_Entries[index]; }
            const GraphNode& Node(const NodeRef& r) const { return m_Entries[r.entry].graph->nodes[r.node]; }
            uint32_t Slot(const NodeRef& r) const { return m_Entries[r.entry].base + r.node; }
            glm::ivec3 NodeMin(const NodeRef& r) const {
                const Entry& e = m_Entries[r.entry];
                const GraphNode& n = e.graph->nodes[r.node];
                return ChunkToVoxel(e.chunk) + CellOrigin(n.cell) + glm::ivec3(n.boxMin[0], n.boxMin[1], n.boxMin[2]);
            }
            glm::ivec3 NodeMax(const NodeRef& r) const {     // inclusive
                const Entry& e = m_Entries[r.entry];
                const GraphNode& n = e.graph->nodes[r.node];
                return ChunkToVoxel(e.chunk) + CellOrigin(n.cell) + glm::ivec3(n.boxMax[0], n.boxMax[1], n.boxMax[2]);
            }

            /// Calls fn(neighbour) for every node linked to `r`, faces -x +x -y +y -z +z, inside a
            /// face by node index. Across a chunk face the neighbour chunk is resolved; `unloaded`
            /// is called (and the walk stops) if it is not loaded. fn returns false to stop.
            template <typename Fn, typename Unloaded>
            void ForEachLink(const NodeRef& r, Fn&& fn, Unloaded&& unloaded) {
                const Entry& e = m_Entries[r.entry];
                const ChunkGraph& g = *e.graph;
                const GraphNode& node = g.nodes[r.node];
                const GraphLink* link = g.links.data() + node.firstLink;
                const GraphLink* end = link + node.linkCount;
                for (uint8_t f = 0; f < 6; ++f) {
                    if (node.face[f] == 0) continue;
                    if (NeighbourCell(node.cell, f) >= 0) {
                        for (; link != end && link->face == f; ++link) {
                            if (!fn(NodeRef{r.entry, link->node})) return;
                        }
                        continue;
                    }
                    glm::ivec3 next = e.chunk;
                    next[f >> 1] += (f & 1u) ? 1 : -1;
                    if (!Contains(next)) continue;           // never for a node off the flood box face
                    const uint16_t ni = IndexOf(next);
                    Entry& n = Resolve(ni, next);
                    if (n.kind == Kind::Air) continue;
                    if (n.kind == Kind::Unloaded) {
                        unloaded(next);
                        return;
                    }
                    const uint32_t cell = AcrossChunkFace(node.cell, f);
                    const ChunkGraph& ng = *n.graph;
                    for (uint32_t b = ng.cellFirst[cell]; b < ng.cellFirst[cell + 1]; ++b) {
                        if (BondVoxels(node, ng.nodes[b], f) == 0) continue;
                        if (!fn(NodeRef{ni, static_cast<uint16_t>(b)})) return;
                    }
                }
            }

            std::vector<uint8_t> state;
            std::vector<uint32_t> seed;

        private:
            const World& m_World;
            TerrainGraphCache& m_Cache;
            GraphSearchResult& m_Result;
            glm::ivec3 m_Min, m_Max;
            int m_BottomRow = std::numeric_limits<int>::min();
            int m_CeilingRow = std::numeric_limits<int>::max();
            std::vector<Entry> m_Entries;
        };

        /// Reads chunks to re-label island nodes: Dense through the World's pointer, Packed decoded
        /// into at most 16 copies (FIFO; counted).
        class VoxelSource {
        public:
            explicit VoxelSource(const World& world) : m_World(world) {}
            /// The chunk's data (null for a Uniform chunk: `uniformId`).
            const Chunk* Get(const glm::ivec3& chunk, uint8_t& uniformId) {
                uniformId = 0;
                switch (m_World.StateOf(chunk)) {
                    case CellState::Dense: return m_World.GetChunk(chunk);
                    case CellState::Uniform: m_World.GetChunk(chunk, &uniformId); return nullptr;
                    case CellState::Packed: {
                        for (Copy& c : m_Copies)
                            if (c.valid && c.chunk == chunk) return c.data.get();
                        size_t slot = 0;
                        if (m_Copies.size() < kMaxCopies) {
                            m_Copies.push_back({chunk, std::make_unique<Chunk>(), false});
                            slot = m_Copies.size() - 1;
                        } else {
                            slot = m_Next;
                            m_Next = (m_Next + 1) % kMaxCopies;
                        }
                        Copy& c = m_Copies[slot];
                        c.chunk = chunk;
                        c.valid = m_World.ReadChunk(chunk, *c.data);
                        if (!c.valid) return nullptr;
                        ++decoded;
                        return c.data.get();
                    }
                    default: return nullptr;
                }
            }
            uint32_t decoded = 0;

        private:
            static constexpr size_t kMaxCopies = 16;
            struct Copy {
                glm::ivec3 chunk{0};
                std::unique_ptr<Chunk> data;
                bool valid = false;
            };
            const World& m_World;
            std::vector<Copy> m_Copies;
            size_t m_Next = 0;
        };

        bool Inside(const glm::ivec3& v, const glm::ivec3& lo, const glm::ivec3& hiInclusive) {
            return glm::all(glm::greaterThanEqual(v, lo)) && glm::all(glm::lessThanEqual(v, hiInclusive));
        }
        uint64_t ScanIndex(const glm::ivec3& v, const glm::ivec3& boxMin, const glm::ivec3& boxSize) {
            const glm::ivec3 l = v - boxMin;
            return static_cast<uint64_t>(l.x) + static_cast<uint64_t>(boxSize.x) * (static_cast<uint64_t>(l.y) + static_cast<uint64_t>(boxSize.y) * static_cast<uint64_t>(l.z));
        }
    }

    namespace {
    /// One run of the search on the graphs the cache gives (SearchIslandsOnGraph checks them).
    GraphSearchResult SearchOnce(const World& world, TerrainGraphCache& cache, const GraphMaterials& materials,
                                 const GraphSearchLimits& limits) {
        GraphSearchResult result;
        for (int a = 0; a < 3; ++a) {
            const int64_t box = static_cast<int64_t>(limits.boxMax[a]) - limits.boxMin[a];
            const int64_t flood = static_cast<int64_t>(limits.floodMax[a]) - limits.floodMin[a];
            if (box <= 0 || box > kMaxBoxAxis || flood <= 0 || flood > kMaxBoxAxis || limits.boxMin[a] < limits.floodMin[a] ||
                limits.boxMax[a] > limits.floodMax[a]) {
                Log::Warn("SearchIslandsOnGraph: island box {} x {} x {} / flood box {} x {} x {} invalid (flood box must hold the island "
                          "box, at most {} voxels per axis); nothing searched",
                          limits.boxMax.x - limits.boxMin.x, limits.boxMax.y - limits.boxMin.y, limits.boxMax.z - limits.boxMin.z,
                          limits.floodMax.x - limits.floodMin.x, limits.floodMax.y - limits.floodMin.y, limits.floodMax.z - limits.floodMin.z,
                          kMaxBoxAxis);
                return result;
            }
        }
        // The labelled box and the flood box end at the bottom row of a full-height window (the
        // world's ground below it is never loaded): a node on that row is on the face.
        const int bottomY = world.FullHeight() ? world.WindowMinChunk().y * kChunkSize : std::numeric_limits<int>::min();
        const int ceilingRow = world.FullHeight() ? world.WindowMinChunk().y + world.WindowSize().y : std::numeric_limits<int>::max();
        glm::ivec3 boxMin = limits.boxMin;
        const glm::ivec3 boxMax = limits.boxMax;
        if (limits.boxMax.y <= bottomY) {
            result.searched = true;
            result.labelledMin = result.labelledMax = boxMin;
            return result;
        }
        boxMin.y = std::max(boxMin.y, bottomY);
        result.labelledMin = boxMin;
        result.labelledMax = boxMax;
        glm::ivec3 floodMin = limits.floodMin;
        floodMin.y = std::max(floodMin.y, bottomY);
        const glm::ivec3 floodMax = limits.floodMax;
        // Voxels a free piece may hold: the flood box without its face layer.
        const glm::ivec3 innerMin = floodMin + glm::ivec3(1), innerMax = floodMax - glm::ivec3(2);
        const glm::ivec3 boxInnerMin = boxMin + glm::ivec3(1), boxInnerMax = boxMax - glm::ivec3(2);
        const glm::ivec3 boxSize = boxMax - boxMin;

        // Without the whole island box nothing can be proven (ExtractFloatingIslands' rule).
        const glm::ivec3 cMin = VoxelToChunk(boxMin), cMax = VoxelToChunk(boxMax - glm::ivec3(1));
        for (int cz = cMin.z; cz <= cMax.z; ++cz)
            for (int cy = cMin.y; cy <= std::min(cMax.y, ceilingRow - 1); ++cy)
                for (int cx = cMin.x; cx <= cMax.x; ++cx)
                    if (!world.IsChunkLoaded(glm::ivec3(cx, cy, cz))) return result;
        result.searched = true;

        using Clock = std::chrono::steady_clock;
        const auto Ms = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };
        Clock::time_point phase = Clock::now();
        cache.BeginPass(world, materials);
        GraphTable table(world, cache, limits.floodMin, limits.floodMax, result);

        // 1. Seeds: nodes whose bounds meet the box, chunks in (z, y, x) order, nodes in graph order.
        std::vector<NodeRef> seeds;
        for (int cz = cMin.z; cz <= cMax.z; ++cz)
            for (int cy = cMin.y; cy <= std::min(cMax.y, ceilingRow - 1); ++cy)
                for (int cx = cMin.x; cx <= cMax.x; ++cx) {
                    const glm::ivec3 chunk(cx, cy, cz);
                    const uint16_t index = table.IndexOf(chunk);
                    GraphTable::Entry& e = table.Resolve(index, chunk);
                    if (e.kind != GraphTable::Kind::Graph) continue;
                    const glm::ivec3 origin = ChunkToVoxel(chunk);
                    for (uint32_t c = 0; c < static_cast<uint32_t>(kCellsPerChunk); ++c) {
                        const uint32_t first = e.graph->cellFirst[c], last = e.graph->cellFirst[c + 1];
                        if (first == last) continue;
                        const glm::ivec3 cellMin = origin + CellOrigin(c);
                        if (glm::any(glm::greaterThanEqual(cellMin, boxMax)) || glm::any(glm::lessThan(cellMin + glm::ivec3(kCellSize - 1), boxMin)))
                            continue;
                        for (uint32_t n = first; n < last; ++n) {
                            const NodeRef r{index, static_cast<uint16_t>(n)};
                            const glm::ivec3 lo = table.NodeMin(r), hi = table.NodeMax(r);
                            if (glm::any(glm::lessThan(hi, boxMin)) || glm::any(glm::greaterThanEqual(lo, boxMax))) continue;
                            table.seed[table.Slot(r)] = static_cast<uint32_t>(seeds.size());
                            seeds.push_back(r);
                        }
                    }
                }
        result.seedNodes = static_cast<uint32_t>(seeds.size());

        // 2. Box components: union-find over links between seeds (no chunk outside the box is read).
        std::vector<uint32_t> parent(seeds.size());
        std::iota(parent.begin(), parent.end(), 0u);
        const auto Find = [&parent](uint32_t i) {
            while (parent[i] != i) {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }
            return i;
        };
        for (uint32_t i = 0; i < seeds.size(); ++i) {
            const NodeRef r = seeds[i];
            const GraphNode& node = table.Node(r);
            const GraphTable::Entry& e = table.At(r.entry);
            const GraphLink* link = e.graph->links.data() + node.firstLink;
            for (uint16_t k = 0; k < node.linkCount; ++k) {
                const uint32_t other = table.seed[e.base + link[k].node];
                if (other == kNoSeed) continue;
                const uint32_t a = Find(i), b = Find(other);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            }
            for (uint8_t f = 1; f < 6; f += 2) {             // across a chunk face: the + faces, each pair once
                if (node.face[f] == 0 || NeighbourCell(node.cell, f) >= 0) continue;
                glm::ivec3 next = e.chunk;
                next[f >> 1] += 1;
                if (!Inside(next, cMin, cMax)) continue;
                const GraphTable::Entry& n = table.At(table.IndexOf(next));
                if (n.kind != GraphTable::Kind::Graph) continue;
                const uint32_t cell = AcrossChunkFace(node.cell, f);
                for (uint32_t b = n.graph->cellFirst[cell]; b < n.graph->cellFirst[cell + 1]; ++b) {
                    const uint32_t other = table.seed[n.base + b];
                    if (other == kNoSeed || BondVoxels(node, n.graph->nodes[b], f) == 0) continue;
                    const uint32_t x = Find(i), y = Find(other);
                    if (x != y) parent[std::max(x, y)] = std::min(x, y);
                }
            }
        }
        // Roots are the smallest seed of each component (unions keep the smaller index).
        struct BoxComponent {
            uint32_t root = 0;
            uint64_t voxels = 0;
            bool interior = true;     // every node inside the box without its face layer
        };
        std::vector<uint32_t> componentOf(seeds.size());
        std::vector<BoxComponent> components;
        {
            std::vector<uint32_t> indexOfRoot(seeds.size(), kNoSeed);
            for (uint32_t i = 0; i < seeds.size(); ++i) {
                const uint32_t root = Find(i);
                if (indexOfRoot[root] == kNoSeed) {
                    indexOfRoot[root] = static_cast<uint32_t>(components.size());
                    components.push_back({root, 0, true});
                }
                BoxComponent& c = components[indexOfRoot[root]];
                componentOf[i] = indexOfRoot[root];
                c.voxels += table.Node(seeds[i]).voxels;
                c.interior = c.interior && Inside(table.NodeMin(seeds[i]), boxInnerMin, boxInnerMax) &&
                             Inside(table.NodeMax(seeds[i]), boxInnerMin, boxInnerMax);
            }
        }
        result.boxComponents = static_cast<uint32_t>(components.size());
        std::vector<uint32_t> order(components.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&components](uint32_t a, uint32_t b) {
            return components[a].voxels != components[b].voxels ? components[a].voxels < components[b].voxels : components[a].root < components[b].root;
        });

        // Over the limit: never an island; their seeds stop every flood that reaches them.
        std::vector<std::vector<NodeRef>> islands;
        std::vector<uint8_t> islandInterior;
        for (uint32_t i = 0; i < seeds.size(); ++i) {
            const BoxComponent& c = components[componentOf[i]];
            if (c.voxels > limits.maxIslandVoxels) table.state[table.Slot(seeds[i])] = kAttached;
        }
        for (const BoxComponent& c : components) {
            if (c.voxels <= limits.maxIslandVoxels) continue;
            if (c.interior) ++result.oversizeIslands;
            else ++result.faceOversize;
        }
        // Inside the box without touching its face: a whole component, an island without a flood.
        {
            std::vector<std::vector<NodeRef>> members(components.size());
            for (uint32_t i = 0; i < seeds.size(); ++i) {
                const BoxComponent& c = components[componentOf[i]];
                if (c.interior && c.voxels <= limits.maxIslandVoxels) members[componentOf[i]].push_back(seeds[i]);
            }
            for (uint32_t ci = 0; ci < components.size(); ++ci) {
                if (members[ci].empty()) continue;
                for (const NodeRef& r : members[ci]) table.state[table.Slot(r)] = kIsland;
                islands.push_back(std::move(members[ci]));
                islandInterior.push_back(1);
            }
        }

        result.seedMs = Ms(phase);
        phase = Clock::now();
        // 3. Floods from the other components, smallest first.
        std::vector<NodeRef> queue;
        bool budgetSpent = false, deferred = false;
        enum class Outcome { Open, Limit, Face, Known, Budget, Unloaded };
        for (const uint32_t ci : order) {
            const BoxComponent& c = components[ci];
            if (c.voxels > limits.maxIslandVoxels || c.interior) continue;
            const NodeRef seed = seeds[c.root];
            const uint8_t st = table.state[table.Slot(seed)];
            if (st == kIsland) {
                ++result.merged;
                continue;
            }
            if (st == kAttached) {
                ++result.attachedByKnown;
                continue;
            }
            if (budgetSpent) {
                ++result.attachedByBudget;
                continue;
            }
            if (deferred) continue;           // offline: no face island is returned any more
            ++result.floods;
            queue.clear();
            uint64_t voxels = 0;
            Outcome outcome = Outcome::Open;
            const auto Enqueue = [&](const NodeRef& r) {
                table.state[table.Slot(r)] = kVisiting;
                queue.push_back(r);
                ++result.nodeVisits;
                voxels += table.Node(r).voxels;
                if (result.nodeVisits > limits.maxNodeVisits) outcome = Outcome::Budget;
                else if (!Inside(table.NodeMin(r), innerMin, innerMax) || !Inside(table.NodeMax(r), innerMin, innerMax)) outcome = Outcome::Face;
                else if (voxels > limits.maxIslandVoxels) outcome = Outcome::Limit;
            };
            Enqueue(seed);
            for (size_t head = 0; head < queue.size() && outcome == Outcome::Open; ++head) {
                const NodeRef r = queue[head];
                table.ForEachLink(
                    r,
                    [&](const NodeRef& m) {
                        const uint8_t s = table.state[table.Slot(m)];
                        if (s == kVisiting || s == kIsland) return true;
                        if (s == kAttached) {
                            outcome = Outcome::Known;
                            return false;
                        }
                        Enqueue(m);
                        return outcome == Outcome::Open;
                    },
                    [&](const glm::ivec3& chunk) {
                        outcome = Outcome::Unloaded;
                        if (limits.deferOnUnloaded && result.complete) {
                            result.complete = false;
                            result.unloadedChunk = chunk;
                        }
                    });
            }
            if (outcome == Outcome::Open) {
                for (const NodeRef& r : queue) table.state[table.Slot(r)] = kIsland;
                islands.push_back(queue);
                islandInterior.push_back(0);
                continue;
            }
            for (const NodeRef& r : queue) table.state[table.Slot(r)] = kAttached;
            switch (outcome) {
                case Outcome::Limit: ++result.attachedByLimit; break;
                case Outcome::Face: ++result.attachedByFace; break;
                case Outcome::Known: ++result.attachedByKnown; break;
                case Outcome::Budget: ++result.attachedByBudget; budgetSpent = true; break;
                case Outcome::Unloaded:
                    if (limits.deferOnUnloaded) deferred = true;
                    else ++result.attachedByUnloaded;
                    break;
                case Outcome::Open: break;
            }
        }

        // 4. Island voxels: only released nodes are re-labelled per voxel, then each island is
        //    placed where the voxel search finds it (in-box islands by first voxel; face islands by
        //    their smallest in-box component, ties by its first voxel).
        result.floodMs = Ms(phase);
        phase = Clock::now();
        VoxelSource source(world);
        struct Placed {
            FloatingIsland island;
            uint64_t count = 0, scan = 0;
            glm::ivec3 inMin{0}, inMax{0};      // bounds of its voxels inside the island box
        };
        std::vector<Placed> boxPlaced, facePlaced;
        for (size_t k = 0; k < islands.size(); ++k) {
            if (!result.complete && islandInterior[k] == 0) continue;
            // Node bounds are tight: their union is the island's bounds.
            glm::ivec3 mn = table.NodeMin(islands[k][0]), mx = table.NodeMax(islands[k][0]);
            for (const NodeRef& r : islands[k]) {
                mn = glm::min(mn, table.NodeMin(r));
                mx = glm::max(mx, table.NodeMax(r));
            }
            Placed placed;
            placed.island.origin = mn;
            placed.island.volume = VoxelVolume(mx - mn + glm::ivec3(1));
            const glm::ivec3 size = placed.island.volume.Size();
            std::vector<uint8_t>& out = placed.island.volume.Data();
            glm::ivec3 inMin(std::numeric_limits<int>::max()), inMax(std::numeric_limits<int>::min());
            bool interior = true;
            uint64_t inBox = 0, firstScan = std::numeric_limits<uint64_t>::max();
            uint32_t count = 0;
            for (const NodeRef& r : islands[k]) {
                const GraphTable::Entry& e = table.At(r.entry);
                const GraphNode& node = e.graph->nodes[r.node];
                uint8_t uniformId = 0;
                const Chunk* data = source.Get(e.chunk, uniformId);
                const Slices mask = NodeVoxels(data, uniformId, node, materials);
                ++result.relabelledNodes;
                {
                    // The voxels flooded now must be the node's: the same count, face masks (which
                    // decide its links) and bounds (which size the island volume). Otherwise the
                    // graph is stale: nothing of it is written (the caller searches again). A stale
                    // neighbour node is not seen here.
                    GraphNode now;
                    Faces(mask, now);
                    uint32_t voxels = 0;
                    for (const uint64_t slice : mask) voxels += static_cast<uint32_t>(std::popcount(slice));
                    if (voxels != node.voxels || !std::equal(std::begin(now.face), std::end(now.face), std::begin(node.face)) ||
                        !std::equal(std::begin(now.boxMin), std::end(now.boxMin), std::begin(node.boxMin)) ||
                        !std::equal(std::begin(now.boxMax), std::end(now.boxMax), std::begin(node.boxMax))) {
                        ++result.relabelMismatch;
                        continue;
                    }
                }
                const glm::ivec3 cell = CellOrigin(node.cell);
                const glm::ivec3 o = ChunkToVoxel(e.chunk) + cell;
                // A node inside the box interior needs no per-voxel box test.
                const glm::ivec3 nodeMin = table.NodeMin(r), nodeMax = table.NodeMax(r);
                const bool nodeInterior = Inside(nodeMin, boxInnerMin, boxInnerMax) && Inside(nodeMax, boxInnerMin, boxInnerMax);
                interior = interior && nodeInterior;
                uint32_t n = 0;
                for (int z = 0; z < 8; ++z)
                    for (uint64_t bits = mask[static_cast<size_t>(z)]; bits != 0; bits &= bits - 1) {
                        const int b = std::countr_zero(bits);
                        const glm::ivec3 l(b & 7, b >> 3, z);
                        const glm::ivec3 v = o + l, d = v - mn;
                        const glm::ivec3 cl = cell + l;
                        out[static_cast<size_t>(d.x) + static_cast<size_t>(size.x) * (static_cast<size_t>(d.y) + static_cast<size_t>(size.y) * static_cast<size_t>(d.z))] =
                            data ? data->Get(cl.x, cl.y, cl.z) : uniformId;
                        ++n;
                        if (nodeInterior || Inside(v, boxMin, boxMax - glm::ivec3(1))) {
                            ++inBox;
                            inMin = glm::min(inMin, v);
                            inMax = glm::max(inMax, v);
                            firstScan = std::min(firstScan, ScanIndex(v, boxMin, boxSize));
                        }
                    }
                count += n;
            }
            placed.island.voxelCount = count;
            if (inBox == 0) {
                ++result.outsideBox;
                continue;
            }
            if (interior) {
                placed.scan = firstScan;
                boxPlaced.push_back(std::move(placed));
                continue;
            }
            placed.inMin = inMin;
            placed.inMax = inMax;
            facePlaced.push_back(std::move(placed));
        }
        // Face islands are ordered as the face flood proves them: by their smallest in-box
        // component (the island box labels would split an island there), ties by its first voxel.
        // Only needed to order two or more.
        if (facePlaced.size() > 1 && result.complete) {
            for (Placed& placed : facePlaced) {
                const FloatingIsland& island = placed.island;
                VoxelVolume part(placed.inMax - placed.inMin + glm::ivec3(1));
                // Only the island's in-box bounds are read: every voxel there is inside the (convex) box.
                const glm::ivec3 lo = placed.inMin - island.origin, hi = placed.inMax - island.origin;
                for (int z = lo.z; z <= hi.z; ++z)
                    for (int y = lo.y; y <= hi.y; ++y)
                        for (int x = lo.x; x <= hi.x; ++x)
                            if (island.volume.Get(x, y, z) != 0) part.Set(glm::ivec3(x, y, z) - lo, 1);
                const ComponentLabels labels = LabelComponents(part);
                placed.count = std::numeric_limits<uint64_t>::max();
                const glm::ivec3 size = part.Size();
                for (uint32_t c = 1; c <= labels.count; ++c) {
                    const uint32_t f = labels.firstIndex[c];
                    const glm::ivec3 l(static_cast<int>(f % static_cast<uint32_t>(size.x)),
                                       static_cast<int>((f / static_cast<uint32_t>(size.x)) % static_cast<uint32_t>(size.y)),
                                       static_cast<int>(f / (static_cast<uint32_t>(size.x) * static_cast<uint32_t>(size.y))));
                    const uint64_t count = labels.voxelCounts[c], scan = ScanIndex(placed.inMin + l, boxMin, boxSize);
                    if (count < placed.count || (count == placed.count && scan < placed.scan)) {
                        placed.count = count;
                        placed.scan = scan;
                    }
                }
            }
        }
        std::sort(boxPlaced.begin(), boxPlaced.end(), [](const Placed& a, const Placed& b) { return a.scan < b.scan; });
        std::sort(facePlaced.begin(), facePlaced.end(),
                  [](const Placed& a, const Placed& b) { return a.count != b.count ? a.count < b.count : a.scan < b.scan; });
        for (Placed& p : boxPlaced) result.boxIslands.push_back(std::move(p.island));
        if (result.complete)
            for (Placed& p : facePlaced) result.faceIslands.push_back(std::move(p.island));
        result.chunksDecoded = source.decoded;
        result.relabelMs = Ms(phase);
        return result;
    }
    }

    GraphSearchResult SearchIslandsOnGraph(const World& world, TerrainGraphCache& cache, const GraphMaterials& materials,
                                           const GraphSearchLimits& limits) {
        GraphSearchResult result = SearchOnce(world, cache, materials, limits);
        if (result.relabelMismatch == 0) return result;
        // A stale graph (an edit path that missed World::ChunkRevision): its islands may hold voxels
        // that are attached now, so none is emitted from it. The search runs again on graphs built
        // from the current voxels.
        const uint32_t stale = result.relabelMismatch;
        const uint64_t built = result.graphsBuilt;
        cache.Clear();
        result = SearchOnce(world, cache, materials, limits);
        result.graphsBuilt += built;
        result.staleGraphRetries = 1;
        Log::Error("SearchIslandsOnGraph: {} island node(s) re-labelled with other voxels than their chunk graph (a stale graph: "
                   "World::ChunkRevision missed a change); searched again on fresh graphs: {}",
                   stale, result.relabelMismatch == 0 ? "consistent" : "still inconsistent, no island returned");
        if (result.relabelMismatch != 0) {
            result.boxIslands.clear();
            result.faceIslands.clear();
        }
        result.relabelMismatch += stale;
        return result;
    }
}
