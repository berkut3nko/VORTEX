module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
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
                                                       const glm::ivec3& regionMin, const glm::ivec3& regionMax,
                                                       uint32_t maxIslandVoxels, bool removeFromWorld,
                                                       uint32_t ignoreFlags) {
        std::vector<FloatingIsland> islands;

        // 64 bit: the difference of two arbitrary voxel coordinates may not fit an int.
        int64_t extent[3];
        for (int a = 0; a < 3; ++a) {
            extent[a] = static_cast<int64_t>(regionMax[a]) - static_cast<int64_t>(regionMin[a]);
            if (extent[a] <= 0) return islands;
        }
        if (extent[0] > kMaxIslandRegionAxis || extent[1] > kMaxIslandRegionAxis || extent[2] > kMaxIslandRegionAxis) {
            Log::Warn("ExtractFloatingIslands: region of {} x {} x {} voxels is too large (limit {} per axis), nothing extracted",
                      extent[0], extent[1], extent[2], kMaxIslandRegionAxis);
            return islands;
        }
        const glm::ivec3 size(static_cast<int>(extent[0]), static_cast<int>(extent[1]), static_cast<int>(extent[2]));

        // Without the complete picture a component cannot be proven to be floating.
        const glm::ivec3 chunkMin = VoxelToChunk(regionMin);
        const glm::ivec3 chunkMax = VoxelToChunk(regionMax - glm::ivec3(1));
        for (int cz = chunkMin.z; cz <= chunkMax.z; ++cz)
            for (int cy = chunkMin.y; cy <= chunkMax.y; ++cy)
                for (int cx = chunkMin.x; cx <= chunkMax.x; ++cx) {
                    if (!world.IsChunkLoaded(glm::ivec3(cx, cy, cz))) return islands;
                }

        std::array<bool, 256> ignored{};
        if (materials != nullptr && ignoreFlags != 0) {
            for (uint32_t id = 1; id < 256; ++id) {
                ignored[id] = (materials->Get(kWorldPalette, static_cast<uint8_t>(id)).flags & ignoreFlags) != 0;
            }
        }

        // Local dense copy of the region, chunk by chunk so that air chunks cost nothing.
        VoxelVolume local(size);
        std::vector<uint8_t>& data = local.Data();
        bool anySolid = false;
        for (int cz = chunkMin.z; cz <= chunkMax.z; ++cz)
            for (int cy = chunkMin.y; cy <= chunkMax.y; ++cy)
                for (int cx = chunkMin.x; cx <= chunkMax.x; ++cx) {
                    const glm::ivec3 chunk(cx, cy, cz);
                    if (world.StateOf(chunk) == CellState::Empty) continue;
                    const glm::ivec3 base = ChunkToVoxel(chunk);
                    const glm::ivec3 lo = glm::max(base, regionMin);
                    const glm::ivec3 hi = glm::min(base + glm::ivec3(kChunkSize), regionMax);
                    for (int z = lo.z; z < hi.z; ++z)
                        for (int y = lo.y; y < hi.y; ++y) {
                            size_t index = local.Index(lo.x - regionMin.x, y - regionMin.y, z - regionMin.z);
                            for (int x = lo.x; x < hi.x; ++x, ++index) {
                                const uint8_t id = world.GetVoxel(glm::ivec3(x, y, z));
                                if (id == 0 || ignored[id]) continue;
                                data[index] = id;
                                anySolid = true;
                            }
                        }
                }
        if (!anySolid) return islands;

        const ComponentLabels labels = LabelComponents(local);
        uint32_t failedRemovals = 0;

        for (uint32_t c = 1; c <= labels.count; ++c) {
            const glm::ivec3 mn = labels.boundsMin[c];
            const glm::ivec3 mx = labels.boundsMax[c];
            // Touching the boundary of the region means the component continues outside it.
            const bool attached = mn.x == 0 || mn.y == 0 || mn.z == 0 || mx.x == size.x || mx.y == size.y || mx.z == size.z;
            if (attached || labels.voxelCounts[c] > maxIslandVoxels) continue;

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
}
