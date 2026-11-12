module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

module vortex.voxel;

import vortex.core;

namespace vortex::voxel {

    namespace {
        constexpr int kMaxIslandRegionAxis = 192;      // voxels; larger regions are refused

        /// Copies the voxels of one component into a volume cropped to the component's bounds.
        VoxelVolume ExtractComponent(const VoxelVolume& source, const ComponentLabels& labels, uint32_t component) {
            const glm::ivec3 mn = labels.boundsMin[component];
            const glm::ivec3 mx = labels.boundsMax[component];
            VoxelVolume out(mx - mn);
            const std::vector<uint8_t>& src = source.Data();
            std::vector<uint8_t>& dst = out.Data();
            for (int z = mn.z; z < mx.z; ++z)
                for (int y = mn.y; y < mx.y; ++y) {
                    size_t from = source.Index(mn.x, y, z);
                    size_t to = out.Index(0, y - mn.y, z - mn.z);
                    for (int x = mn.x; x < mx.x; ++x, ++from, ++to) {
                        if (labels.labels[from] == component) dst[to] = src[from];
                    }
                }
            return out;
        }
    }

    // --- labelling ----------------------------------------------------------------------------

    ComponentLabels LabelComponents(const VoxelVolume& volume) {
        ComponentLabels out;
        out.size = volume.Size();
        out.voxelCounts.assign(1, 0u);
        out.boundsMin.assign(1, glm::ivec3(0));
        out.boundsMax.assign(1, glm::ivec3(0));
        out.firstIndex.assign(1, 0u);

        const glm::ivec3 size = out.size;
        const std::vector<uint8_t>& data = volume.Data();
        if (size.x <= 0 || size.y <= 0 || size.z <= 0) return out;
        const size_t sx = static_cast<size_t>(size.x);
        const size_t sy = static_cast<size_t>(size.y);
        const size_t sz = static_cast<size_t>(size.z);
        const size_t sxy = sx * sy;
        const size_t total = sxy * sz;
        if (data.size() < total) return out;

        out.labels.assign(total, 0u);
        std::vector<size_t> stack;

        for (size_t seed = 0; seed < total; ++seed) {
            if (data[seed] == 0 || out.labels[seed] != 0) continue;

            const uint32_t label = ++out.count;
            uint32_t voxels = 0;
            glm::ivec3 mn(size), mx(0);

            out.labels[seed] = label;
            stack.push_back(seed);
            while (!stack.empty()) {
                const size_t index = stack.back();
                stack.pop_back();

                const size_t x = index % sx;
                const size_t y = (index / sx) % sy;
                const size_t z = index / sxy;
                const glm::ivec3 p(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z));
                mn = glm::min(mn, p);
                mx = glm::max(mx, p + glm::ivec3(1));
                ++voxels;

                // Voxels are labelled when pushed, so each one enters the stack exactly once.
                const auto visit = [&](size_t neighbour) {
                    if (data[neighbour] != 0 && out.labels[neighbour] == 0) {
                        out.labels[neighbour] = label;
                        stack.push_back(neighbour);
                    }
                };
                if (x > 0) visit(index - 1);
                if (x + 1 < sx) visit(index + 1);
                if (y > 0) visit(index - sx);
                if (y + 1 < sy) visit(index + sx);
                if (z > 0) visit(index - sxy);
                if (z + 1 < sz) visit(index + sxy);
            }

            out.voxelCounts.push_back(voxels);
            out.firstIndex.push_back(static_cast<uint32_t>(seed));
            out.boundsMin.push_back(mn);
            out.boundsMax.push_back(mx);
        }
        return out;
    }

    // --- splitting ----------------------------------------------------------------------------

    SplitResult SplitComponents(const VoxelVolume& volume, uint32_t minVoxels) {
        SplitResult result;
        const ComponentLabels labels = LabelComponents(volume);
        const std::vector<uint8_t>& data = volume.Data();

        for (uint32_t c = 1; c <= labels.count; ++c) {
            const glm::ivec3 mn = labels.boundsMin[c];
            const glm::ivec3 mx = labels.boundsMax[c];

            if (labels.voxelCounts[c] < minVoxels) {
                for (int z = mn.z; z < mx.z; ++z)
                    for (int y = mn.y; y < mx.y; ++y) {
                        size_t index = volume.Index(mn.x, y, z);
                        for (int x = mn.x; x < mx.x; ++x, ++index) {
                            if (labels.labels[index] != c) continue;
                            LooseVoxel loose;
                            loose.position = glm::ivec3(x, y, z);
                            loose.material = data[index];
                            result.loose.push_back(loose);
                        }
                    }
                continue;
            }

            VolumePiece piece;
            piece.volume = ExtractComponent(volume, labels, c);
            piece.offset = mn;
            piece.voxelCount = labels.voxelCounts[c];
            result.pieces.push_back(std::move(piece));
        }

        // Stable: pieces of equal size keep their scan order, so the result is deterministic.
        std::stable_sort(result.pieces.begin(), result.pieces.end(),
                         [](const VolumePiece& a, const VolumePiece& b) { return a.voxelCount > b.voxelCount; });
        return result;
    }

    // --- floating islands ---------------------------------------------------------------------

    std::vector<FloatingIsland> ExtractFloatingIslands(World& world, const MaterialLibrary* materials,
                                                       const glm::ivec3& requestMin, const glm::ivec3& regionMax,
                                                       uint32_t maxIslandVoxels, bool removeFromWorld,
                                                       uint32_t ignoreFlags, bool* searched, IslandBoxScan* scan) {
        std::vector<FloatingIsland> islands;
        if (searched) *searched = false;
        if (scan) *scan = IslandBoxScan{};

        // 64 bit: the difference of two arbitrary voxel coordinates may not fit an int.
        int64_t extent[3];
        for (int a = 0; a < 3; ++a) {
            extent[a] = static_cast<int64_t>(regionMax[a]) - static_cast<int64_t>(requestMin[a]);
            if (extent[a] <= 0) return islands;
        }
        if (extent[0] > kMaxIslandRegionAxis || extent[1] > kMaxIslandRegionAxis || extent[2] > kMaxIslandRegionAxis) {
            Log::Warn("ExtractFloatingIslands: region of {} x {} x {} voxels is too large (limit {} per axis), nothing extracted",
                      extent[0], extent[1], extent[2], kMaxIslandRegionAxis);
            return islands;
        }
        // Below the bottom row of a fixed-height window nothing is ever loaded: it is the world's
        // ground. The box ends there, so a component touching the window's bottom row touches the
        // box face and is attached, as the face flood treats those rows (an integer of the fixed
        // height range, equal on every machine). A box entirely below holds nothing.
        glm::ivec3 regionMin = requestMin;
        if (world.FullHeight()) {
            const int bottom = world.WindowMinChunk().y * kChunkSize;
            if (regionMax.y <= bottom) {
                if (searched) *searched = true;
                return islands;
            }
            regionMin.y = std::max(regionMin.y, bottom);
        }
        const glm::ivec3 size(static_cast<int>(extent[0]), regionMax.y - regionMin.y, static_cast<int>(extent[2]));

        // Without the complete picture a component cannot be proven to be floating. Above the
        // build ceiling of a fixed-height window nothing exists: those chunks are air.
        const glm::ivec3 chunkMin = VoxelToChunk(regionMin);
        const glm::ivec3 chunkMax = VoxelToChunk(regionMax - glm::ivec3(1));
        const int ceilingRow = world.FullHeight() ? world.WindowMinChunk().y + world.WindowSize().y : std::numeric_limits<int>::max();
        for (int cz = chunkMin.z; cz <= chunkMax.z; ++cz)
            for (int cy = chunkMin.y; cy <= std::min(chunkMax.y, ceilingRow - 1); ++cy)
                for (int cx = chunkMin.x; cx <= chunkMax.x; ++cx) {
                    if (!world.IsChunkLoaded(glm::ivec3(cx, cy, cz))) return islands;
                }
        if (searched) *searched = true;
        if (scan) {
            scan->boxVoxels = static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.y) * static_cast<uint64_t>(size.z);
            scan->boxMin = regionMin;
            scan->boxMax = regionMax;
        }

        std::array<bool, 256> ignored{};
        if (materials != nullptr && ignoreFlags != 0) {
            for (uint32_t id = 1; id < 256; ++id) {
                ignored[id] = (materials->Get(kWorldPalette, static_cast<uint8_t>(id)).flags & ignoreFlags) != 0;
            }
        }

        // Local dense copy of the region, chunk by chunk (docs/design/shred.md stage 0): one lookup
        // per chunk, air chunks and empty 4^3 blocks cost nothing, a Uniform chunk fills rows, a
        // Packed chunk is decoded once into a copy (it stays packed).
        VoxelVolume local(size);
        std::unique_ptr<Chunk> packedCopy;
        std::vector<uint8_t>& data = local.Data();
        bool anySolid = false;
        for (int cz = chunkMin.z; cz <= chunkMax.z; ++cz)
            for (int cy = chunkMin.y; cy <= std::min(chunkMax.y, ceilingRow - 1); ++cy)
                for (int cx = chunkMin.x; cx <= chunkMax.x; ++cx) {
                    const glm::ivec3 chunk(cx, cy, cz);
                    const CellState state = world.StateOf(chunk);
                    if (state == CellState::Empty) continue;
                    const glm::ivec3 base = ChunkToVoxel(chunk);
                    const glm::ivec3 lo = glm::max(base, regionMin);
                    const glm::ivec3 hi = glm::min(base + glm::ivec3(kChunkSize), regionMax);
                    if (state == CellState::Uniform) {
                        uint8_t id = 0;
                        world.GetChunk(chunk, &id);
                        if (id == 0 || ignored[id]) continue;
                        for (int z = lo.z; z < hi.z; ++z)
                            for (int y = lo.y; y < hi.y; ++y) {
                                const size_t index = local.Index(lo.x - regionMin.x, y - regionMin.y, z - regionMin.z);
                                std::fill(data.begin() + static_cast<std::ptrdiff_t>(index),
                                          data.begin() + static_cast<std::ptrdiff_t>(index + static_cast<size_t>(hi.x - lo.x)), id);
                            }
                        anySolid = true;
                        continue;
                    }
                    const Chunk* source = world.GetChunk(chunk);
                    if (source == nullptr) {               // Packed: read from a decoded copy
                        if (!packedCopy) packedCopy = std::make_unique<Chunk>();
                        if (!world.ReadChunk(chunk, *packedCopy)) continue;    // DecodeChunk rebuilds the occupancy
                        source = packedCopy.get();
                    }
                    const Chunk& src = *source;
                    for (int z = lo.z; z < hi.z; ++z)
                        for (int y = lo.y; y < hi.y; ++y) {
                            const int ly = y - base.y, lz = z - base.z;
                            size_t index = local.Index(lo.x - regionMin.x, y - regionMin.y, z - regionMin.z);
                            for (int x = lo.x; x < hi.x;) {
                                const int lx = x - base.x;
                                const uint32_t block = Chunk::BlockIndex(lx, ly, lz);
                                const int runEnd = std::min(hi.x, base.x + ((lx | 3) + 1));     // end of this 4-voxel block row
                                if (((src.occ.blockMask[block >> 6] >> (block & 63u)) & 1u) == 0) {
                                    index += static_cast<size_t>(runEnd - x);
                                    x = runEnd;
                                    continue;
                                }
                                for (; x < runEnd; ++x, ++index) {
                                    const uint8_t id = src.ids[Chunk::VoxelIndex(x - base.x, ly, lz)];
                                    if (id == 0 || ignored[id]) continue;
                                    data[index] = id;
                                    anySolid = true;
                                }
                            }
                        }
                }
        if (!anySolid) return islands;

        ComponentLabels ownLabels;
        ComponentLabels& labels = scan ? scan->labels : ownLabels;
        labels = LabelComponents(local);
        uint32_t failedRemovals = 0;

        for (uint32_t c = 1; c <= labels.count; ++c) {
            const glm::ivec3 mn = labels.boundsMin[c];
            const glm::ivec3 mx = labels.boundsMax[c];
            // Touching the boundary of the region means the component continues outside it.
            const bool attached = mn.x == 0 || mn.y == 0 || mn.z == 0 || mx.x == size.x || mx.y == size.y || mx.z == size.z;
            const bool oversize = labels.voxelCounts[c] > maxIslandVoxels;
            if (scan && attached) {
                if (oversize) {
                    ++scan->faceOversize;
                } else {
                    const uint32_t first = labels.firstIndex[c];
                    const glm::ivec3 local3(static_cast<int>(first % static_cast<uint32_t>(size.x)),
                                            static_cast<int>((first / static_cast<uint32_t>(size.x)) % static_cast<uint32_t>(size.y)),
                                            static_cast<int>(first / (static_cast<uint32_t>(size.x) * static_cast<uint32_t>(size.y))));
                    scan->faceComponents.push_back({c, regionMin + local3, labels.voxelCounts[c]});
                }
            }
            if (scan && !attached && oversize) ++scan->oversizeIslands;
            if (attached || oversize) continue;

            FloatingIsland island;
            island.origin = regionMin + mn;
            island.volume = ExtractComponent(local, labels, c);
            island.voxelCount = labels.voxelCounts[c];

            if (removeFromWorld) {
                for (int z = mn.z; z < mx.z; ++z)
                    for (int y = mn.y; y < mx.y; ++y) {
                        size_t index = local.Index(mn.x, y, z);
                        for (int x = mn.x; x < mx.x; ++x, ++index) {
                            if (labels.labels[index] != c) continue;
                            if (world.SetVoxel(regionMin + glm::ivec3(x, y, z), 0)) continue;
                            // The voxel stays in the world, so it must not also exist in the island.
                            island.volume.Set(x - mn.x, y - mn.y, z - mn.z, 0);
                            --island.voxelCount;
                            ++failedRemovals;
                        }
                    }
                if (island.voxelCount == 0) continue;
            }
            islands.push_back(std::move(island));
        }

        if (failedRemovals != 0) {
            Log::Warn("ExtractFloatingIslands: {} voxels could not be removed from the world and were left in place", failedRemovals);
        }
        return islands;
    }

    // --- face flood (docs/design/shred.md, stage 1) ---------------------------------------------

    namespace {
        constexpr uint32_t kVisiting = 1, kIsland = 2, kAttached = 3;      // 0: not visited
        constexpr int kFloodAxis = kMaxIslandRegionAxis;          // 192: the flood box limit per axis
        constexpr size_t kFloodVoxels = static_cast<size_t>(kFloodAxis) * kFloodAxis * kFloodAxis;
        constexpr int kFloodChunkAxis = kFloodAxis / kChunkSize + 1;   // chunks a 192-voxel box can touch per axis
        constexpr size_t kMaxPackedCopies = 16;

        /// 2 bits per voxel of the flood box (1.7 MiB), allocated once per thread. Every voxel a
        /// call marks is listed in `touched` and cleared at its end: never a full memset.
        struct FloodScratch {
            std::vector<uint32_t> state = std::vector<uint32_t>(kFloodVoxels / 16, 0u);
            std::vector<uint32_t> touched;
            uint32_t Get(uint32_t i) const { return (state[i >> 4] >> ((i & 15u) * 2u)) & 3u; }
            void Set(uint32_t i, uint32_t v) {
                uint32_t& word = state[i >> 4];
                const uint32_t shift = (i & 15u) * 2u;
                word = (word & ~(3u << shift)) | (v << shift);
            }
            void Clear() {
                for (const uint32_t i : touched) state[i >> 4] = 0;
                touched.clear();
            }
        };

        /// Reads world voxels of the flood box: one World lookup per chunk, kept in a table over
        /// the box's chunks; Packed chunks decoded into at most 16 copies (FIFO; an evicted one is
        /// decoded again when needed, and counted).
        class FloodReader {
        public:
            enum class Kind : uint8_t { Unresolved, Air, Uniform, Dense, Packed, Unloaded };
            FloodReader(const World& world, const glm::ivec3& floodMin, const std::array<bool, 256>& ignored)
                : m_World(world), m_ChunkMin(VoxelToChunk(floodMin)), m_Ignored(ignored),
                  m_CeilingY(world.FullHeight() ? (world.WindowMinChunk().y + world.WindowSize().y) * kChunkSize
                                                : std::numeric_limits<int>::max()) {
                m_Entries.resize(static_cast<size_t>(kFloodChunkAxis) * kFloodChunkAxis * kFloodChunkAxis);
            }
            enum class Read : uint8_t { Air, Solid, Unloaded };
            /// What the voxel is; `id` is set for a solid one.
            Read At(const glm::ivec3& v, uint8_t& id) {
                if (v.y >= m_CeilingY) return Read::Air;              // nothing exists above the build ceiling
                const glm::ivec3 chunk = VoxelToChunk(v);
                const glm::ivec3 rel = chunk - m_ChunkMin;
                Entry& e = m_Entries[static_cast<size_t>(rel.x + kFloodChunkAxis * (rel.y + kFloodChunkAxis * rel.z))];
                if (e.kind == Kind::Unresolved) Resolve(chunk, e);
                const glm::ivec3 l = VoxelToLocal(v);
                switch (e.kind) {
                    case Kind::Air: return Read::Air;
                    case Kind::Unloaded: return Read::Unloaded;
                    case Kind::Uniform: id = e.uniform; break;
                    default: id = e.chunk->ids[Chunk::VoxelIndex(l.x, l.y, l.z)]; break;
                }
                return (id != 0 && !m_Ignored[id]) ? Read::Solid : Read::Air;
            }
            uint32_t Decoded() const { return m_Decoded; }

        private:
            struct Entry {
                Kind kind = Kind::Unresolved;
                uint8_t uniform = 0;
                const Chunk* chunk = nullptr;
            };
            void Resolve(const glm::ivec3& chunk, Entry& e) {
                switch (m_World.StateOf(chunk)) {
                    case CellState::Empty: e.kind = Kind::Air; return;
                    case CellState::Uniform:
                        m_World.GetChunk(chunk, &e.uniform);
                        e.kind = (e.uniform == 0 || m_Ignored[e.uniform]) ? Kind::Air : Kind::Uniform;
                        return;
                    case CellState::Dense:
                        e.chunk = m_World.GetChunk(chunk);
                        e.kind = e.chunk ? Kind::Dense : Kind::Unloaded;
                        return;
                    case CellState::Packed: {
                        // Up to 16 copies; then the oldest is reused and its entry must decode again.
                        size_t slot = 0;
                        if (m_Copies.size() < kMaxPackedCopies) {
                            m_Copies.push_back({std::make_unique<Chunk>(), nullptr});
                            slot = m_Copies.size() - 1;
                        } else {
                            slot = m_Next;
                            m_Next = (m_Next + 1) % kMaxPackedCopies;
                            Copy& old = m_Copies[slot];
                            if (old.owner) {
                                old.owner->kind = Kind::Unresolved;
                                old.owner->chunk = nullptr;
                            }
                        }
                        Copy& copy = m_Copies[slot];
                        if (!m_World.ReadChunk(chunk, *copy.chunk)) {
                            copy.owner = nullptr;
                            e.kind = Kind::Unloaded;
                            return;
                        }
                        ++m_Decoded;
                        copy.owner = &e;
                        e.chunk = copy.chunk.get();
                        e.kind = Kind::Packed;
                        return;
                    }
                    default: e.kind = Kind::Unloaded; return;
                }
            }
            struct Copy {
                std::unique_ptr<Chunk> chunk;
                Entry* owner = nullptr;
            };
            const World& m_World;
            glm::ivec3 m_ChunkMin;
            const std::array<bool, 256>& m_Ignored;
            int m_CeilingY;
            std::vector<Entry> m_Entries;
            std::vector<Copy> m_Copies;
            size_t m_Next = 0;
            uint32_t m_Decoded = 0;
        };
    }

    FloodResult ProveFaceComponents(const World& world, const MaterialLibrary* materials, std::span<const FaceComponent> candidates,
                                    const glm::ivec3& boxMin, const glm::ivec3& boxMax, const ComponentLabels& boxLabels,
                                    uint32_t ignoreFlags, const FloodLimits& limits) {
        FloodResult result;
        result.candidates = static_cast<uint32_t>(candidates.size());
        if (candidates.empty()) return result;

        // The flood box must fit the state array and hold the island box; anything else proves nothing.
        int64_t extent[3];
        bool valid = boxLabels.size == boxMax - boxMin;
        for (int a = 0; a < 3; ++a) {
            extent[a] = static_cast<int64_t>(limits.floodMax[a]) - static_cast<int64_t>(limits.floodMin[a]);
            valid = valid && extent[a] > 0 && extent[a] <= kFloodAxis && boxMin[a] >= limits.floodMin[a] && boxMax[a] <= limits.floodMax[a];
        }
        if (!valid) {
            Log::Warn("ProveFaceComponents: flood box {} x {} x {} does not hold the island box or is over {} voxels per axis; "
                      "{} candidate(s) stay attached", extent[0], extent[1], extent[2], kFloodAxis, candidates.size());
            result.attachedByLimit = result.candidates;
            return result;
        }
        const glm::ivec3 size(static_cast<int>(extent[0]), static_cast<int>(extent[1]), static_cast<int>(extent[2]));
        const glm::ivec3 boxSize = boxMax - boxMin;

        std::array<bool, 256> ignored{};
        if (materials != nullptr && ignoreFlags != 0) {
            for (uint32_t id = 1; id < 256; ++id) {
                ignored[id] = (materials->Get(kWorldPalette, static_cast<uint8_t>(id)).flags & ignoreFlags) != 0;
            }
        }
        // Below the bottom row of a fixed-height window nothing is ever loaded: a flood that would
        // go there gives up (it is the ground of the world).
        const int bottomY = world.FullHeight() ? world.WindowMinChunk().y * kChunkSize : std::numeric_limits<int>::min();

        // In-box labels a flood may run into: the candidates (pending) and everything else, which
        // is attached (face components over the limit; others cannot be reached from outside).
        std::vector<uint8_t> pending(static_cast<size_t>(boxLabels.count) + 1, 0);
        for (const FaceComponent& c : candidates) {
            if (c.label > 0 && c.label <= boxLabels.count) pending[c.label] = 1;
        }
        std::vector<uint32_t> order(candidates.size());
        for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return candidates[a].inBoxVoxels != candidates[b].inBoxVoxels ? candidates[a].inBoxVoxels < candidates[b].inBoxVoxels
                                                                          : candidates[a].label < candidates[b].label;
        });

        // One state array per thread, reached once (a thread_local in the loop costs a guard per use).
        thread_local FloodScratch threadScratch;
        FloodScratch& scratch = threadScratch;
        FloodReader reader(world, limits.floodMin, ignored);
        const auto Index = [&size](const glm::ivec3& l) {
            return static_cast<uint32_t>(l.x + size.x * (l.y + size.y * l.z));
        };
        std::vector<glm::ivec3> queue;
        std::vector<uint8_t> ids;
        bool budgetSpent = false;
        enum class Outcome { Open, Limit, Known, Budget, Unloaded };

        for (const uint32_t ci : order) {
            const FaceComponent& candidate = candidates[ci];
            if (budgetSpent) {
                ++result.attachedByBudget;
                continue;
            }
            const glm::ivec3 seed = candidate.firstVoxel - limits.floodMin;
            const uint32_t seedState = scratch.Get(Index(seed));
            if (seedState == kIsland) {             // part of an island an earlier flood proved
                ++result.merged;
                continue;
            }
            if (seedState == kAttached) {
                ++result.attachedByKnown;
                continue;
            }

            queue.clear();
            ids.clear();
            Outcome outcome = Outcome::Open;
            // Marks a solid voxel as part of this flood; false once a limit is passed.
            const auto Enqueue = [&](const glm::ivec3& l, uint8_t id) {
                const uint32_t i = Index(l);
                scratch.Set(i, kVisiting);
                scratch.touched.push_back(i);
                queue.push_back(l);
                ids.push_back(id);
                ++result.visits;
                if (result.visits > limits.maxVisitsPerSearch) outcome = Outcome::Budget;
                else if (queue.size() > limits.maxVoxels) outcome = Outcome::Limit;
            };
            {
                uint8_t id = 0;
                reader.At(candidate.firstVoxel, id);
                Enqueue(seed, id);
            }
            static constexpr glm::ivec3 kNeighbours[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
            for (size_t head = 0; head < queue.size() && outcome == Outcome::Open; ++head) {
                const glm::ivec3 l = queue[head];
                // On the flood box face: the piece would leave the box.
                if (l.x == 0 || l.y == 0 || l.z == 0 || l.x == size.x - 1 || l.y == size.y - 1 || l.z == size.z - 1) {
                    outcome = Outcome::Limit;
                    break;
                }
                for (const glm::ivec3& d : kNeighbours) {
                    const glm::ivec3 nl = l + d;
                    const uint32_t ni = Index(nl);
                    const uint32_t st = scratch.Get(ni);
                    if (st == kVisiting || st == kIsland) continue;
                    if (st == kAttached) {
                        outcome = Outcome::Known;
                        break;
                    }
                    const glm::ivec3 v = limits.floodMin + nl;
                    if (v.y < bottomY) {
                        outcome = Outcome::Limit;
                        break;
                    }
                    uint8_t id = 0;
                    const glm::ivec3 b = v - boxMin;
                    if (b.x >= 0 && b.y >= 0 && b.z >= 0 && b.x < boxSize.x && b.y < boxSize.y && b.z < boxSize.z) {
                        // Inside the island box the labels decide.
                        const uint32_t label = boxLabels.labels[static_cast<size_t>(b.x) +
                                                                static_cast<size_t>(boxSize.x) * (static_cast<size_t>(b.y) + static_cast<size_t>(boxSize.y) * static_cast<size_t>(b.z))];
                        if (label == 0) continue;
                        if (pending[label] == 0) {
                            outcome = Outcome::Known;
                            break;
                        }
                        reader.At(v, id);
                    } else {
                        const FloodReader::Read read = reader.At(v, id);
                        if (read == FloodReader::Read::Air) continue;
                        if (read == FloodReader::Read::Unloaded) {
                            if (limits.deferOnUnloaded) {
                                result.complete = false;
                                result.unloadedChunk = VoxelToChunk(v);
                                result.islands.clear();
                                result.chunksDecoded = reader.Decoded();
                                scratch.Clear();
                                return result;
                            }
                            outcome = Outcome::Unloaded;
                            break;
                        }
                    }
                    Enqueue(nl, id);
                    if (outcome != Outcome::Open) break;
                }
            }

            if (outcome == Outcome::Open) {
                // Closed inside the flood box: an island.
                glm::ivec3 mn = queue[0], mx = queue[0];
                for (const glm::ivec3& l : queue) {
                    mn = glm::min(mn, l);
                    mx = glm::max(mx, l);
                }
                FloatingIsland island;
                island.origin = limits.floodMin + mn;
                island.volume = VoxelVolume(mx - mn + glm::ivec3(1));
                island.voxelCount = static_cast<uint32_t>(queue.size());
                for (size_t i = 0; i < queue.size(); ++i) {
                    scratch.Set(Index(queue[i]), kIsland);
                    island.volume.Set(queue[i] - mn, ids[i]);
                }
                result.islands.push_back(std::move(island));
                ++result.proven;
                continue;
            }
            for (const glm::ivec3& l : queue) scratch.Set(Index(l), kAttached);
            pending[candidate.label] = 0;          // its whole in-box component is attached now
            switch (outcome) {
                case Outcome::Limit: ++result.attachedByLimit; break;
                case Outcome::Known: ++result.attachedByKnown; break;
                case Outcome::Budget: ++result.attachedByBudget; budgetSpent = true; break;
                case Outcome::Unloaded: ++result.attachedByUnloaded; break;
                case Outcome::Open: break;
            }
        }
        result.chunksDecoded = reader.Decoded();
        scratch.Clear();
        return result;
    }
}
