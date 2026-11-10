module;

#include <glm/glm.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

module vortex.voxel;

import vortex.core;

namespace vortex::voxel {

    // --- box decomposition --------------------------------------------------------------------

    std::vector<ColliderBox> BuildColliderBoxes(const VoxelVolume& volume, bool mergeMaterials) {
        std::vector<ColliderBox> boxes;
        const glm::ivec3 size = volume.Size();
        const std::vector<uint8_t>& data = volume.Data();
        if (size.x <= 0 || size.y <= 0 || size.z <= 0) return boxes;
        const size_t sx = static_cast<size_t>(size.x);
        const size_t sxy = sx * static_cast<size_t>(size.y);
        if (data.size() < sxy * static_cast<size_t>(size.z)) return boxes;

        std::vector<uint8_t> visited(data.size(), 0);

        // A voxel can join the box started by a voxel with id `first` if it is solid, not yet
        // part of another box and (unless materials are merged) made of the same material.
        const auto usable = [&](size_t index, uint8_t first) {
            const uint8_t id = data[index];
            return id != 0 && visited[index] == 0 && (mergeMaterials || id == first);
        };

        size_t start = 0;
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x, ++start) {
                    const uint8_t first = data[start];
                    if (first == 0 || visited[start] != 0) continue;

                    // Grow along X.
                    int w = 1;
                    while (x + w < size.x && usable(start + static_cast<size_t>(w), first)) ++w;

                    // Grow along Y: every voxel of the next row must be usable.
                    int h = 1;
                    for (; y + h < size.y; ++h) {
                        const size_t row = start + static_cast<size_t>(h) * sx;
                        bool ok = true;
                        for (int i = 0; i < w && ok; ++i) ok = usable(row + static_cast<size_t>(i), first);
                        if (!ok) break;
                    }

                    // Grow along Z: every voxel of the next layer must be usable.
                    int d = 1;
                    for (; z + d < size.z; ++d) {
                        const size_t layer = start + static_cast<size_t>(d) * sxy;
                        bool ok = true;
                        for (int j = 0; j < h && ok; ++j) {
                            const size_t row = layer + static_cast<size_t>(j) * sx;
                            for (int i = 0; i < w && ok; ++i) ok = usable(row + static_cast<size_t>(i), first);
                        }
                        if (!ok) break;
                    }

                    for (int k = 0; k < d; ++k)
                        for (int j = 0; j < h; ++j) {
                            const size_t row = start + static_cast<size_t>(k) * sxy + static_cast<size_t>(j) * sx;
                            for (int i = 0; i < w; ++i) visited[row + static_cast<size_t>(i)] = 1;
                        }

                    ColliderBox box;
                    box.min = glm::ivec3(x, y, z);
                    box.size = glm::ivec3(w, h, d);
                    box.material = first;
                    boxes.push_back(box);
                }
        return boxes;
    }

    // --- mass ---------------------------------------------------------------------------------

    MassProperties ComputeMassProperties(const VoxelVolume& volume, const MaterialLibrary& materials,
                                         PaletteId palette, float voxelSize) {
        MassProperties out;
        const glm::ivec3 size = volume.Size();
        const std::vector<uint8_t>& data = volume.Data();
        if (size.x <= 0 || size.y <= 0 || size.z <= 0) return out;
        if (data.size() < static_cast<size_t>(size.x) * static_cast<size_t>(size.y) * static_cast<size_t>(size.z)) return out;
        if (palette >= materials.PaletteCount()) {
            Log::Warn("ComputeMassProperties: palette {} does not exist, default density is used", palette);
        }

        std::array<double, 256> density{};
        for (uint32_t id = 1; id < 256; ++id) {
            const double d = static_cast<double>(materials.Get(palette, static_cast<uint8_t>(id)).density);
            density[id] = d > 0.0 ? d : 0.0;
        }

        // Accumulate in double: a large volume sums millions of terms.
        double densitySum = 0.0;
        glm::dvec3 weighted(0.0);       // sum(density * centre)
        glm::dvec3 unweighted(0.0);     // sum(centre): fallback when every material is weightless
        uint32_t solid = 0;
        size_t index = 0;
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x, ++index) {
                    const uint8_t id = data[index];
                    if (id == 0) continue;
                    const glm::dvec3 center(x + 0.5, y + 0.5, z + 0.5);
                    const double d = density[id];
                    densitySum += d;
                    weighted += center * d;
                    unweighted += center;
                    ++solid;
                }

        out.solidVoxels = solid;
        if (solid == 0) return out;
        const double cell = static_cast<double>(voxelSize);
        out.mass = static_cast<float>(densitySum * cell * cell * cell);
        out.centerOfMass = densitySum > 0.0 ? glm::vec3(weighted / densitySum)
                                            : glm::vec3(unweighted / static_cast<double>(solid));
        return out;
    }

    // --- collision filter ---------------------------------------------------------------------

    CollisionFilter CollisionFilter::FromMaterials(const MaterialLibrary& materials, PaletteId palette, uint32_t excludedFlags) {
        CollisionFilter filter;
        if (palette >= materials.PaletteCount()) {
            Log::Warn("CollisionFilter::FromMaterials: palette {} does not exist, every material collides", palette);
            return filter;
        }
        for (uint32_t id = 1; id < 256; ++id) {
            if ((materials.Get(palette, static_cast<uint8_t>(id)).flags & excludedFlags) != 0) filter.collidable[id] = false;
        }
        return filter;
    }

    // --- chunk collision mesh -----------------------------------------------------------------

    ChunkMesh BuildChunkCollisionMesh(const Chunk& chunk, const CollisionFilter& filter,
                                      const std::function<bool(const glm::ivec3& local)>& outside) {
        ChunkMesh mesh;
        if (chunk.IsEmpty()) return mesh;

        constexpr int N = kChunkSize;
        static_assert(N == 32, "face masks are one 32 bit word per row");

        // Collidability bit set in plain x/y/z order: rows[y + z * 32] has bit x set.
        std::vector<uint32_t> rows(static_cast<size_t>(N) * N, 0u);
        bool any = false;
        for (int z = 0; z < N; ++z)
            for (int y = 0; y < N; ++y) {
                uint32_t bits = 0;
                for (int x = 0; x < N; ++x) {
                    if (filter.collidable[chunk.Get(x, y, z)]) bits |= 1u << x;
                }
                rows[static_cast<size_t>(y + z * N)] = bits;
                any = any || bits != 0;
            }
        if (!any) return mesh;

        const auto collidable = [&](const glm::ivec3& p) {
            return ((rows[static_cast<size_t>(p.y + p.z * N)] >> p.x) & 1u) != 0;
        };

        for (int axis = 0; axis < 3; ++axis) {
            // (u, v, axis) is a cyclic permutation of (x, y, z), so e_u x e_v = e_axis.
            const int u = (axis + 1) % 3;
            const int v = (axis + 2) % 3;

            for (int side = 0; side < 2; ++side) {
                const int step = side == 0 ? 1 : -1;      // direction the faces look at

                for (int slice = 0; slice < N; ++slice) {
                    const int neighbourSlice = slice + step;
                    const bool neighbourInside = neighbourSlice >= 0 && neighbourSlice < N;

                    // 1. Mask of exposed faces of this slice: mask[v] has bit u set.
                    std::array<uint32_t, N> mask{};
                    bool anyFace = false;
                    glm::ivec3 p(0);
                    p[axis] = slice;
                    for (int j = 0; j < N; ++j) {
                        p[v] = j;
                        uint32_t bits = 0;
                        for (int i = 0; i < N; ++i) {
                            p[u] = i;
                            if (!collidable(p)) continue;
                            glm::ivec3 q = p;
                            q[axis] = neighbourSlice;
                            const bool covered = neighbourInside ? collidable(q) : (outside ? outside(q) : false);
                            if (!covered) bits |= 1u << i;
                        }
                        mask[static_cast<size_t>(j)] = bits;
                        anyFace = anyFace || bits != 0;
                    }
                    if (!anyFace) continue;

                    // 2. Greedy rectangles: take the first run of a row, extend it over the following rows.
                    const float plane = static_cast<float>(step > 0 ? slice + 1 : slice);
                    for (int j = 0; j < N; ++j) {
                        while (mask[static_cast<size_t>(j)] != 0) {
                            const uint32_t row = mask[static_cast<size_t>(j)];
                            const int i = std::countr_zero(row);
                            const int w = std::countr_one(row >> i);
                            const uint32_t run = (w >= 32 ? ~0u : ((1u << w) - 1u)) << i;
                            int h = 1;
                            while (j + h < N && (mask[static_cast<size_t>(j + h)] & run) == run) ++h;
                            for (int k = 0; k < h; ++k) mask[static_cast<size_t>(j + k)] &= ~run;

                            // 3. One quad. Corners 0-1-2-3 run counter-clockwise seen from +axis.
                            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
                            const float u0 = static_cast<float>(i), u1 = static_cast<float>(i + w);
                            const float v0 = static_cast<float>(j), v1 = static_cast<float>(j + h);
                            const float corners[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
                            for (const auto& corner : corners) {
                                glm::vec3 vertex(0.0f);
                                vertex[axis] = plane;
                                vertex[u] = corner[0];
                                vertex[v] = corner[1];
                                mesh.vertices.push_back(vertex);
                            }
                            if (step > 0) {
                                mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                            } else {
                                mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
                            }
                        }
                    }
                }
            }
        }
        return mesh;
    }
}
