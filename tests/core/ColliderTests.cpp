// Collision helpers: greedy box decomposition, mass properties, collision filter and the
// greedy-meshed collision surface of a chunk. Every result is checked against a brute-force
// reference computed straight from the voxels.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <random>
#include <vector>

import vortex.core;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {

    VoxelVolume RandomVolume(const glm::ivec3& size, uint32_t seed, uint32_t solidPercent, uint32_t materialCount) {
        std::mt19937 rng(seed);
        VoxelVolume volume(size);
        for (uint8_t& v : volume.Data()) {
            const bool solid = rng() % 100u < solidPercent;
            const uint8_t id = static_cast<uint8_t>(1u + rng() % materialCount);
            v = solid ? id : uint8_t{0};
        }
        return volume;
    }

    /// Checks the BuildColliderBoxes contract: disjoint boxes whose union is exactly the solid set.
    void ExpectExactCover(const VoxelVolume& volume, const std::vector<ColliderBox>& boxes, bool mergeMaterials) {
        std::vector<int> covered(volume.Data().size(), 0);
        for (const ColliderBox& box : boxes) {
            ASSERT_GT(box.size.x, 0);
            ASSERT_GT(box.size.y, 0);
            ASSERT_GT(box.size.z, 0);
            ASSERT_TRUE(volume.InBounds(box.min.x, box.min.y, box.min.z));
            const glm::ivec3 last = box.min + box.size - glm::ivec3(1);
            ASSERT_TRUE(volume.InBounds(last.x, last.y, last.z));
            EXPECT_EQ(box.material, volume.Get(box.min));
            EXPECT_NE(box.material, 0);
            for (int z = box.min.z; z <= last.z; ++z)
                for (int y = box.min.y; y <= last.y; ++y)
                    for (int x = box.min.x; x <= last.x; ++x) {
                        ++covered[volume.Index(x, y, z)];
                        if (!mergeMaterials) {
                            ASSERT_EQ(volume.Get(x, y, z), box.material);
                        }
                    }
        }
        size_t wrong = 0;
        for (size_t i = 0; i < covered.size(); ++i) {
            const int expected = volume.Data()[i] != 0 ? 1 : 0;
            if (covered[i] != expected) ++wrong;
        }
        EXPECT_EQ(wrong, 0u) << "voxels covered a wrong number of times";
    }

    // --- chunk mesh helpers ---

    glm::vec3 TriangleCross(const ChunkMesh& mesh, size_t triangle) {
        const glm::vec3 a = mesh.vertices[mesh.indices[triangle * 3 + 0]];
        const glm::vec3 b = mesh.vertices[mesh.indices[triangle * 3 + 1]];
        const glm::vec3 c = mesh.vertices[mesh.indices[triangle * 3 + 2]];
        return glm::cross(b - a, c - a);
    }

    glm::vec3 TriangleCentroid(const ChunkMesh& mesh, size_t triangle) {
        return (mesh.vertices[mesh.indices[triangle * 3 + 0]] + mesh.vertices[mesh.indices[triangle * 3 + 1]] +
                mesh.vertices[mesh.indices[triangle * 3 + 2]]) / 3.0f;
    }

    double MeshArea(const ChunkMesh& mesh) {
        double area = 0.0;
        for (size_t t = 0; t < mesh.indices.size() / 3; ++t) area += 0.5 * static_cast<double>(glm::length(TriangleCross(mesh, t)));
        return area;
    }

    void ExpectWellFormed(const ChunkMesh& mesh) {
        ASSERT_EQ(mesh.indices.size() % 6, 0u);                              // two triangles per quad
        ASSERT_EQ(mesh.vertices.size() / 4 * 6, mesh.indices.size());        // four vertices per quad
        ASSERT_EQ(mesh.vertices.size() % 4, 0u);
        for (uint32_t index : mesh.indices) ASSERT_LT(index, mesh.vertices.size());
        for (const glm::vec3& v : mesh.vertices) {
            for (int a = 0; a < 3; ++a) {
                ASSERT_GE(v[a], 0.0f);
                ASSERT_LE(v[a], static_cast<float>(kChunkSize));
                ASSERT_EQ(v[a], std::round(v[a]));                           // on the voxel lattice
            }
        }
    }

    bool OutsideNever(const glm::ivec3&) { return false; }
    bool OutsideAlways(const glm::ivec3&) { return true; }

    /// Deterministic pseudo-random neighbourhood for chunk borders.
    bool OutsideHash(const glm::ivec3& p) {
        const uint32_t h = static_cast<uint32_t>(p.x + 7) * 73856093u ^ static_cast<uint32_t>(p.y + 7) * 19349663u ^
                           static_cast<uint32_t>(p.z + 7) * 83492791u;
        return ((h >> 4) & 1u) != 0;
    }

    bool ReferenceCollidable(const Chunk& chunk, const CollisionFilter& filter, const glm::ivec3& p, bool (*outside)(const glm::ivec3&)) {
        if (Chunk::InBounds(p.x, p.y, p.z)) return filter.collidable[chunk.Get(p.x, p.y, p.z)];
        return outside(p);
    }

    uint32_t CountExposedFaces(const Chunk& chunk, const CollisionFilter& filter, bool (*outside)(const glm::ivec3&)) {
        static const glm::ivec3 kDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        uint32_t faces = 0;
        for (int z = 0; z < kChunkSize; ++z)
            for (int y = 0; y < kChunkSize; ++y)
                for (int x = 0; x < kChunkSize; ++x) {
                    if (!filter.collidable[chunk.Get(x, y, z)]) continue;
                    for (const glm::ivec3& d : kDirs) {
                        if (!ReferenceCollidable(chunk, filter, glm::ivec3(x, y, z) + d, outside)) ++faces;
                    }
                }
        return faces;
    }

    /// Every triangle must lie on a face between a collidable voxel (behind it) and a
    /// non-collidable one (in front of it, where its normal points).
    void ExpectTrianglesFaceOutwards(const ChunkMesh& mesh, const Chunk& chunk, const CollisionFilter& filter,
                                     bool (*outside)(const glm::ivec3&)) {
        size_t wrong = 0;
        for (size_t t = 0; t < mesh.indices.size() / 3; ++t) {
            const glm::vec3 cross = TriangleCross(mesh, t);
            const float length = glm::length(cross);
            ASSERT_GT(length, 0.0f);
            const glm::vec3 normal = cross / length;
            // Axis aligned unit normal.
            ASSERT_NEAR(std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z), 1.0f, 1e-5f);
            const glm::vec3 centroid = TriangleCentroid(mesh, t);
            const glm::ivec3 behind = glm::ivec3(glm::floor(centroid - normal * 0.5f));
            const glm::ivec3 front = glm::ivec3(glm::floor(centroid + normal * 0.5f));
            if (!ReferenceCollidable(chunk, filter, behind, outside) || ReferenceCollidable(chunk, filter, front, outside)) ++wrong;
        }
        EXPECT_EQ(wrong, 0u) << "triangles not on an exposed face or wound the wrong way";
    }
}

// --- BuildColliderBoxes ------------------------------------------------------------------------

TEST(ColliderBoxes, EmptyVolumesGiveNoBoxes) {
    EXPECT_TRUE(BuildColliderBoxes(VoxelVolume{}).empty());
    EXPECT_TRUE(BuildColliderBoxes(VoxelVolume(glm::ivec3(0, 4, 4))).empty());
    EXPECT_TRUE(BuildColliderBoxes(VoxelVolume(glm::ivec3(5, 6, 7))).empty());
    EXPECT_TRUE(BuildColliderBoxes(VoxelVolume(glm::ivec3(5, 6, 7)), false).empty());
}

TEST(ColliderBoxes, SolidCuboidIsOneBox) {
    VoxelVolume volume(glm::ivec3(9, 7, 11));
    volume.FillBox({2, 1, 3}, {8, 6, 10}, 5);
    for (bool merge : {true, false}) {
        const std::vector<ColliderBox> boxes = BuildColliderBoxes(volume, merge);
        ASSERT_EQ(boxes.size(), 1u);
        EXPECT_EQ(boxes[0].min, glm::ivec3(2, 1, 3));
        EXPECT_EQ(boxes[0].size, glm::ivec3(6, 5, 7));
        EXPECT_EQ(boxes[0].material, 5);
    }

    VoxelVolume full(glm::ivec3(4, 4, 4));
    full.FillBox({0, 0, 0}, {4, 4, 4}, 1);
    const std::vector<ColliderBox> boxes = BuildColliderBoxes(full);
    ASSERT_EQ(boxes.size(), 1u);
    EXPECT_EQ(boxes[0].min, glm::ivec3(0));
    EXPECT_EQ(boxes[0].size, glm::ivec3(4));
}

TEST(ColliderBoxes, MaterialsSplitBoxesOnlyWhenNotMerged) {
    VoxelVolume volume(glm::ivec3(8, 4, 4));
    volume.FillBox({0, 0, 0}, {4, 4, 4}, 1);
    volume.FillBox({4, 0, 0}, {8, 4, 4}, 2);

    const std::vector<ColliderBox> merged = BuildColliderBoxes(volume, true);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0].size, glm::ivec3(8, 4, 4));
    EXPECT_EQ(merged[0].material, 1);        // id of the first voxel

    const std::vector<ColliderBox> split = BuildColliderBoxes(volume, false);
    ASSERT_EQ(split.size(), 2u);
    ExpectExactCover(volume, split, false);
    EXPECT_EQ(split[0].material, 1);
    EXPECT_EQ(split[0].size, glm::ivec3(4, 4, 4));
    EXPECT_EQ(split[1].material, 2);
    EXPECT_EQ(split[1].min, glm::ivec3(4, 0, 0));
}

TEST(ColliderBoxes, RandomVolumesAreCoveredExactlyByDisjointBoxes) {
    const glm::ivec3 sizes[] = {{1, 1, 1}, {13, 1, 1}, {1, 9, 5}, {7, 11, 5}, {16, 16, 16}, {20, 3, 17}};
    const uint32_t densities[] = {10, 50, 85, 100};
    uint32_t seed = 1;
    for (const glm::ivec3& size : sizes)
        for (uint32_t density : densities)
            for (uint32_t materialCount : {1u, 3u}) {
                const VoxelVolume volume = RandomVolume(size, seed++, density, materialCount);
                const uint32_t solid = volume.CountSolid();
                for (bool merge : {true, false}) {
                    SCOPED_TRACE(testing::Message() << "seed " << seed - 1 << " merge " << merge);
                    const std::vector<ColliderBox> boxes = BuildColliderBoxes(volume, merge);
                    ExpectExactCover(volume, boxes, merge);
                    EXPECT_LE(boxes.size(), solid);
                    uint64_t boxVolume = 0;
                    for (const ColliderBox& b : boxes) boxVolume += static_cast<uint64_t>(b.size.x) * static_cast<uint64_t>(b.size.y) * static_cast<uint64_t>(b.size.z);
                    EXPECT_EQ(boxVolume, solid);
                }
                // Merging across materials can never need more boxes than keeping them apart
                // when there is only one material.
                if (materialCount == 1u) {
                    EXPECT_EQ(BuildColliderBoxes(volume, true).size(), BuildColliderBoxes(volume, false).size());
                }
            }
}

TEST(ColliderBoxes, MergesStructuredShapesIntoFewBoxes) {
    // A hollow shell of 10^3 with a 8^3 cavity: far fewer boxes than voxels.
    VoxelVolume volume(glm::ivec3(10));
    volume.FillBox({0, 0, 0}, {10, 10, 10}, 1);
    volume.FillBox({1, 1, 1}, {9, 9, 9}, 0);
    const std::vector<ColliderBox> boxes = BuildColliderBoxes(volume);
    ExpectExactCover(volume, boxes, true);
    EXPECT_LE(boxes.size(), 6u);
}

// --- ComputeMassProperties ---------------------------------------------------------------------

namespace {
    struct MassFixture {
        MassFixture() {
            Material light;
            light.density = 1000.0f;
            Material heavy;
            heavy.density = 3000.0f;
            materials.Set(kWorldPalette, 1, light, "light");
            materials.Set(kWorldPalette, 2, heavy, "heavy");
        }
        MaterialLibrary materials;
    };
}

TEST(MassProperties, EmptyVolumeHasNoMass) {
    MassFixture f;
    for (const VoxelVolume& volume : {VoxelVolume{}, VoxelVolume(glm::ivec3(4, 5, 6))}) {
        const MassProperties mp = ComputeMassProperties(volume, f.materials, kWorldPalette, 0.1f);
        EXPECT_EQ(mp.solidVoxels, 0u);
        EXPECT_EQ(mp.mass, 0.0f);
        EXPECT_EQ(mp.centerOfMass, glm::vec3(0.0f));
    }
}

TEST(MassProperties, SingleVoxel) {
    MassFixture f;
    VoxelVolume volume(glm::ivec3(5, 5, 5));
    volume.Set(3, 1, 4, 2);
    const MassProperties mp = ComputeMassProperties(volume, f.materials, kWorldPalette, 0.5f);
    EXPECT_EQ(mp.solidVoxels, 1u);
    EXPECT_NEAR(mp.mass, 3000.0f * 0.125f, 1e-3f);
    EXPECT_NEAR(mp.centerOfMass.x, 3.5f, 1e-5f);
    EXPECT_NEAR(mp.centerOfMass.y, 1.5f, 1e-5f);
    EXPECT_NEAR(mp.centerOfMass.z, 4.5f, 1e-5f);
}

TEST(MassProperties, UniformCuboid) {
    MassFixture f;
    VoxelVolume volume(glm::ivec3(8, 8, 8));
    volume.FillBox({1, 2, 0}, {5, 4, 6}, 1);     // 4 x 2 x 6 = 48 voxels
    const MassProperties mp = ComputeMassProperties(volume, f.materials, kWorldPalette, 0.1f);
    EXPECT_EQ(mp.solidVoxels, 48u);
    EXPECT_NEAR(mp.mass, 48.0f, 1e-3f);          // 48 * 1000 kg/m^3 * (0.1 m)^3
    EXPECT_NEAR(mp.centerOfMass.x, 3.0f, 1e-5f);
    EXPECT_NEAR(mp.centerOfMass.y, 3.0f, 1e-5f);
    EXPECT_NEAR(mp.centerOfMass.z, 3.0f, 1e-5f);
}

TEST(MassProperties, CentreOfMassIsDensityWeighted) {
    MassFixture f;
    VoxelVolume volume(glm::ivec3(2, 1, 1));
    volume.Set(0, 0, 0, 1);
    volume.Set(1, 0, 0, 2);
    const MassProperties mp = ComputeMassProperties(volume, f.materials, kWorldPalette, 0.5f);
    EXPECT_EQ(mp.solidVoxels, 2u);
    EXPECT_NEAR(mp.mass, 4000.0f * 0.125f, 1e-3f);
    EXPECT_NEAR(mp.centerOfMass.x, (0.5f * 1000.0f + 1.5f * 3000.0f) / 4000.0f, 1e-5f);
    EXPECT_NEAR(mp.centerOfMass.y, 0.5f, 1e-5f);
    EXPECT_NEAR(mp.centerOfMass.z, 0.5f, 1e-5f);
}

TEST(MassProperties, MatchesSumOverSingleMaterialBoxes) {
    MassFixture f;
    const VoxelVolume volume = RandomVolume(glm::ivec3(12, 9, 10), 77, 60, 2);
    const float voxelSize = 0.25f;
    const MassProperties mp = ComputeMassProperties(volume, f.materials, kWorldPalette, voxelSize);

    double mass = 0.0;
    glm::dvec3 moment(0.0);
    for (const ColliderBox& box : BuildColliderBoxes(volume, false)) {
        const double m = static_cast<double>(f.materials.Get(kWorldPalette, box.material).density) *
                         static_cast<double>(box.size.x * box.size.y * box.size.z) * 0.25 * 0.25 * 0.25;
        mass += m;
        moment += (glm::dvec3(box.min) + glm::dvec3(box.size) * 0.5) * m;
    }
    EXPECT_EQ(mp.solidVoxels, volume.CountSolid());
    EXPECT_NEAR(static_cast<double>(mp.mass), mass, mass * 1e-5);
    const glm::dvec3 com = moment / mass;
    EXPECT_NEAR(static_cast<double>(mp.centerOfMass.x), com.x, 1e-4);
    EXPECT_NEAR(static_cast<double>(mp.centerOfMass.y), com.y, 1e-4);
    EXPECT_NEAR(static_cast<double>(mp.centerOfMass.z), com.z, 1e-4);
}

// --- CollisionFilter ---------------------------------------------------------------------------

TEST(CollisionFilter, DefaultCollidesWithEverythingButAir) {
    const CollisionFilter filter;
    EXPECT_FALSE(filter.collidable[0]);
    for (size_t id = 1; id < 256; ++id) EXPECT_TRUE(filter.collidable[id]);
}

TEST(CollisionFilter, FromMaterialsExcludesFlaggedMaterials) {
    MaterialLibrary materials;
    Material water;
    water.flags = MaterialFlag_Liquid;
    Material leaves;
    leaves.flags = MaterialFlag_Foliage | MaterialFlag_Flammable;
    materials.Set(kWorldPalette, 1, Material{}, "stone");
    materials.Set(kWorldPalette, 2, water, "water");
    materials.Set(kWorldPalette, 3, leaves, "leaves");

    const CollisionFilter liquids = CollisionFilter::FromMaterials(materials, kWorldPalette, MaterialFlag_Liquid);
    EXPECT_FALSE(liquids.collidable[0]);
    EXPECT_TRUE(liquids.collidable[1]);
    EXPECT_FALSE(liquids.collidable[2]);
    EXPECT_TRUE(liquids.collidable[3]);
    EXPECT_TRUE(liquids.collidable[200]);

    const CollisionFilter soft = CollisionFilter::FromMaterials(materials, kWorldPalette, MaterialFlag_Liquid | MaterialFlag_Foliage);
    EXPECT_TRUE(soft.collidable[1]);
    EXPECT_FALSE(soft.collidable[2]);
    EXPECT_FALSE(soft.collidable[3]);        // any of the excluded flags is enough

    const CollisionFilter none = CollisionFilter::FromMaterials(materials, kWorldPalette, MaterialFlag_None);
    EXPECT_FALSE(none.collidable[0]);
    for (size_t id = 1; id < 256; ++id) EXPECT_TRUE(none.collidable[id]);
}

// --- BuildChunkCollisionMesh -------------------------------------------------------------------

TEST(ChunkCollisionMesh, EmptyChunkHasNoMesh) {
    const auto chunk = std::make_unique<Chunk>();
    const ChunkMesh mesh = BuildChunkCollisionMesh(*chunk, CollisionFilter{}, OutsideNever);
    EXPECT_TRUE(mesh.Empty());
    EXPECT_TRUE(mesh.vertices.empty());
}

TEST(ChunkCollisionMesh, SingleVoxelIsTwelveOutwardTriangles) {
    const glm::ivec3 positions[] = {{5, 6, 7}, {0, 0, 0}, {31, 31, 31}, {0, 31, 15}};
    for (const glm::ivec3& p : positions) {
        SCOPED_TRACE(testing::Message() << p.x << "," << p.y << "," << p.z);
        auto chunk = std::make_unique<Chunk>();
        chunk->Set(p.x, p.y, p.z, 1);
        const ChunkMesh mesh = BuildChunkCollisionMesh(*chunk, CollisionFilter{}, OutsideNever);
        ExpectWellFormed(mesh);
        ASSERT_EQ(mesh.indices.size(), 36u);
        ASSERT_EQ(mesh.vertices.size(), 24u);

        const glm::vec3 center = glm::vec3(p) + 0.5f;
        int facesPerDirection[6] = {};
        for (size_t t = 0; t < 12; ++t) {
            const glm::vec3 cross = TriangleCross(mesh, t);
            EXPECT_NEAR(glm::length(cross), 1.0f, 1e-5f);                    // half a unit square
            const glm::vec3 fromCenter = TriangleCentroid(mesh, t) - center;
            EXPECT_GT(glm::dot(cross, fromCenter), 0.0f) << "triangle " << t << " faces the voxel";
            for (int a = 0; a < 3; ++a) {
                if (cross[a] > 0.5f) ++facesPerDirection[a * 2];
                if (cross[a] < -0.5f) ++facesPerDirection[a * 2 + 1];
            }
        }
        for (int count : facesPerDirection) EXPECT_EQ(count, 2);
        for (const glm::vec3& v : mesh.vertices) {
            for (int a = 0; a < 3; ++a) {
                EXPECT_GE(v[a], static_cast<float>(p[a]));
                EXPECT_LE(v[a], static_cast<float>(p[a] + 1));
            }
        }
    }
}

TEST(ChunkCollisionMesh, FullSlabIsSixQuadsWhenNeighboursAreOpen) {
    auto chunk = std::make_unique<Chunk>();
    for (int z = 0; z < kChunkSize; ++z)
        for (int x = 0; x < kChunkSize; ++x) chunk->Set(x, 0, z, static_cast<uint8_t>(1 + ((x + z) & 3)));   // materials do not matter

    const ChunkMesh mesh = BuildChunkCollisionMesh(*chunk, CollisionFilter{}, OutsideNever);
    ExpectWellFormed(mesh);
    EXPECT_EQ(mesh.vertices.size(), 6u * 4u);
    EXPECT_EQ(mesh.indices.size(), 6u * 6u);
    EXPECT_NEAR(MeshArea(mesh), 2.0 * 32.0 * 32.0 + 4.0 * 32.0, 1e-3);
    ExpectTrianglesFaceOutwards(mesh, *chunk, CollisionFilter{}, OutsideNever);
}

TEST(ChunkCollisionMesh, BorderFacesDisappearWhenNeighboursAreSolid) {
    auto chunk = std::make_unique<Chunk>();
    for (int z = 0; z < kChunkSize; ++z)
        for (int x = 0; x < kChunkSize; ++x) chunk->Set(x, 0, z, 1);

    const ChunkMesh mesh = BuildChunkCollisionMesh(*chunk, CollisionFilter{}, OutsideAlways);
    ExpectWellFormed(mesh);
    // Only the top of the slab is left: one quad.
    ASSERT_EQ(mesh.vertices.size(), 4u);
    ASSERT_EQ(mesh.indices.size(), 6u);
    for (const glm::vec3& v : mesh.vertices) EXPECT_EQ(v.y, 1.0f);
    for (size_t t = 0; t < 2; ++t) {
        const glm::vec3 cross = TriangleCross(mesh, t);
        EXPECT_GT(cross.y, 0.0f);
        EXPECT_EQ(cross.x, 0.0f);
        EXPECT_EQ(cross.z, 0.0f);
    }
    EXPECT_NEAR(MeshArea(mesh), 32.0 * 32.0, 1e-3);

    // A completely solid chunk surrounded by solid chunks has no surface at all.
    chunk->Fill(3);
    EXPECT_TRUE(BuildChunkCollisionMesh(*chunk, CollisionFilter{}, OutsideAlways).Empty());
    // ... and is a plain cube when the surroundings are open.
    const ChunkMesh cube = BuildChunkCollisionMesh(*chunk, CollisionFilter{}, OutsideNever);
    ExpectWellFormed(cube);
    EXPECT_EQ(cube.indices.size(), 36u);
    EXPECT_NEAR(MeshArea(cube), 6.0 * 32.0 * 32.0, 1e-2);
}

TEST(ChunkCollisionMesh, OutsideIsAskedOnlyAboutOutOfChunkNeighbours) {
    auto chunk = std::make_unique<Chunk>();
    chunk->Fill(1);
    uint32_t calls = 0;
    bool valid = true;
    const ChunkMesh mesh = BuildChunkCollisionMesh(*chunk, CollisionFilter{}, [&](const glm::ivec3& p) {
        ++calls;
        int outsideAxes = 0;
        for (int a = 0; a < 3; ++a) {
            if (p[a] == -1 || p[a] == kChunkSize) ++outsideAxes;
            else if (p[a] < 0 || p[a] >= kChunkSize) valid = false;
        }
        if (outsideAxes != 1) valid = false;
        return p.y == kChunkSize;            // only the chunk above is solid
    });
    EXPECT_TRUE(valid) << "outside() received a position that is not a face neighbour of the chunk";
    EXPECT_EQ(calls, 6u * 32u * 32u);
    ExpectWellFormed(mesh);
    EXPECT_EQ(mesh.indices.size(), 5u * 6u);     // every side but the top
    EXPECT_NEAR(MeshArea(mesh), 5.0 * 32.0 * 32.0, 1e-2);
}

TEST(ChunkCollisionMesh, NonCollidableMaterialsAreTreatedAsEmpty) {
    CollisionFilter filter;
    filter.collidable[2] = false;                // "water"

    auto chunk = std::make_unique<Chunk>();
    chunk->Set(10, 10, 10, 2);
    EXPECT_TRUE(BuildChunkCollisionMesh(*chunk, filter, OutsideNever).Empty());

    // Stone next to water: the face between them still exists.
    chunk->Set(11, 10, 10, 1);
    const ChunkMesh mesh = BuildChunkCollisionMesh(*chunk, filter, OutsideNever);
    ExpectWellFormed(mesh);
    EXPECT_EQ(mesh.indices.size(), 36u);
    EXPECT_NEAR(MeshArea(mesh), 6.0, 1e-4);
    ExpectTrianglesFaceOutwards(mesh, *chunk, filter, OutsideNever);
}

TEST(ChunkCollisionMesh, RandomChunksAreaEqualsExposedFaces) {
    CollisionFilter filter;
    filter.collidable[3] = false;

    struct Case {
        uint32_t seed;
        uint32_t solidPercent;
        bool (*outside)(const glm::ivec3&);
    };
    const Case cases[] = {{1, 5, OutsideNever}, {2, 50, OutsideNever}, {3, 50, OutsideAlways},
                          {4, 90, OutsideHash}, {5, 30, OutsideHash}, {6, 100, OutsideHash}};
    for (const Case& c : cases) {
        SCOPED_TRACE(testing::Message() << "seed " << c.seed);
        std::mt19937 rng(c.seed);
        auto chunk = std::make_unique<Chunk>();
        for (int z = 0; z < kChunkSize; ++z)
            for (int y = 0; y < kChunkSize; ++y)
                for (int x = 0; x < kChunkSize; ++x) {
                    const bool solid = rng() % 100u < c.solidPercent;
                    const uint8_t id = static_cast<uint8_t>(1u + rng() % 3u);
                    if (solid) chunk->Set(x, y, z, id);
                }

        const ChunkMesh mesh = BuildChunkCollisionMesh(*chunk, filter, c.outside);
        ExpectWellFormed(mesh);
        const uint32_t faces = CountExposedFaces(*chunk, filter, c.outside);
        EXPECT_NEAR(MeshArea(mesh), static_cast<double>(faces), 0.01);
        EXPECT_LE(mesh.indices.size() / 6, faces);                  // merging never adds quads
        ExpectTrianglesFaceOutwards(mesh, *chunk, filter, c.outside);
    }
}

TEST(ChunkCollisionMesh, BlockyTerrainMergesIntoFewQuads) {
    // Two terraces: a 32 x 4 x 32 floor with a 16 x 4 x 32 step on top.
    auto chunk = std::make_unique<Chunk>();
    for (int z = 0; z < kChunkSize; ++z)
        for (int x = 0; x < kChunkSize; ++x) {
            const int height = x < 16 ? 8 : 4;
            for (int y = 0; y < height; ++y) chunk->Set(x, y, z, 1);
        }
    const ChunkMesh mesh = BuildChunkCollisionMesh(*chunk, CollisionFilter{}, OutsideAlways);
    ExpectWellFormed(mesh);
    EXPECT_EQ(mesh.indices.size(), 3u * 6u);     // two tops and the riser between them
    EXPECT_NEAR(MeshArea(mesh), 32.0 * 32.0 + 4.0 * 32.0, 1e-3);
    ExpectTrianglesFaceOutwards(mesh, *chunk, CollisionFilter{}, OutsideAlways);
}
