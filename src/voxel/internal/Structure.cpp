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
#include <span>
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

        /// Per id: its density in bits 0-31, bit 32 set for an ignored id (bits 32-39 count them over
        /// a block), the id itself in bits 40-47 when it is solid (0 for air and ignored ids).
        /// Summed over a block in one pass (bits 0-39): the mass and whether anything was ignored.
        using DensityTable = std::array<uint64_t, 256>;
        constexpr uint64_t kSumMask = (uint64_t{1} << 40) - 1;
        DensityTable MakeDensityTable(const GraphMaterials& materials) {
            DensityTable t{};
            for (uint32_t id = 1; id < 256; ++id)
                t[id] = materials.ignored[id] ? (uint64_t{1} << 32) : (static_cast<uint64_t>(materials.densityQ[id]) | (uint64_t{id} << 40));
            return t;
        }

        /// The solid (not ignored) voxels of a 4^3 block (bit lx + 4 ly + 16 lz) and their density
        /// sum; `blockId` = the one solid id of the block, -1 if it holds several (0: none).
        using IdSet = std::array<uint64_t, 4>;     // a bit per material id

        uint64_t BlockSolid(const Chunk& chunk, uint32_t b, const GraphMaterials& materials, const DensityTable* table, uint32_t& mass,
                            int& blockId, IdSet* idSet = nullptr) {
            const uint64_t occupied = chunk.occ.voxelMask[b];
            mass = 0;
            blockId = 0;
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
                blockId = ids[0];
                if (idSet) (*idSet)[ids[0] >> 6] |= uint64_t{1} << (ids[0] & 63u);
                return occupied;
            }
            if (table != nullptr) {
                uint64_t sum = 0;
                IdSet seen{};                     // solid ids (bit 0: air and ignored ids)
                for (uint32_t l = 0; l < 64; ++l) {
                    const uint64_t e = (*table)[ids[l]];
                    sum += e & kSumMask;
                    const uint32_t id = static_cast<uint32_t>(e >> 40);
                    seen[id >> 6] |= uint64_t{1} << (id & 63u);
                }
                seen[0] &= ~uint64_t{1};
                mass = static_cast<uint32_t>(sum);
                const uint32_t distinct = static_cast<uint32_t>(std::popcount(seen[0]) + std::popcount(seen[1]) + std::popcount(seen[2]) +
                                                                std::popcount(seen[3]));
                blockId = 0;
                if (distinct > 1) blockId = -1;
                else
                    for (uint32_t w = 0; w < 4 && distinct == 1; ++w)
                        if (seen[w] != 0) blockId = static_cast<int>(std::countr_zero(seen[w]) + 64u * w);
                if (idSet)
                    for (uint32_t w = 0; w < 4; ++w) (*idSet)[w] |= seen[w];
                if ((sum >> 32) == 0) return occupied;        // nothing ignored: the occupancy is the answer
            }
            uint64_t m = 0;
            for (uint32_t l = 0; l < 64; ++l) {
                const uint8_t id = ids[l];
                m |= static_cast<uint64_t>(id != 0 && !materials.ignored[id]) << l;
            }
            return m;
        }

        /// The solid (not ignored) voxels of cell `cell` of a chunk, and the sum of their densities;
        /// `cellId` = the one solid id of the cell, -1 if it holds several (with a table only).
        void CellSolid(const Chunk& chunk, uint32_t cell, const GraphMaterials& materials, const DensityTable* table, Slices& out,
                       uint32_t* mass, int* cellId = nullptr, IdSet* cellIds = nullptr) {
            out = {};
            uint32_t sum = 0;
            int one = 0;
            const uint32_t cx = cell & 3u, cy = (cell >> 2) & 3u, cz = cell >> 4;
            for (uint32_t dz = 0; dz < 2; ++dz)
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        const uint32_t bx = cx * 2 + dx, by = cy * 2 + dy, bz = cz * 2 + dz;
                        const uint32_t b = bx + (by << 3) + (bz << 6);
                        uint32_t blockMass = 0;
                        int blockId = 0;
                        const uint64_t m = BlockSolid(chunk, b, materials, table, blockMass, blockId, cellIds);
                        sum += blockMass;
                        if (m == 0) continue;
                        if (blockId < 0 || (one != 0 && one != blockId)) one = -1;
                        else if (one == 0) one = blockId;
                        const uint32_t xo = dx * 4, yo = dy * 4, zo = dz * 4;
                        for (uint32_t lz = 0; lz < 4; ++lz) {
                            // Four rows of 4 bits -> four rows of the slice, at x offset xo.
                            const uint64_t rows = (m >> (16 * lz)) & 0xFFFFu;
                            const uint64_t spread = (rows & 0xFu) | ((rows & 0xF0u) << 4) | ((rows & 0xF00u) << 8) | ((rows & 0xF000u) << 12);
                            out[zo + lz] |= spread << (xo + 8 * yo);
                        }
                    }
            if (mass) *mass = sum;
            if (cellId) *cellId = one;
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

        /// Per StrengthClass the weakest of a set of ids (lowest strength, ties to the smaller id).
        /// Foliage counts only when the set holds nothing else: a leaf touching a wall does not
        /// rate the wall (shred.md "Stage 4b review"); a node of leaves alone keeps their strength.
        void WeakestOf(const std::array<uint64_t, 4>& ids, const GraphMaterials& materials, GraphNode& node) {
            bool other = false;
            for (uint32_t w = 0; w < 4 && !other; ++w)
                for (uint64_t bits = ids[w]; bits != 0 && !other; bits &= bits - 1)
                    other = !materials.foliage[static_cast<size_t>(std::countr_zero(bits)) + 64u * w];
            for (uint32_t k = 0; k < 3; ++k) {
                uint64_t best = ~uint64_t{0};
                for (uint32_t w = 0; w < 4; ++w)
                    for (uint64_t bits = ids[w]; bits != 0; bits &= bits - 1) {
                        const uint32_t id = static_cast<uint32_t>(std::countr_zero(bits)) + 64u * w;
                        if (other && materials.foliage[id]) continue;
                        best = std::min(best, (static_cast<uint64_t>(materials.strength[k][id]) << 8) | id);
                    }
                node.weak[k] = static_cast<uint8_t>(best & 0xFFu);
            }
        }

        /// A node holding every id of `ids`: its weakest materials and its anchor bit.
        void SetMaterials(GraphNode& node, const IdSet& ids, const GraphMaterials& materials) {
            WeakestOf(ids, materials, node);
            for (uint32_t w = 0; w < 4; ++w)
                for (uint64_t bits = ids[w]; bits != 0; bits &= bits - 1)
                    if (materials.anchor[static_cast<size_t>(std::countr_zero(bits)) + 64u * w]) node.cutPlane |= kCutAnchorBit;
        }

        /// One id for every voxel of a node: its weakest materials and anchor bit are that id's.
        void SetSingleMaterial(GraphNode& node, uint8_t id, const GraphMaterials& materials) {
            node.weak[0] = node.weak[1] = node.weak[2] = id;
            if (materials.anchor[id]) node.cutPlane |= kCutAnchorBit;
        }

        /// A node of a cell with several ids: its mass, weakest materials and anchor bit from the
        /// set of ids its voxels hold (one table lookup per voxel).
        uint32_t NodeMaterials(const Chunk& chunk, uint32_t cell, const Slices& n, const GraphMaterials& materials, GraphNode& node) {
            const glm::ivec3 o = CellOrigin(cell);
            uint32_t sum = 0;
            std::array<uint64_t, 4> seen{};
            for (int z = 0; z < 8; ++z)
                for (uint64_t bits = n[z]; bits != 0; bits &= bits - 1) {
                    const int i = std::countr_zero(bits);
                    const uint8_t id = chunk.Get(o.x + (i & 7), o.y + (i >> 3), o.z + z);
                    sum += materials.densityQ[id];
                    seen[id >> 6] |= uint64_t{1} << (id & 63u);
                }
            SetMaterials(node, seen, materials);
            return sum;
        }

        /// Bytewise popcount: each byte of the result holds the number of bits of that byte.
        constexpr uint64_t BytePopcount(uint64_t x) {
            x = x - ((x >> 1) & 0x5555555555555555ull);
            x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
            return (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
        }

        /// Layers y <= k of a slice (bit x + 8y).
        constexpr uint64_t LayersUpTo(uint32_t k) { return k >= 7 ? kFull : (uint64_t{1} << (8u * (k + 1u))) - 1u; }

        /// shred.md Part 4b "The narrowest plane": the lowest horizontal plane k (between layers k
        /// and k + 1) with the fewest voxel faces across it that is a real constriction (narrower
        /// than the widest layer on both sides) and leaves both halves 6-connected: cutFaces and
        /// cutPlane bits 0-2; otherwise cutFaces stays kNoCut.
        void PlaneCut(const Slices& n, GraphNode& node) {
            const uint32_t ymin = node.boxMin[1], ymax = node.boxMax[1];
            if (ymin >= ymax) return;
            uint64_t cuts = 0, areas = 0;
            for (int z = 0; z < 8; ++z) {
                cuts += BytePopcount(n[z] & (n[z] >> 8));
                areas += BytePopcount(n[z]);
            }
            const auto Byte = [](uint64_t v, uint32_t k) { return static_cast<uint32_t>((v >> (8u * k)) & 0xFFu); };
            uint32_t minC = 255;
            for (uint32_t k = ymin; k < ymax; ++k) minC = std::min(minC, Byte(cuts, k));
            uint32_t lowerMax = 0;
            for (uint32_t k = ymin; k < ymax; ++k) {
                lowerMax = std::max(lowerMax, Byte(areas, k));
                if (Byte(cuts, k) != minC) continue;
                uint32_t upperMax = 0;
                for (uint32_t y = k + 1; y <= ymax; ++y) upperMax = std::max(upperMax, Byte(areas, y));
                if (minC >= std::min(lowerMax, upperMax)) continue;
                // k*: both halves must be one component each (a U inside one cell is not split).
                const uint64_t low = LayersUpTo(k);
                for (const uint64_t keep : {low, ~low}) {
                    Slices half{};
                    Slices seed{};
                    bool seeded = false;
                    for (int z = 0; z < 8; ++z) {
                        half[static_cast<size_t>(z)] = n[static_cast<size_t>(z)] & keep;
                        if (!seeded && half[static_cast<size_t>(z)] != 0) {
                            seed[static_cast<size_t>(z)] = half[static_cast<size_t>(z)] & (~half[static_cast<size_t>(z)] + 1);
                            seeded = true;
                        }
                    }
                    if (FloodCell(seed, half) != half) return;
                }
                node.cutFaces = static_cast<uint8_t>(minC);
                node.cutPlane = static_cast<uint8_t>((node.cutPlane & kCutAnchorBit) | k);
                return;
            }
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
        const Material stone{};
        for (uint32_t k = 0; k < 3; ++k) out.strength[k].fill(stone.strength[k]);
        for (uint32_t k = 0; k < 3; ++k) out.strength[k][0] = 0;
        for (uint32_t id = 1; id < 256; ++id) {
            if (materials == nullptr) {
                out.densityQ[id] = 1000;
                continue;
            }
            const Material& m = materials->Get(kWorldPalette, static_cast<uint8_t>(id));
            out.ignored[id] = (m.flags & ignoreFlags) != 0;
            out.anchor[id] = !out.ignored[id] && (m.flags & MaterialFlag_Indestructible) != 0;
            const float d = std::isfinite(m.density) ? std::clamp(m.density, 0.0f, 65535.0f) : 0.0f;
            // Foliage weighs nothing for the load check: a canopy never overloads its trunk.
            out.foliage[id] = !out.ignored[id] && (m.flags & MaterialFlag_Foliage) != 0;
            const bool weightless = out.ignored[id] || out.foliage[id];
            out.densityQ[id] = weightless ? uint16_t{0} : static_cast<uint16_t>(std::lround(d));
            for (uint32_t k = 0; k < 3; ++k) out.strength[k][id] = m.strength[k];
        }
        return out;
    }

    uint32_t VoxelSizeMicro(float voxelSize) {
        const double micro = std::isfinite(voxelSize) ? static_cast<double>(voxelSize) * 1.0e6 : 0.0;
        return static_cast<uint32_t>(std::clamp<long long>(std::llround(micro), 1, std::numeric_limits<int32_t>::max()));
    }

    NeckLimits MakeNeckLimits(const GraphMaterials& materials, float voxelSize) {
        NeckLimits out;
        const uint64_t micro = VoxelSizeMicro(voxelSize);
        for (uint32_t k = 0; k < 3; ++k)
            for (uint32_t id = 0; id < 256; ++id) out.capFace[k][id] = static_cast<uint64_t>(materials.strength[k][id]) * 1000000u / micro;
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
            for (GraphNode& node : out.nodes) {
                node.mass = mass;
                SetSingleMaterial(node, uniformId, materials);       // full cells are never constricted
            }
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
            int cellId = 0;
            IdSet cellIds{};
            CellSolid(*chunk, c, materials, &table, solid, &cellMass, &cellId, &cellIds);
            bool full = true, any = false;
            for (const uint64_t s : solid) {
                full = full && s == kFull;
                any = any || s != 0;
            }
            if (!any) continue;
            if (full) {
                AddNode(out, c, solid, cellMass);
                GraphNode& node = out.nodes.back();
                if (cellId > 0) SetSingleMaterial(node, static_cast<uint8_t>(cellId), materials);
                else SetMaterials(node, cellIds, materials);         // the only node holds every id of the cell
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
                    // The only node of its cell has the whole cell's mass; one id: the count tells it.
                    AddNode(out, c, component, 0);
                    GraphNode& node = out.nodes.back();
                    if (cellId > 0) {
                        node.mass = (nodes == 0 && last) ? cellMass : static_cast<uint32_t>(node.voxels) * materials.densityQ[cellId];
                        SetSingleMaterial(node, static_cast<uint8_t>(cellId), materials);
                    } else if (nodes == 0 && last) {
                        node.mass = cellMass;                         // the only node: every id of the cell
                        SetMaterials(node, cellIds, materials);
                    } else {
                        node.mass = NodeMaterials(*chunk, c, component, materials, node);
                    }
                    PlaneCut(component, node);
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


    // --- the load check (docs/design/shred.md Part 4b) ------------------------------------------

    namespace {
        /// Every finite capacity c is used as c x 2^17 + 1 and every supply as mass x 2^17: a set A
        /// then keeps more supply than its out-arcs carry exactly when mass(A) > cap(A -> out) (the
        /// network has fewer than 2^17 arcs), so a load equal to its capacity holds.
        constexpr uint64_t kPerturb = uint64_t{1} << 17;
        constexpr uint64_t kInfiniteResidual = uint64_t{1} << 62;
        constexpr uint32_t kFlowSink = 0xFFFFFFFFu;
        uint64_t Perturbed(uint64_t cap) { return cap == NeckNetwork::kInfinite || cap >= kInfiniteResidual / kPerturb ? kInfiniteResidual : cap * kPerturb + 1; }

        struct FlowPair {
            uint32_t a = 0, b = 0;        ///< b == kFlowSink: an arc to the sink
            uint64_t ab = 0, ba = 0;      ///< perturbed residual capacities
        };
        struct FlowProblem {
            uint32_t nodes = 0;
            std::vector<uint64_t> supply;  ///< perturbed
            std::vector<FlowPair> pairs;
            uint32_t shuffleSeed = 0;
        };
        struct FlowOutcome {
            bool solved = false;
            bool allReached = false;       ///< every supply reached the sink: nothing is overloaded
            uint64_t work = 0;             ///< arc scans
            std::vector<uint8_t> reach;    ///< per node: can reach the sink in the final residual graph
        };

        /// A deterministic permutation (tests: NeckLimits::shuffleForTesting).
        template <typename T>
        void Shuffle(std::vector<T>& v, size_t from, size_t to, uint64_t& state) {
            for (size_t i = to; i > from + 1; --i) {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                const size_t j = from + static_cast<size_t>((state >> 33) % (i - from));
                std::swap(v[i - 1], v[j]);
            }
        }

        /// A maximum preflow by push-relabel (shred.md Part 4b step 4), all integer, fixed order:
        /// exact labels by a reverse breadth-first search from the sink; active nodes first by label
        /// descending, ties by index (the "gravity sweep": in a structure that holds, all mass
        /// flows down in this one pass), then FIFO; current-arc pointers in arc order; a global
        /// relabel every max(64, n / 4) relabels. Work = arc scans; over `maxWork` it gives up.
        FlowOutcome MaxPreflow(const FlowProblem& p, uint64_t maxWork) {
            FlowOutcome out;
            const uint32_t n = p.nodes + 1, t = p.nodes;
            // Arcs: pair k is arc 2k (a -> b) and 2k + 1 (b -> a); CSR per node in pair order.
            std::vector<uint32_t> head(p.pairs.size() * 2);
            std::vector<uint64_t> residual(p.pairs.size() * 2);
            std::vector<uint32_t> first(static_cast<size_t>(n) + 1, 0);
            for (size_t k = 0; k < p.pairs.size(); ++k) {
                const FlowPair& e = p.pairs[k];
                const uint32_t b = e.b == kFlowSink ? t : e.b;
                head[2 * k] = b;
                head[2 * k + 1] = e.a;
                residual[2 * k] = e.ab;
                residual[2 * k + 1] = e.b == kFlowSink ? 0 : e.ba;
                ++first[e.a + 1];
                ++first[b + 1];
            }
            for (uint32_t v = 0; v < n; ++v) first[v + 1] += first[v];
            std::vector<uint32_t> arcs(first[n]);
            {
                std::vector<uint32_t> fill(first.begin(), first.end() - 1);
                for (size_t k = 0; k < p.pairs.size(); ++k) {
                    const FlowPair& e = p.pairs[k];
                    arcs[fill[e.a]++] = static_cast<uint32_t>(2 * k);
                    arcs[fill[e.b == kFlowSink ? t : e.b]++] = static_cast<uint32_t>(2 * k + 1);
                }
            }
            uint64_t shuffle = p.shuffleSeed;
            if (shuffle != 0)
                for (uint32_t v = 0; v < n; ++v) Shuffle(arcs, first[v], first[v + 1], shuffle);

            std::vector<uint32_t> label(n, n);
            std::vector<uint32_t> bfs;
            bfs.reserve(n);
            uint64_t work = 0;
            // Exact distances to the sink over arcs with residual capacity; n = cannot reach it.
            const auto GlobalRelabel = [&] {
                std::fill(label.begin(), label.end(), n);
                label[t] = 0;
                bfs.clear();
                bfs.push_back(t);
                for (size_t h = 0; h < bfs.size(); ++h) {
                    const uint32_t w = bfs[h];
                    for (uint32_t i = first[w]; i < first[w + 1]; ++i) {
                        ++work;
                        const uint32_t a = arcs[i];
                        const uint32_t x = head[a];
                        if (label[x] == n && residual[a ^ 1u] > 0) {
                            label[x] = label[w] + 1;
                            bfs.push_back(x);
                        }
                    }
                }
            };
            GlobalRelabel();
            std::vector<uint64_t> excess(n, 0);
            uint64_t total = 0;
            for (uint32_t v = 0; v < p.nodes; ++v) {
                excess[v] = p.supply[v];
                total += p.supply[v];
            }
            std::vector<uint32_t> queue;
            std::vector<uint8_t> queued(n, 0);
            for (uint32_t v = 0; v < p.nodes; ++v)
                if (excess[v] > 0 && label[v] < n) queue.push_back(v);
            std::stable_sort(queue.begin(), queue.end(), [&label](uint32_t a, uint32_t b) { return label[a] > label[b]; });
            if (shuffle != 0) Shuffle(queue, 0, queue.size(), shuffle);
            for (const uint32_t v : queue) queued[v] = 1;
            std::vector<uint32_t> current(first.begin(), first.end() - 1);
            const uint64_t relabelPeriod = std::max<uint64_t>(64, n / 4);
            uint64_t relabels = 0;
            for (size_t h = 0; h < queue.size(); ++h) {
                const uint32_t v = queue[h];
                queued[v] = 0;
                while (excess[v] > 0 && label[v] < n) {
                    if (work > maxWork) return out;
                    if (current[v] == first[v + 1]) {
                        // Relabel: one above the lowest neighbour with residual capacity.
                        uint32_t lowest = n;
                        for (uint32_t i = first[v]; i < first[v + 1]; ++i) {
                            ++work;
                            if (residual[arcs[i]] > 0) lowest = std::min(lowest, label[head[arcs[i]]]);
                        }
                        label[v] = lowest + 1 >= n ? n : lowest + 1;
                        current[v] = first[v];
                        if (++relabels % relabelPeriod == 0) {
                            GlobalRelabel();
                            std::copy(first.begin(), first.end() - 1, current.begin());
                        }
                        continue;
                    }
                    const uint32_t a = arcs[current[v]];
                    const uint32_t w = head[a];
                    ++work;
                    if (residual[a] > 0 && label[v] == label[w] + 1) {
                        const uint64_t d = std::min(excess[v], residual[a]);
                        residual[a] -= d;
                        residual[a ^ 1u] += d;
                        excess[v] -= d;
                        excess[w] += d;
                        if (w != t && !queued[w] && label[w] < n) {
                            queued[w] = 1;
                            queue.push_back(w);
                        }
                    } else {
                        ++current[v];
                    }
                }
            }
            if (work > maxWork) return out;
            out.solved = true;
            out.allReached = excess[t] == total;
            if (!out.allReached) {
                // Who can still reach the sink: the kept side. The rest is the released set, the
                // same for every maximum preflow (shred.md Part 4b "The rule, exactly").
                GlobalRelabel();
                out.reach.resize(p.nodes);
                for (uint32_t v = 0; v < p.nodes; ++v) out.reach[v] = label[v] < n ? 1 : 0;
            }
            out.work = work;
            return out;
        }
    }

    // --- the search -----------------------------------------------------------------------------

    namespace {
        constexpr int kMaxBoxAxis = 192;                       // as ExtractFloatingIslands / ProveFaceComponents
        constexpr int kTableAxis = kMaxBoxAxis / kChunkSize + 1;
        constexpr uint8_t kUnvisited = 0, kVisiting = 1, kIsland = 2, kAttached = 3;
        constexpr uint32_t kNoSeed = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t kNeckNone = 0xFFFFFFFFu, kNeckAnchor = 0xFFFFFFFEu, kNeckVisiting = 0xFFFFFFFDu, kNeckLoad = 0x80000000u;

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
                int8_t solid = -1;              // SolidThrough: -1 not asked yet
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
                neck.resize(state.size(), kNeckNone);
                ++m_Result.chunkGraphs;
                m_Result.overfullCells += graph->overfullCells;
                return e;
            }
            Entry& Resolve(const glm::ivec3& chunk) { return Resolve(IndexOf(chunk), chunk); }

            /// Every voxel of the chunk solid (not air, not ignored): a pure function of its voxels,
            /// answered without building its graph when it has none yet (Uniform: its id; Dense: the
            /// occupancy, then the ids only when it is full; Packed: the RLE runs). Not loaded: false.
            bool SolidThrough(uint16_t index, const glm::ivec3& chunk) {
                Entry& e = m_Entries[index];
                if (e.solid >= 0) return e.solid != 0;
                bool solid = false;
                if (e.kind == Kind::Graph) {
                    solid = e.graph->solidVoxels == static_cast<uint32_t>(kChunkVolume);
                } else if (e.kind == Kind::Unresolved && chunk.y < m_CeilingRow && chunk.y >= m_BottomRow) {
                    const GraphMaterials& m = m_Cache.Materials();
                    const auto Solid = [&m](uint8_t id) { return id != 0 && !m.ignored[id]; };
                    switch (m_World.StateOf(chunk)) {
                        case CellState::Uniform: {
                            uint8_t id = 0;
                            m_World.GetChunk(chunk, &id);
                            solid = Solid(id);
                            break;
                        }
                        case CellState::Dense: {
                            const Chunk* c = m_World.GetChunk(chunk);
                            solid = c != nullptr && c->occ.cellMask == ~uint64_t{0};
                            for (size_t b = 0; solid && b < std::size(c->occ.voxelMask); ++b) solid = c->occ.voxelMask[b] == ~uint64_t{0};
                            for (int i = 0; solid && i < kChunkVolume; ++i) solid = !m.ignored[c->ids[i]];
                            break;
                        }
                        case CellState::Packed: {
                            // EncodeChunk: a kind byte, then (Dense) runs of id, length (16 bits).
                            const std::span<const uint8_t> rle = m_World.PackedData(chunk);
                            if (rle.size() == 2 && rle[0] == static_cast<uint8_t>(ChunkKind::Uniform)) {
                                solid = Solid(rle[1]);
                            } else if (!rle.empty() && rle[0] == static_cast<uint8_t>(ChunkKind::Dense)) {
                                solid = true;
                                for (size_t p = 1; solid && p + 2 < rle.size(); p += 3) solid = Solid(rle[p]);
                            }
                            break;
                        }
                        default: break;
                    }
                }
                e.solid = solid ? 1 : 0;
                return solid;
            }
            const World& GetWorld() const { return m_World; }
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

            /// As ForEachLink, with the face crossed and the bond (voxel faces shared): fn(neighbour,
            /// face, bond) returns false to stop. Across a chunk face that is not loaded
            /// `unloaded(chunk)` is called: false stops, true goes on with the next face. Before a
            /// chunk face is crossed `enter(chunk)` is asked (false: stop, the chunk is not resolved).
            template <typename Fn, typename Unloaded, typename Enter>
            void ForEachBond(const NodeRef r, Fn&& fn, Unloaded&& unloaded, Enter&& enter) {   // r by value: fn may grow the vector it came from
                const Entry& e = m_Entries[r.entry];
                const ChunkGraph& g = *e.graph;
                const GraphNode& node = g.nodes[r.node];
                const GraphLink* link = g.links.data() + node.firstLink;
                const GraphLink* end = link + node.linkCount;
                for (uint8_t f = 0; f < 6; ++f) {
                    if (node.face[f] == 0) continue;
                    if (NeighbourCell(node.cell, f) >= 0) {
                        for (; link != end && link->face == f; ++link) {
                            if (!fn(NodeRef{r.entry, link->node}, f, static_cast<uint32_t>(link->bond))) return;
                        }
                        continue;
                    }
                    glm::ivec3 next = e.chunk;
                    next[f >> 1] += (f & 1u) ? 1 : -1;
                    if (!Contains(next)) continue;
                    const uint16_t ni = IndexOf(next);
                    if (!enter(ni, next)) return;
                    Entry& n = Resolve(ni, next);
                    if (n.kind == Kind::Air) continue;
                    if (n.kind == Kind::Unloaded) {
                        if (!unloaded(next)) return;
                        continue;
                    }
                    const uint32_t cell = AcrossChunkFace(node.cell, f);
                    const ChunkGraph& ng = *n.graph;
                    for (uint32_t b = ng.cellFirst[cell]; b < ng.cellFirst[cell + 1]; ++b) {
                        const uint32_t bond = BondVoxels(node, ng.nodes[b], f);
                        if (bond == 0) continue;
                        if (!fn(NodeRef{ni, static_cast<uint16_t>(b)}, f, bond)) return;
                    }
                }
            }

            std::vector<uint8_t> state;
            std::vector<uint32_t> seed;
            /// The load check's label per node: a judged node's first network node, a load
            /// (kNeckLoad | index), kNeckAnchor, kNeckVisiting or kNeckNone.
            std::vector<uint32_t> neck;

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
        /// The voxels of a node re-labelled now are the graph's node: count, face masks, bounds and
        /// what the load check reads - mass, weakest materials, anchor bit, narrowest plane (review:
        /// a material-only change that missed World::ChunkRevision changed releases unseen).
        /// Otherwise the graph is stale. One table lookup per voxel.
        bool SameAsGraph(const Slices& mask, const GraphNode& node, const Chunk* data, uint8_t uniformId, const GraphMaterials& materials) {
            GraphNode now;
            Faces(mask, now);
            uint32_t voxels = 0;
            for (const uint64_t slice : mask) voxels += static_cast<uint32_t>(std::popcount(slice));
            if (voxels != node.voxels || !std::equal(std::begin(now.face), std::end(now.face), std::begin(node.face)) ||
                !std::equal(std::begin(now.boxMin), std::end(now.boxMin), std::begin(node.boxMin)) ||
                !std::equal(std::begin(now.boxMax), std::end(now.boxMax), std::begin(node.boxMax)))
                return false;
            const glm::ivec3 o = CellOrigin(node.cell);
            uint32_t mass = 0;
            IdSet ids{};
            for (int z = 0; z < 8; ++z)
                for (uint64_t bits = mask[static_cast<size_t>(z)]; bits != 0; bits &= bits - 1) {
                    const int b = std::countr_zero(bits);
                    const uint8_t id = data ? data->Get(o.x + (b & 7), o.y + (b >> 3), o.z + z) : uniformId;
                    mass += materials.densityQ[id];
                    ids[id >> 6] |= uint64_t{1} << (id & 63u);
                }
            now.cell = node.cell;
            SetMaterials(now, ids, materials);
            PlaneCut(mask, now);
            return mass == node.mass && std::equal(std::begin(now.weak), std::end(now.weak), std::begin(node.weak)) &&
                   now.cutFaces == node.cutFaces && now.cutPlane == node.cutPlane;
        }

        /// The voxels of one half of a split node (1: layers <= plane, 2: above; 0: all).
        Slices HalfOf(const Slices& n, uint8_t half, uint8_t plane) {
            if (half == 0) return n;
            const uint64_t keep = half == 1 ? LayersUpTo(plane) : ~LayersUpTo(plane);
            Slices out;
            for (size_t z = 0; z < 8; ++z) out[z] = n[z] & keep;
            return out;
        }

        /// Bits of a face mask of `face` (x: bit y + 8z, z: bit x + 8y) in layers y <= k.
        uint64_t FaceLayersUpTo(uint8_t face, uint32_t k) {
            if (k >= 7) return kFull;
            if ((face >> 1) == 0) return kColumnX0 * ((uint64_t{2} << k) - 1u);
            return LayersUpTo(k);
        }

        /// One network node of the load check: a judged node, one half of a split one, or a load.
        struct NeckUnit {
            NodeRef ref{};
            uint8_t half = 0;              ///< 0 whole, 1 lower, 2 upper
            bool sink = false;             ///< an infinite arc to the sink
            uint32_t load = kNeckNone;     ///< a load: its index
            uint32_t mask = kNeckNone;     ///< a split node: its voxels (NeckState::masks)
            uint64_t mass = 0;
            uint64_t voxels = 0;
        };
        struct NeckLoadNodes {
            std::vector<NodeRef> members;
            uint64_t mass = 0, voxels = 0;
        };
        /// An arc pair in real capacities (MU); b == kFlowSink: an arc to the sink.
        struct NeckPair {
            uint32_t a = 0, b = 0;
            uint64_t ab = 0, ba = 0;
            uint32_t faces = 0;
            bool inner = false;            ///< across the plane of a split node
        };

        /// shred.md Part 4b, phase 3b of the search: the judged nodes (seeds that are not islands),
        /// exterior floods (loads and anchors), the network, the maximum preflow, the released
        /// parts as pieces (at most NeckLimits::maxReleasedParts / maxReleasedVoxels). Writes
        /// `result.neck*`; a chunk that is not loaded is support (counted; offline `neckWaits`).
        void NeckCheck(GraphTable& table, const std::vector<NodeRef>& seeds, const NeckLimits& lim, const GraphSearchLimits& limits,
                       const GraphMaterials& materials, VoxelSource& source, const glm::ivec3& innerMin, const glm::ivec3& innerMax,
                       GraphSearchResult& result) {
            result.neckRan = true;
            const auto Face = [&](const NodeRef& r) {
                return !Inside(table.NodeMin(r), innerMin, innerMax) || !Inside(table.NodeMax(r), innerMin, innerMax);
            };
            const auto Skip = [&result] {
                ++result.neckSkipped;
                Log::Warn("SearchIslandsOnGraph: the load check's network is over its node or arc limit: nothing judged");
            };
            const auto ChunkOf = [&table](const NodeRef& r) { return table.At(r.entry).chunk; };

            // 1. The judged nodes, in seed order; a split node is two network nodes (lower, upper).
            std::vector<NeckUnit> units;
            std::vector<NodeRef> judged;
            std::vector<Slices> masks;
            for (const NodeRef& r : seeds) {
                const uint32_t slot = table.Slot(r);
                if (table.state[slot] == kIsland) continue;
                const GraphNode& node = table.Node(r);
                table.neck[slot] = static_cast<uint32_t>(units.size());
                judged.push_back(r);
                const bool sink = Face(r) || (node.cutPlane & kCutAnchorBit) != 0;
                if (node.cutFaces == kNoCut) {
                    units.push_back({r, 0, sink, kNeckNone, kNeckNone, node.mass, node.voxels});
                } else {
                    // The halves' exact masses: the node re-labelled per voxel (and re-checked).
                    uint8_t uniformId = 0;
                    const Chunk* data = source.Get(ChunkOf(r), uniformId);
                    const Slices mask = NodeVoxels(data, uniformId, node, materials);
                    ++result.neckSplitRelabels;
                    if (!SameAsGraph(mask, node, data, uniformId, materials)) {
                        ++result.relabelMismatch;
                        return;
                    }
                    const Slices lower = HalfOf(mask, 1, node.cutPlane & 7u);
                    const glm::ivec3 o = CellOrigin(node.cell);
                    uint64_t lowerMass = 0, lowerVoxels = 0;
                    for (int z = 0; z < 8; ++z)
                        for (uint64_t bits = lower[static_cast<size_t>(z)]; bits != 0; bits &= bits - 1) {
                            const int b = std::countr_zero(bits);
                            lowerMass += materials.densityQ[data ? data->Get(o.x + (b & 7), o.y + (b >> 3), o.z + z) : uniformId];
                            ++lowerVoxels;
                        }
                    const uint32_t m = static_cast<uint32_t>(masks.size());
                    masks.push_back(mask);
                    units.push_back({r, 1, sink, kNeckNone, m, lowerMass, lowerVoxels});
                    units.push_back({r, 2, sink, kNeckNone, m, node.mass - lowerMass, node.voxels - lowerVoxels});
                    ++result.neckSplit;
                }
                if (units.size() > lim.maxNetworkNodes) {
                    Skip();
                    return;
                }
            }
            const uint32_t judgedUnits = static_cast<uint32_t>(units.size());

            // 2. Exterior floods from the judged nodes' neighbours: anchors and loads.
            std::vector<NeckLoadNodes> loads;
            std::vector<NodeRef> queue;
            bool budgetSpent = false;
            const World& world = table.GetWorld();
            const glm::ivec3 windowMin = world.WindowMinChunk(), windowMax = windowMin + world.WindowSize();
            bool waitsForLoading = false;
            const auto Unloaded = [&](const glm::ivec3& chunk) {
                // A chunk that is not loaded is support: that can only make the check release less
                // than the full answer, never something that holds. A server's window holds every
                // flood box (an error there). Offline the check asks to run again once the chunk
                // has loaded (review: holding the search back for it held the face islands too);
                // a chunk of the window still loading is waited for first, one outside the window
                // loads only once the window moves over it.
                if (!limits.deferOnUnloaded) {
                    ++result.neckUnloaded;
                    return true;
                }
                const bool inWindow = glm::all(glm::greaterThanEqual(chunk, windowMin)) && glm::all(glm::lessThan(chunk, windowMax));
                if (inWindow) ++result.neckLoading;
                else ++result.neckBeyondWindow;
                if (!result.neckWaits || (inWindow && !waitsForLoading)) {
                    result.neckWaits = true;
                    result.neckWaitChunk = chunk;
                    waitsForLoading = inWindow;
                }
                return true;
            };
            const auto EnterAny = [](uint16_t, const glm::ivec3&) { return true; };
            const auto Flood = [&](const NodeRef& start) {
                if (budgetSpent) {
                    table.neck[table.Slot(start)] = kNeckAnchor;
                    ++result.neckExteriorBudget;
                    ++result.neckAnchors;
                    return;
                }
                enum class Outcome { Open, Anchor, Budget } outcome = Outcome::Open;
                uint64_t voxels = 0, mass = 0;
                queue.clear();
                // A chunk solid through (32^3 solid voxels) is ground: the flood ends there as an
                // anchor without reading further (review: cold floods through buried chunks built
                // graphs only they read). A function of the voxels: equal whatever is cached.
                const auto Ground = [&](uint16_t index, const glm::ivec3& chunk) {
                    if (!table.SolidThrough(index, chunk)) return false;
                    ++result.neckSolidChunks;
                    return true;
                };
                const auto Enqueue = [&](const NodeRef& x) {
                    table.neck[table.Slot(x)] = kNeckVisiting;
                    queue.push_back(x);
                    ++result.neckExteriorVisits;
                    const GraphNode& node = table.Node(x);
                    voxels += node.voxels;
                    mass += node.mass;
                    if (result.neckExteriorVisits > lim.maxExteriorVisits) outcome = Outcome::Budget;
                    else if (Face(x) || (node.cutPlane & kCutAnchorBit) != 0 || voxels > limits.maxIslandVoxels) outcome = Outcome::Anchor;
                    else if (Ground(x.entry, ChunkOf(x))) outcome = Outcome::Anchor;
                };
                Enqueue(start);
                for (size_t head = 0; head < queue.size() && outcome == Outcome::Open; ++head) {
                    table.ForEachBond(
                        queue[head],
                        [&](const NodeRef& x, uint8_t, uint32_t) {
                            const uint32_t slot = table.Slot(x);
                            if (table.state[slot] == kIsland) return true;
                            const uint32_t label = table.neck[slot];
                            if (label == kNeckVisiting || label < kNeckLoad) return true;
                            if (label != kNeckNone) {          // an earlier flood's anchor (a load cannot be met)
                                outcome = Outcome::Anchor;
                                return false;
                            }
                            Enqueue(x);
                            return outcome == Outcome::Open;
                        },
                        [&](const glm::ivec3& chunk) {
                            Unloaded(chunk);
                            outcome = Outcome::Anchor;       // taken as support (counted)
                            return false;
                        },
                        [&](uint16_t index, const glm::ivec3& chunk) {
                            if (!Ground(index, chunk)) return true;
                            outcome = Outcome::Anchor;
                            return false;
                        });
                }
                if (outcome == Outcome::Open) {
                    const uint32_t index = static_cast<uint32_t>(loads.size());
                    for (const NodeRef& x : queue) table.neck[table.Slot(x)] = kNeckLoad | index;
                    loads.push_back({queue, mass, voxels});
                    return;
                }
                for (const NodeRef& x : queue) table.neck[table.Slot(x)] = kNeckAnchor;
                ++result.neckAnchors;
                if (outcome == Outcome::Budget) {
                    budgetSpent = true;
                    ++result.neckExteriorBudget;
                }
            };
            for (const NodeRef& r : judged) {
                const uint32_t first = table.neck[table.Slot(r)];
                table.ForEachBond(
                    r,
                    [&](const NodeRef& x, uint8_t, uint32_t) {
                        const uint32_t slot = table.Slot(x);
                        if (table.state[slot] == kIsland || table.neck[slot] != kNeckNone) return true;
                        Flood(x);
                        return true;
                    },
                    [&](const glm::ivec3& chunk) {
                        Unloaded(chunk);
                        // A judged node next to a chunk that is not loaded is held by it (counted).
                        units[first].sink = true;
                        if (units[first].half == 1) units[first + 1].sink = true;
                        return true;
                    },
                    EnterAny);
            }
            result.neckLoads = static_cast<uint32_t>(loads.size());
            for (uint32_t i = 0; i < loads.size(); ++i) {
                NeckUnit u;
                u.load = i;
                u.mass = loads[i].mass;
                u.voxels = loads[i].voxels;
                units.push_back(u);
            }
            result.neckNodes = static_cast<uint32_t>(units.size());
            if (units.size() > lim.maxNetworkNodes) {
                Skip();
                return;
            }

            // 3. Arc pairs, judged nodes in order, their links in ForEachBond order, each pair once.
            std::vector<NeckPair> pairs;
            const auto Cap = [&lim](uint8_t face, const GraphNode& u, const GraphNode& v, uint32_t bond) {
                const uint8_t k = StrengthClassOf(face);
                return static_cast<uint64_t>(bond) * std::min(lim.capFace[k][u.weak[k]], lim.capFace[k][v.weak[k]]);
            };
            struct Accumulator {
                uint32_t unit, target, pair;
            };
            std::vector<Accumulator> accumulators;
            for (const NodeRef& r : judged) {
                const uint32_t first = table.neck[table.Slot(r)];
                const GraphNode node = table.Node(r);
                const bool split = node.cutFaces != kNoCut;
                const uint32_t plane = node.cutPlane & 7u;
                for (uint32_t u = first; u < first + (split ? 2u : 1u); ++u)
                    if (units[u].sink) pairs.push_back({u, kFlowSink, NeckNetwork::kInfinite, 0, 0, false});
                if (split) {
                    // Lower -> upper: the lower half hangs from the upper (tension); upper -> lower: it rests on it.
                    const uint64_t t = static_cast<uint64_t>(node.cutFaces) * lim.capFace[kTension][node.weak[kTension]];
                    const uint64_t c = static_cast<uint64_t>(node.cutFaces) * lim.capFace[kCompression][node.weak[kCompression]];
                    pairs.push_back({first, first + 1, t, c, node.cutFaces, true});
                }
                accumulators.clear();
                // The groups of voxel faces across `face` by the half of each side they belong to.
                const auto Groups = [&](uint8_t face, uint64_t overlap, const GraphNode& other, bool otherSplit, auto&& add) {
                    if (face == kFaceNegY) {
                        add(0u, otherSplit ? 1u : 0u, static_cast<uint32_t>(std::popcount(overlap)));
                        return;
                    }
                    if (face == kFacePosY) {
                        add(split ? 1u : 0u, 0u, static_cast<uint32_t>(std::popcount(overlap)));
                        return;
                    }
                    const uint64_t mine = split ? FaceLayersUpTo(face, plane) : kFull;
                    const uint64_t theirs = otherSplit ? FaceLayersUpTo(face, other.cutPlane & 7u) : kFull;
                    for (uint32_t h = 0; h < 2; ++h)
                        for (uint32_t k = 0; k < 2; ++k) {
                            const uint64_t bits = overlap & (h ? ~mine : mine) & (k ? ~theirs : theirs);
                            if (bits != 0) add(h, k, static_cast<uint32_t>(std::popcount(bits)));
                        }
                };
                table.ForEachBond(
                    r,
                    [&](const NodeRef& x, uint8_t face, uint32_t) {
                        const uint32_t slot = table.Slot(x);
                        if (table.state[slot] == kIsland) return true;
                        const uint32_t label = table.neck[slot];
                        const GraphNode& other = table.Node(x);
                        const uint64_t overlap = node.face[face] & other.face[face ^ 1u];
                        if (label < kNeckLoad) {
                            if (label <= first) return true;          // made from the other side
                            const bool otherSplit = other.cutFaces != kNoCut;
                            Groups(face, overlap, other, otherSplit, [&](uint32_t h, uint32_t k, uint32_t bond) {
                                pairs.push_back({first + h, label + k, Cap(face, node, other, bond), Cap(static_cast<uint8_t>(face ^ 1u), other, node, bond),
                                                 bond, false});
                            });
                            return true;
                        }
                        // An exterior node: a load (both ways) or support (one arc to the sink),
                        // summed per (half, load or sink) in first-encounter order.
                        const bool load = label >= kNeckLoad && label < kNeckVisiting;
                        const uint32_t target = load ? judgedUnits + (label & ~kNeckLoad) : kFlowSink;
                        Groups(face, overlap, other, false, [&](uint32_t h, uint32_t, uint32_t bond) {
                            const uint32_t unit = first + h;
                            const uint64_t ab = Cap(face, node, other, bond), ba = load ? Cap(static_cast<uint8_t>(face ^ 1u), other, node, bond) : 0;
                            for (const Accumulator& acc : accumulators)
                                if (acc.unit == unit && acc.target == target) {
                                    pairs[acc.pair].ab += ab;
                                    pairs[acc.pair].ba += ba;
                                    pairs[acc.pair].faces += bond;
                                    return;
                                }
                            accumulators.push_back({unit, target, static_cast<uint32_t>(pairs.size())});
                            pairs.push_back({unit, target, ab, ba, bond, false});
                        });
                        return true;
                    },
                    [](const glm::ivec3&) { return true; }, EnterAny);
                if (pairs.size() > lim.maxArcPairs) {
                    result.neckArcs = static_cast<uint32_t>(pairs.size());
                    Skip();
                    return;
                }
            }
            result.neckArcs = static_cast<uint32_t>(pairs.size());

            // 4. The maximum preflow on perturbed capacities.
            FlowProblem problem;
            problem.nodes = static_cast<uint32_t>(units.size());
            problem.supply.resize(units.size());
            for (size_t u = 0; u < units.size(); ++u) problem.supply[u] = units[u].mass * kPerturb;
            problem.pairs.reserve(pairs.size());
            for (const NeckPair& e : pairs) problem.pairs.push_back({e.a, e.b, Perturbed(e.ab), e.b == kFlowSink ? 0 : Perturbed(e.ba)});
            problem.shuffleSeed = lim.shuffleForTesting;
            const FlowOutcome flow = MaxPreflow(problem, lim.maxFlowWork);
            result.neckFlowWork = static_cast<uint32_t>(std::min<uint64_t>(flow.work, std::numeric_limits<uint32_t>::max()));
            if (!flow.solved) {
                result.neckFlowWork = lim.maxFlowWork;
                ++result.neckFlowBudget;
                Log::Warn("SearchIslandsOnGraph: the load check's flow work budget ({} arc scans) ran out: nothing released", lim.maxFlowWork);
                return;
            }
            if (flow.allReached) return;

            // 5. Released parts: connected over network pairs, ordered by their smallest index.
            std::vector<std::vector<uint32_t>> adjacent(units.size());
            for (const NeckPair& e : pairs)
                if (e.b != kFlowSink && !flow.reach[e.a] && !flow.reach[e.b]) {
                    adjacent[e.a].push_back(e.b);
                    adjacent[e.b].push_back(e.a);
                }
            std::vector<uint32_t> piece(units.size(), kNeckNone);
            std::vector<std::vector<uint32_t>> parts;
            for (uint32_t u = 0; u < units.size(); ++u) {
                if (flow.reach[u] || piece[u] != kNeckNone) continue;
                const uint32_t id = static_cast<uint32_t>(parts.size());
                parts.emplace_back(1, u);
                piece[u] = id;
                for (size_t h = 0; h < parts[id].size(); ++h)
                    for (const uint32_t w : adjacent[parts[id][h]])
                        if (piece[w] == kNeckNone) {
                            piece[w] = id;
                            parts[id].push_back(w);
                        }
                std::sort(parts[id].begin(), parts[id].end());
            }
            std::vector<NeckCut> cuts(parts.size());
            for (size_t i = 0; i < parts.size(); ++i)
                for (const uint32_t u : parts[i]) {
                    cuts[i].load += units[u].mass;
                    cuts[i].voxels += static_cast<uint32_t>(units[u].voxels);
                }
            for (const NeckPair& e : pairs) {
                const uint32_t pa = piece[e.a], pb = e.b == kFlowSink ? kNeckNone : piece[e.b];
                if (pa == pb) continue;
                if (pa != kNeckNone) {
                    cuts[pa].cut += e.ab;
                    cuts[pa].faces += e.faces;
                    cuts[pa].insideCell = cuts[pa].insideCell || e.inner;
                }
                if (pb != kNeckNone) {
                    cuts[pb].cut += e.ba;
                    cuts[pb].faces += e.faces;
                    cuts[pb].insideCell = cuts[pb].insideCell || e.inner;
                }
            }

            // 6. Voxels: released nodes re-labelled (and re-checked), an upper half keeps its layers above the plane.
            struct Member {
                NodeRef ref;
                Slices mask;
            };
            uint64_t releasedVoxels = 0;
            uint32_t releasedParts = 0;
            bool full = false;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (cuts[i].voxels > limits.maxIslandVoxels) {
                    ++result.neckOversize;
                    continue;
                }
                // What one check may release (R5): parts in piece order while both caps hold; the
                // first that does not fit and every later one stay. Parts of A have no network arc
                // between them, so each one left is still overloaded on its own: a later cast near
                // it releases it (late, never wrong; the kept side holds with them in place, the
                // preflow routed their cut capacity through it).
                full = full || releasedParts + 1 > lim.maxReleasedParts || releasedVoxels + cuts[i].voxels > lim.maxReleasedVoxels;
                if (full) {
                    ++result.neckDeferredParts;
                    continue;
                }
                ++releasedParts;
                releasedVoxels += cuts[i].voxels;
                std::vector<Member> members;
                const auto AddNode = [&](const NodeRef& r, uint8_t half, uint32_t maskIndex) {
                    const GraphNode& node = table.Node(r);
                    Slices mask;
                    if (maskIndex != kNeckNone) {
                        mask = masks[maskIndex];
                    } else {
                        uint8_t uniformId = 0;
                        const Chunk* data = source.Get(ChunkOf(r), uniformId);
                        mask = NodeVoxels(data, uniformId, node, materials);
                        ++result.relabelledNodes;
                        if (!SameAsGraph(mask, node, data, uniformId, materials)) {
                            ++result.relabelMismatch;
                            return false;
                        }
                    }
                    members.push_back({r, HalfOf(mask, half, node.cutPlane & 7u)});
                    return true;
                };
                for (const uint32_t u : parts[i]) {
                    const NeckUnit& unit = units[u];
                    if (unit.load == kNeckNone) {
                        if (!AddNode(unit.ref, unit.half, unit.mask)) return;
                        continue;
                    }
                    for (const NodeRef& r : loads[unit.load].members)
                        if (!AddNode(r, 0, kNeckNone)) return;
                }
                glm::ivec3 mn(std::numeric_limits<int>::max()), mx(std::numeric_limits<int>::min());
                for (const Member& m : members) {
                    const glm::ivec3 o = ChunkToVoxel(ChunkOf(m.ref)) + CellOrigin(table.Node(m.ref).cell);
                    for (int z = 0; z < 8; ++z) {
                        const uint64_t slice = m.mask[static_cast<size_t>(z)];
                        if (slice == 0) continue;
                        uint64_t xs = slice | (slice >> 32);
                        xs |= xs >> 16;
                        xs |= xs >> 8;
                        xs &= 0xFFu;
                        const int y0 = std::countr_zero(slice) >> 3, y1 = (63 - std::countl_zero(slice)) >> 3;
                        mn = glm::min(mn, o + glm::ivec3(std::countr_zero(xs), y0, z));
                        mx = glm::max(mx, o + glm::ivec3(63 - std::countl_zero(xs), y1, z));
                    }
                }
                FloatingIsland island;
                island.origin = mn;
                island.volume = VoxelVolume(mx - mn + glm::ivec3(1));
                const glm::ivec3 size = island.volume.Size();
                std::vector<uint8_t>& out = island.volume.Data();
                uint32_t count = 0;
                for (const Member& m : members) {
                    uint8_t uniformId = 0;
                    const Chunk* data = source.Get(ChunkOf(m.ref), uniformId);
                    const glm::ivec3 cell = CellOrigin(table.Node(m.ref).cell);
                    const glm::ivec3 o = ChunkToVoxel(ChunkOf(m.ref)) + cell;
                    for (int z = 0; z < 8; ++z)
                        for (uint64_t bits = m.mask[static_cast<size_t>(z)]; bits != 0; bits &= bits - 1) {
                            const int b = std::countr_zero(bits);
                            const glm::ivec3 l(b & 7, b >> 3, z);
                            const glm::ivec3 d = o + l - mn;
                            const glm::ivec3 cl = cell + l;
                            out[static_cast<size_t>(d.x) + static_cast<size_t>(size.x) * (static_cast<size_t>(d.y) + static_cast<size_t>(size.y) * static_cast<size_t>(d.z))] =
                                data ? data->Get(cl.x, cl.y, cl.z) : uniformId;
                            ++count;
                        }
                }
                island.voxelCount = count;
                result.neckVoxels += count;
                result.neckPieces.push_back(std::move(island));
                result.neckCuts.push_back(cuts[i]);
            }
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

        result.floodMs = Ms(phase);
        phase = Clock::now();
        VoxelSource source(world);
        // 3b. The load check (shred.md Part 4b): the nodes of the island box that are not islands
        //     are judged; the largest strictly overloaded set becomes neck pieces.
        if (limits.neck != nullptr && result.complete) {
            const double buildBefore = cache.Stats().buildMs;
            NeckCheck(table, seeds, *limits.neck, limits, materials, source, innerMin, innerMax, result);
            // The graphs the check's floods built are logged apart (review: they were counted twice).
            result.neckBuildMs = cache.Stats().buildMs - buildBefore;
            result.neckMs = std::max(0.0, Ms(phase) - result.neckBuildMs);
            phase = Clock::now();
        }

        // 4. Island voxels: only released nodes are re-labelled per voxel, then each island is
        //    placed where the voxel search finds it (in-box islands by first voxel; face islands by
        //    their smallest in-box component, ties by its first voxel).
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

    NeckFlowResult SolveNeckNetwork(const NeckNetwork& network, uint32_t maxWork) {
        FlowProblem p;
        p.nodes = static_cast<uint32_t>(network.supply.size());
        p.supply.resize(p.nodes);
        for (uint32_t v = 0; v < p.nodes; ++v) p.supply[v] = network.supply[v] * kPerturb;
        for (const NeckNetwork::Arc& a : network.arcs) {
            const bool sink = a.to == NeckNetwork::kNeckSink;
            p.pairs.push_back({a.from, sink ? kFlowSink : a.to, Perturbed(a.cap), sink ? 0 : Perturbed(a.capBack)});
        }
        p.shuffleSeed = network.shuffleSeed;
        const FlowOutcome o = MaxPreflow(p, maxWork);
        NeckFlowResult out;
        out.solved = o.solved;
        out.work = o.work;
        out.released.assign(p.nodes, 0);
        if (o.solved && !o.allReached)
            for (uint32_t v = 0; v < p.nodes; ++v) out.released[v] = o.reach[v] ? 0 : 1;
        return out;
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
            result.neckPieces.clear();
            result.neckCuts.clear();
        }
        result.relabelMismatch += stale;
        return result;
    }
}
