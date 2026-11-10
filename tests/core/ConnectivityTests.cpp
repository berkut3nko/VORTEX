// Connected components of voxel volumes, splitting volumes into pieces, and the search for
// terrain that lost its support in a streaming world.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <random>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    constexpr int kGround = 40;          // flat world: voxels with y < 40 are solid
    constexpr uint8_t kSurface = 2;
    constexpr uint8_t kRock = 3;
    constexpr uint8_t kWater = 4;

    struct Fixture {
        Fixture() : store(512), jobs(3) {
            WorldConfig config;
            config.voxelSize = 0.5f;
            config.windowSize = glm::ivec3(8, 8, 8);
            Material hard;
            hard.hardness = 10.0f;
            Material water;
            water.flags = MaterialFlag_Liquid;
            materials.Set(kWorldPalette, kSurface, Material{}, "surface");
            materials.Set(kWorldPalette, kRock, hard, "rock");
            materials.Set(kWorldPalette, kWater, water, "water");
            world = std::make_unique<World>(config, store, jobs, CreateFlatGenerator(kGround, kSurface, kRock), &materials);
            // Focus at voxel (0, 40, 0): chunks -4..3 in x / z and -3..4 in y are loaded.
            world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
        }
        ChunkStore store;
        JobSystem jobs;
        MaterialLibrary materials;
        std::unique_ptr<World> world;
    };

    uint32_t CountWorldSolid(const World& world, const glm::ivec3& min, const glm::ivec3& max) {
        uint32_t n = 0;
        for (int z = min.z; z < max.z; ++z)
            for (int y = min.y; y < max.y; ++y)
                for (int x = min.x; x < max.x; ++x) n += world.IsSolid({x, y, z}) ? 1u : 0u;
        return n;
    }

    /// Checks every documented invariant of ComponentLabels against the volume it was built from.
    void ExpectConsistentLabels(const VoxelVolume& volume, const ComponentLabels& labels) {
        ASSERT_EQ(labels.size, volume.Size());
        ASSERT_EQ(labels.labels.size(), volume.Data().size());
        ASSERT_EQ(labels.voxelCounts.size(), static_cast<size_t>(labels.count) + 1u);
        ASSERT_EQ(labels.boundsMin.size(), static_cast<size_t>(labels.count) + 1u);
        ASSERT_EQ(labels.boundsMax.size(), static_cast<size_t>(labels.count) + 1u);

        const glm::ivec3 size = volume.Size();
        std::vector<uint32_t> counts(static_cast<size_t>(labels.count) + 1u, 0u);
        std::vector<glm::ivec3> mins(counts.size(), size);
        std::vector<glm::ivec3> maxs(counts.size(), glm::ivec3(0));
        size_t mismatches = 0;
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x) {
                    const uint32_t label = labels.labels[volume.Index(x, y, z)];
                    ASSERT_LE(label, labels.count);
                    const bool solid = volume.Get(x, y, z) != 0;
                    if (solid != (label != 0)) ++mismatches;
                    if (label == 0) continue;
                    ++counts[label];
                    mins[label] = glm::min(mins[label], glm::ivec3(x, y, z));
                    maxs[label] = glm::max(maxs[label], glm::ivec3(x + 1, y + 1, z + 1));
                    // Face neighbours that are solid must carry the same label.
                    if (volume.Get(x + 1, y, z) != 0 && labels.labels[volume.Index(x + 1, y, z)] != label) ++mismatches;
                    if (volume.Get(x, y + 1, z) != 0 && labels.labels[volume.Index(x, y + 1, z)] != label) ++mismatches;
                    if (volume.Get(x, y, z + 1) != 0 && labels.labels[volume.Index(x, y, z + 1)] != label) ++mismatches;
                }
        EXPECT_EQ(mismatches, 0u);
        for (uint32_t c = 1; c <= labels.count; ++c) {
            EXPECT_GT(counts[c], 0u) << "label " << c << " is unused";
            EXPECT_EQ(labels.voxelCounts[c], counts[c]);
            EXPECT_EQ(labels.boundsMin[c], mins[c]);
            EXPECT_EQ(labels.boundsMax[c], maxs[c]);
        }
    }

    /// Reference component count: union-find over face-adjacent solid voxels.
    uint32_t ReferenceComponentCount(const VoxelVolume& volume) {
        const glm::ivec3 size = volume.Size();
        std::vector<size_t> parent(volume.Data().size());
        for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
        const auto find = [&](size_t i) {
            while (parent[i] != i) {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }
            return i;
        };
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x) {
                    if (volume.Get(x, y, z) == 0) continue;
                    const size_t self = volume.Index(x, y, z);
                    if (volume.Get(x + 1, y, z) != 0) parent[find(volume.Index(x + 1, y, z))] = find(self);
                    if (volume.Get(x, y + 1, z) != 0) parent[find(volume.Index(x, y + 1, z))] = find(self);
                    if (volume.Get(x, y, z + 1) != 0) parent[find(volume.Index(x, y, z + 1))] = find(self);
                }
        uint32_t roots = 0;
        for (size_t i = 0; i < parent.size(); ++i) {
            if (volume.Data()[i] != 0 && parent[i] == i) ++roots;
        }
        return roots;
    }

    VoxelVolume RandomVolume(const glm::ivec3& size, uint32_t seed, uint32_t solidPercent) {
        std::mt19937 rng(seed);
        VoxelVolume volume(size);
        for (uint8_t& v : volume.Data()) {
            const bool solid = rng() % 100u < solidPercent;
            const uint8_t id = static_cast<uint8_t>(1u + rng() % 5u);
            v = solid ? id : uint8_t{0};
        }
        return volume;
    }
}

// --- LabelComponents ---------------------------------------------------------------------------

TEST(Connectivity, EmptyVolumesHaveNoComponents) {
    for (const VoxelVolume& volume : {VoxelVolume{}, VoxelVolume(glm::ivec3(0, 3, 3)), VoxelVolume(glm::ivec3(6, 5, 4))}) {
        const ComponentLabels labels = LabelComponents(volume);
        EXPECT_EQ(labels.count, 0u);
        EXPECT_EQ(labels.size, volume.Size());
        EXPECT_EQ(labels.voxelCounts.size(), 1u);
        EXPECT_EQ(labels.boundsMin.size(), 1u);
        EXPECT_EQ(labels.boundsMax.size(), 1u);
        for (uint32_t label : labels.labels) EXPECT_EQ(label, 0u);
    }
}

TEST(Connectivity, SolidCubeIsOneComponent) {
    VoxelVolume volume(glm::ivec3(8, 8, 8));
    volume.FillBox({1, 2, 3}, {5, 7, 6}, 1);
    const ComponentLabels labels = LabelComponents(volume);
    ExpectConsistentLabels(volume, labels);
    ASSERT_EQ(labels.count, 1u);
    EXPECT_EQ(labels.voxelCounts[1], 4u * 5u * 3u);
    EXPECT_EQ(labels.boundsMin[1], glm::ivec3(1, 2, 3));
    EXPECT_EQ(labels.boundsMax[1], glm::ivec3(5, 7, 6));
}

TEST(Connectivity, DifferentMaterialsStillConnect) {
    VoxelVolume volume(glm::ivec3(4, 1, 1));
    volume.Set(0, 0, 0, 1);
    volume.Set(1, 0, 0, 2);
    volume.Set(2, 0, 0, 3);
    const ComponentLabels labels = LabelComponents(volume);
    ASSERT_EQ(labels.count, 1u);
    EXPECT_EQ(labels.voxelCounts[1], 3u);
}

TEST(Connectivity, CubesTouchingByAFaceAreOneComponent) {
    VoxelVolume volume(glm::ivec3(6, 3, 3));
    volume.FillBox({0, 0, 0}, {2, 2, 2}, 1);
    volume.FillBox({2, 0, 0}, {4, 2, 2}, 2);
    const ComponentLabels labels = LabelComponents(volume);
    ExpectConsistentLabels(volume, labels);
    ASSERT_EQ(labels.count, 1u);
    EXPECT_EQ(labels.voxelCounts[1], 16u);
}

TEST(Connectivity, CubesTouchingOnlyByAnEdgeOrCornerAreSeparate) {
    // Edge contact: the cubes share the line x = 2, y = 2.
    VoxelVolume edge(glm::ivec3(4, 4, 2));
    edge.FillBox({0, 0, 0}, {2, 2, 2}, 1);
    edge.FillBox({2, 2, 0}, {4, 4, 2}, 1);
    const ComponentLabels edgeLabels = LabelComponents(edge);
    ExpectConsistentLabels(edge, edgeLabels);
    ASSERT_EQ(edgeLabels.count, 2u);
    EXPECT_EQ(edgeLabels.voxelCounts[1], 8u);
    EXPECT_EQ(edgeLabels.voxelCounts[2], 8u);
    EXPECT_NE(edgeLabels.labels[edge.Index(1, 1, 0)], edgeLabels.labels[edge.Index(2, 2, 0)]);

    // Corner contact: the cubes share the single point (2, 2, 2).
    VoxelVolume corner(glm::ivec3(4, 4, 4));
    corner.FillBox({0, 0, 0}, {2, 2, 2}, 1);
    corner.FillBox({2, 2, 2}, {4, 4, 4}, 1);
    const ComponentLabels cornerLabels = LabelComponents(corner);
    ExpectConsistentLabels(corner, cornerLabels);
    EXPECT_EQ(cornerLabels.count, 2u);
}

TEST(Connectivity, CheckerboardIsAllSingleVoxels) {
    VoxelVolume volume(glm::ivec3(5, 4, 3));
    uint32_t solid = 0;
    for (int z = 0; z < 3; ++z)
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 5; ++x) {
                if (((x + y + z) & 1) == 0) {
                    volume.Set(x, y, z, 1);
                    ++solid;
                }
            }
    const ComponentLabels labels = LabelComponents(volume);
    ExpectConsistentLabels(volume, labels);
    EXPECT_EQ(labels.count, solid);
    for (uint32_t c = 1; c <= labels.count; ++c) EXPECT_EQ(labels.voxelCounts[c], 1u);
}

TEST(Connectivity, WindingPathIsFollowedToItsEnd) {
    // A serpentine one voxel wide: a naive scanline labelling would split it.
    VoxelVolume volume(glm::ivec3(9, 9, 1));
    for (int y = 0; y < 9; y += 2) volume.FillBox({0, y, 0}, {9, y + 1, 1}, 1);
    for (int y = 1; y < 9; y += 2) volume.Set(((y / 2) & 1) == 0 ? 8 : 0, y, 0, 1);
    const ComponentLabels labels = LabelComponents(volume);
    ExpectConsistentLabels(volume, labels);
    ASSERT_EQ(labels.count, 1u);
    EXPECT_EQ(labels.voxelCounts[1], 5u * 9u + 4u);
    EXPECT_EQ(labels.boundsMin[1], glm::ivec3(0));
    EXPECT_EQ(labels.boundsMax[1], glm::ivec3(9, 9, 1));
}

TEST(Connectivity, LargeSolidVolumeDoesNotOverflowTheStack) {
    VoxelVolume volume(glm::ivec3(96, 96, 96));
    volume.FillBox({0, 0, 0}, {96, 96, 96}, 7);
    const ComponentLabels labels = LabelComponents(volume);
    ASSERT_EQ(labels.count, 1u);
    EXPECT_EQ(labels.voxelCounts[1], 96u * 96u * 96u);
    EXPECT_EQ(labels.boundsMax[1], glm::ivec3(96));
}

TEST(Connectivity, RandomVolumesMatchUnionFindReference) {
    const glm::ivec3 sizes[] = {{1, 1, 1}, {17, 1, 1}, {1, 6, 9}, {12, 10, 8}, {20, 20, 20}};
    uint32_t seed = 100;
    for (const glm::ivec3& size : sizes)
        for (uint32_t density : {15u, 35u, 60u, 100u}) {
            SCOPED_TRACE(testing::Message() << "seed " << seed);
            const VoxelVolume volume = RandomVolume(size, seed++, density);
            const ComponentLabels labels = LabelComponents(volume);
            ExpectConsistentLabels(volume, labels);
            EXPECT_EQ(labels.count, ReferenceComponentCount(volume));
            uint32_t total = 0;
            for (uint32_t c = 1; c <= labels.count; ++c) total += labels.voxelCounts[c];
            EXPECT_EQ(total, volume.CountSolid());
        }
}

// --- SplitComponents ---------------------------------------------------------------------------

TEST(SplitComponents, EmptyVolumeGivesNothing) {
    const SplitResult result = SplitComponents(VoxelVolume(glm::ivec3(4, 4, 4)));
    EXPECT_TRUE(result.pieces.empty());
    EXPECT_TRUE(result.loose.empty());
}

TEST(SplitComponents, PiecesAreCroppedSortedAndPlaced) {
    VoxelVolume volume(glm::ivec3(16, 8, 8));
    volume.FillBox({1, 1, 1}, {3, 3, 3}, 1);         // 8 voxels
    volume.FillBox({5, 0, 2}, {11, 4, 7}, 2);        // 120 voxels
    volume.FillBox({13, 5, 5}, {16, 8, 8}, 3);       // 27 voxels

    const SplitResult result = SplitComponents(volume);
    EXPECT_TRUE(result.loose.empty());
    ASSERT_EQ(result.pieces.size(), 3u);

    EXPECT_EQ(result.pieces[0].voxelCount, 120u);
    EXPECT_EQ(result.pieces[0].offset, glm::ivec3(5, 0, 2));
    EXPECT_EQ(result.pieces[0].volume.Size(), glm::ivec3(6, 4, 5));
    EXPECT_EQ(result.pieces[0].volume.Get(0, 0, 0), 2);

    EXPECT_EQ(result.pieces[1].voxelCount, 27u);
    EXPECT_EQ(result.pieces[1].offset, glm::ivec3(13, 5, 5));
    EXPECT_EQ(result.pieces[1].volume.Size(), glm::ivec3(3, 3, 3));

    EXPECT_EQ(result.pieces[2].voxelCount, 8u);
    EXPECT_EQ(result.pieces[2].offset, glm::ivec3(1, 1, 1));
    EXPECT_EQ(result.pieces[2].volume.Size(), glm::ivec3(2, 2, 2));

    for (const VolumePiece& piece : result.pieces) EXPECT_EQ(piece.volume.CountSolid(), piece.voxelCount);
}

TEST(SplitComponents, PieceContainsOnlyItsOwnComponent) {
    // A ring with a separate voxel in its hole: the voxel lies inside the ring's bounds.
    VoxelVolume volume(glm::ivec3(5, 5, 1));
    volume.FillBox({0, 0, 0}, {5, 5, 1}, 1);
    volume.FillBox({1, 1, 0}, {4, 4, 1}, 0);
    volume.Set(2, 2, 0, 9);

    const SplitResult result = SplitComponents(volume);
    ASSERT_EQ(result.pieces.size(), 2u);
    EXPECT_EQ(result.pieces[0].voxelCount, 16u);
    EXPECT_EQ(result.pieces[0].volume.Size(), glm::ivec3(5, 5, 1));
    EXPECT_EQ(result.pieces[0].volume.CountSolid(), 16u);
    EXPECT_EQ(result.pieces[0].volume.Get(2, 2, 0), 0);          // the foreign voxel was not copied
    EXPECT_EQ(result.pieces[1].voxelCount, 1u);
    EXPECT_EQ(result.pieces[1].offset, glm::ivec3(2, 2, 0));
    EXPECT_EQ(result.pieces[1].volume.Size(), glm::ivec3(1, 1, 1));
    EXPECT_EQ(result.pieces[1].volume.Get(0, 0, 0), 9);
}

TEST(SplitComponents, SmallComponentsBecomeLooseVoxels) {
    VoxelVolume volume(glm::ivec3(12, 4, 4));
    volume.FillBox({0, 0, 0}, {3, 3, 3}, 1);         // 27 voxels
    volume.Set(5, 1, 1, 4);                          // 2 voxels
    volume.Set(6, 1, 1, 5);
    volume.Set(10, 3, 2, 6);                         // 1 voxel

    const SplitResult all = SplitComponents(volume, 1);
    EXPECT_EQ(all.pieces.size(), 3u);
    EXPECT_TRUE(all.loose.empty());

    const SplitResult exact = SplitComponents(volume, 2);       // "fewer than minVoxels": 2 is still a piece
    ASSERT_EQ(exact.pieces.size(), 2u);
    EXPECT_EQ(exact.pieces[1].voxelCount, 2u);
    ASSERT_EQ(exact.loose.size(), 1u);
    EXPECT_EQ(exact.loose[0].position, glm::ivec3(10, 3, 2));
    EXPECT_EQ(exact.loose[0].material, 6);

    const SplitResult coarse = SplitComponents(volume, 3);
    ASSERT_EQ(coarse.pieces.size(), 1u);
    EXPECT_EQ(coarse.pieces[0].voxelCount, 27u);
    ASSERT_EQ(coarse.loose.size(), 3u);
    VoxelVolume looseOnly(volume.Size());
    for (const LooseVoxel& v : coarse.loose) {
        EXPECT_EQ(looseOnly.Get(v.position), 0) << "loose voxel reported twice";
        looseOnly.Set(v.position, v.material);
    }
    EXPECT_EQ(looseOnly.Get(5, 1, 1), 4);
    EXPECT_EQ(looseOnly.Get(6, 1, 1), 5);
    EXPECT_EQ(looseOnly.Get(10, 3, 2), 6);

    const SplitResult none = SplitComponents(volume, 1000);
    EXPECT_TRUE(none.pieces.empty());
    EXPECT_EQ(none.loose.size(), 30u);
}

TEST(SplitComponents, OffsetsReconstructTheOriginal) {
    uint32_t seed = 500;
    for (uint32_t density : {10u, 30u, 55u, 90u})
        for (uint32_t minVoxels : {0u, 1u, 4u}) {
            SCOPED_TRACE(testing::Message() << "seed " << seed << " minVoxels " << minVoxels);
            const VoxelVolume volume = RandomVolume(glm::ivec3(14, 11, 9), seed++, density);
            const SplitResult result = SplitComponents(volume, minVoxels);

            VoxelVolume rebuilt(volume.Size());
            uint32_t written = 0;
            size_t overlaps = 0;
            for (size_t i = 0; i < result.pieces.size(); ++i) {
                const VolumePiece& piece = result.pieces[i];
                if (i > 0) {
                    EXPECT_GE(result.pieces[i - 1].voxelCount, piece.voxelCount);
                }
                EXPECT_GE(piece.voxelCount, minVoxels);
                EXPECT_EQ(piece.volume.CountSolid(), piece.voxelCount);
                // Cropped tightly, and a single connected component.
                glm::ivec3 mn(0), mx(0);
                ASSERT_TRUE(piece.volume.SolidBounds(mn, mx));
                EXPECT_EQ(mn, glm::ivec3(0));
                EXPECT_EQ(mx, piece.volume.Size());
                EXPECT_EQ(LabelComponents(piece.volume).count, 1u);

                const glm::ivec3 size = piece.volume.Size();
                for (int z = 0; z < size.z; ++z)
                    for (int y = 0; y < size.y; ++y)
                        for (int x = 0; x < size.x; ++x) {
                            const uint8_t id = piece.volume.Get(x, y, z);
                            if (id == 0) continue;
                            const glm::ivec3 p = piece.offset + glm::ivec3(x, y, z);
                            if (!rebuilt.InBounds(p.x, p.y, p.z) || rebuilt.Get(p) != 0) ++overlaps;
                            rebuilt.Set(p, id);
                            ++written;
                        }
            }
            for (const LooseVoxel& v : result.loose) {
                if (!rebuilt.InBounds(v.position.x, v.position.y, v.position.z) || rebuilt.Get(v.position) != 0) ++overlaps;
                rebuilt.Set(v.position, v.material);
                ++written;
            }
            EXPECT_EQ(overlaps, 0u);
            EXPECT_EQ(written, volume.CountSolid());
            EXPECT_EQ(rebuilt.Data(), volume.Data());
        }
}

// --- ExtractFloatingIslands --------------------------------------------------------------------

TEST(FloatingIslands, BlobInTheAirIsExtractedAndRemoved) {
    Fixture f;
    World& world = *f.world;
    const glm::ivec3 blobMin(2, 50, 2), blobMax(5, 53, 5);
    world.FillBox(blobMin, blobMax, kRock);
    world.SetVoxel({3, 53, 3}, kSurface);                              // a bump on top: 28 voxels
    world.FillBox({10, kGround, 10}, {12, 52, 12}, kRock);             // a pillar standing on the ground

    const glm::ivec3 regionMin(-8, 32, -8), regionMax(24, 64, 24);
    const uint32_t solidBefore = CountWorldSolid(world, regionMin, regionMax);

    const std::vector<FloatingIsland> islands = ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 1000);
    ASSERT_EQ(islands.size(), 1u);
    const FloatingIsland& island = islands[0];
    EXPECT_EQ(island.origin, blobMin);
    EXPECT_EQ(island.voxelCount, 28u);
    EXPECT_EQ(island.volume.Size(), glm::ivec3(3, 4, 3));
    EXPECT_EQ(island.volume.CountSolid(), 28u);
    EXPECT_EQ(island.volume.Get(0, 0, 0), kRock);
    EXPECT_EQ(island.volume.Get(2, 2, 2), kRock);
    EXPECT_EQ(island.volume.Get(1, 3, 1), kSurface);
    EXPECT_EQ(island.volume.Get(0, 3, 0), 0);

    // Removed from the world; nothing else was touched.
    EXPECT_EQ(CountWorldSolid(world, blobMin, blobMax + glm::ivec3(0, 1, 0)), 0u);
    EXPECT_EQ(CountWorldSolid(world, regionMin, regionMax), solidBefore - 28u);
    EXPECT_EQ(CountWorldSolid(world, {10, kGround, 10}, {12, 52, 12}), 2u * 12u * 2u);
    EXPECT_EQ(world.GetVoxel({3, kGround - 1, 3}), kSurface);
    EXPECT_EQ(world.GetVoxel({3, kGround - 2, 3}), kRock);

    // A second search finds nothing more.
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 1000).empty());
}

TEST(FloatingIslands, PillarOnTheGroundIsAttached) {
    Fixture f;
    World& world = *f.world;
    world.FillBox({10, kGround, 10}, {12, 58, 12}, kRock);
    world.FillBox({6, 56, 10}, {12, 58, 12}, kRock);                   // an overhanging arm, still attached

    const glm::ivec3 regionMin(-8, 32, -8), regionMax(24, 64, 24);
    const uint32_t solidBefore = CountWorldSolid(world, regionMin, regionMax);
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 100000).empty());
    EXPECT_EQ(CountWorldSolid(world, regionMin, regionMax), solidBefore);

    // Cut the pillar: the upper part with the arm now floats.
    world.FillBox({10, 45, 10}, {12, 46, 12}, 0);
    const std::vector<FloatingIsland> islands = ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 100000);
    ASSERT_EQ(islands.size(), 1u);
    EXPECT_EQ(islands[0].origin, glm::ivec3(6, 46, 10));
    EXPECT_EQ(islands[0].volume.Size(), glm::ivec3(6, 12, 2));
    EXPECT_EQ(islands[0].voxelCount, 2u * 12u * 2u + 4u * 2u * 2u);
    EXPECT_EQ(islands[0].volume.CountSolid(), islands[0].voxelCount);
    EXPECT_EQ(CountWorldSolid(world, {6, 46, 10}, {12, 58, 12}), 0u);
    EXPECT_EQ(CountWorldSolid(world, {10, kGround, 10}, {12, 45, 12}), 2u * 5u * 2u);   // the stump stays
}

TEST(FloatingIslands, BlobLargerThanTheLimitStays) {
    Fixture f;
    World& world = *f.world;
    const glm::ivec3 blobMin(2, 50, 2), blobMax(5, 53, 5);             // 27 voxels
    world.FillBox(blobMin, blobMax, kRock);
    world.SetVoxel({-3, 48, -3}, kRock);                               // and a single floating voxel

    const glm::ivec3 regionMin(-8, 32, -8), regionMax(24, 64, 24);
    std::vector<FloatingIsland> islands = ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 26);
    ASSERT_EQ(islands.size(), 1u);                                     // only the small one qualifies
    EXPECT_EQ(islands[0].origin, glm::ivec3(-3, 48, -3));
    EXPECT_EQ(islands[0].voxelCount, 1u);
    EXPECT_EQ(islands[0].volume.Size(), glm::ivec3(1));
    EXPECT_EQ(world.GetVoxel({-3, 48, -3}), 0);
    EXPECT_EQ(CountWorldSolid(world, blobMin, blobMax), 27u);

    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 0).empty());
    EXPECT_EQ(CountWorldSolid(world, blobMin, blobMax), 27u);

    // The limit is inclusive.
    islands = ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 27);
    ASSERT_EQ(islands.size(), 1u);
    EXPECT_EQ(islands[0].voxelCount, 27u);
    EXPECT_EQ(CountWorldSolid(world, blobMin, blobMax), 0u);
}

TEST(FloatingIslands, CanReportWithoutRemoving) {
    Fixture f;
    World& world = *f.world;
    const glm::ivec3 blobMin(2, 50, 2), blobMax(5, 53, 5);
    world.FillBox(blobMin, blobMax, kRock);

    const glm::ivec3 regionMin(-8, 32, -8), regionMax(24, 64, 24);
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<FloatingIsland> islands = ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 1000, false);
        ASSERT_EQ(islands.size(), 1u);
        EXPECT_EQ(islands[0].origin, blobMin);
        EXPECT_EQ(islands[0].voxelCount, 27u);
        EXPECT_EQ(CountWorldSolid(world, blobMin, blobMax), 27u);
    }
}

TEST(FloatingIslands, ComponentTouchingTheRegionBoundaryIsAttached) {
    Fixture f;
    World& world = *f.world;
    world.FillBox({2, 50, 2}, {5, 53, 5}, kRock);

    // The blob floats, but each of these boxes cuts through it or ends exactly at its surface
    // layer, so nothing proves that it does not continue outside.
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {-8, 45, -8}, {4, 60, 24}, 1000).empty());    // cut in x
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {2, 45, -8}, {24, 60, 24}, 1000).empty());    // min face
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {-8, 45, -8}, {24, 53, 24}, 1000).empty());   // max face
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {-8, 50, -8}, {24, 60, 24}, 1000).empty());   // bottom face
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {2, 50, 2}, {5, 53, 5}, 1000).empty());       // exactly the blob
    EXPECT_EQ(CountWorldSolid(world, {2, 50, 2}, {5, 53, 5}), 27u);

    // One voxel of margin on every side is enough.
    const std::vector<FloatingIsland> islands = ExtractFloatingIslands(world, &f.materials, {1, 49, 1}, {6, 54, 6}, 1000);
    ASSERT_EQ(islands.size(), 1u);
    EXPECT_EQ(islands[0].voxelCount, 27u);
    EXPECT_EQ(CountWorldSolid(world, {2, 50, 2}, {5, 53, 5}), 0u);
}

TEST(FloatingIslands, IslandAcrossChunkBordersKeepsItsShape) {
    Fixture f;
    World& world = *f.world;
    // Straddles the chunk borders at x = 32, y = 64 and z = 32; negative coordinates too.
    world.FillBox({30, 62, 30}, {35, 67, 35}, kRock);
    world.FillBox({31, 63, 31}, {34, 66, 34}, 0);                      // hollow
    world.SetVoxel({32, 64, 32}, kSurface);                            // a separate voxel inside the shell
    world.FillBox({-34, 62, -34}, {-30, 66, -30}, kSurface);

    std::vector<FloatingIsland> islands = ExtractFloatingIslands(world, &f.materials, {20, 52, 20}, {44, 76, 44}, 100000);
    ASSERT_EQ(islands.size(), 2u);
    const size_t shellIndex = islands[0].voxelCount > 1u ? 0u : 1u;
    const FloatingIsland& shell = islands[shellIndex];
    const FloatingIsland& core = islands[1u - shellIndex];
    EXPECT_EQ(shell.origin, glm::ivec3(30, 62, 30));
    EXPECT_EQ(shell.volume.Size(), glm::ivec3(5, 5, 5));
    EXPECT_EQ(shell.voxelCount, 5u * 5u * 5u - 3u * 3u * 3u);
    EXPECT_EQ(shell.volume.CountSolid(), shell.voxelCount);
    EXPECT_EQ(shell.volume.Get(2, 2, 2), 0);                           // the inner voxel is not part of the shell
    EXPECT_EQ(core.origin, glm::ivec3(32, 64, 32));
    EXPECT_EQ(core.voxelCount, 1u);
    EXPECT_EQ(core.volume.Get(0, 0, 0), kSurface);
    EXPECT_EQ(CountWorldSolid(world, {20, 52, 20}, {44, 76, 44}), 0u);

    islands = ExtractFloatingIslands(world, &f.materials, {-40, 56, -40}, {-24, 72, -24}, 100000);
    ASSERT_EQ(islands.size(), 1u);
    EXPECT_EQ(islands[0].origin, glm::ivec3(-34, 62, -34));
    EXPECT_EQ(islands[0].voxelCount, 64u);
    EXPECT_EQ(islands[0].volume.Size(), glm::ivec3(4));
    EXPECT_EQ(CountWorldSolid(world, {-40, 56, -40}, {-24, 72, -24}), 0u);
}

TEST(FloatingIslands, IgnoredMaterialsNeitherSupportNorBelong) {
    Fixture f;
    World& world = *f.world;
    // A slab resting on a column of water, with a puddle on top.
    world.FillBox({2, kGround, 2}, {3, 50, 3}, kWater);
    world.FillBox({1, 50, 1}, {4, 52, 4}, kRock);                      // 18 voxels
    world.FillBox({1, 52, 1}, {4, 53, 4}, kWater);

    const glm::ivec3 regionMin(-8, 32, -8), regionMax(24, 64, 24);

    // Without a material library water is as good a support as anything else.
    EXPECT_TRUE(ExtractFloatingIslands(world, nullptr, regionMin, regionMax, 1000).empty());
    // With the flags switched off as well.
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 1000, true, MaterialFlag_None).empty());
    EXPECT_EQ(CountWorldSolid(world, {1, 50, 1}, {4, 52, 4}), 18u);

    const std::vector<FloatingIsland> islands = ExtractFloatingIslands(world, &f.materials, regionMin, regionMax, 1000);
    ASSERT_EQ(islands.size(), 1u);
    EXPECT_EQ(islands[0].origin, glm::ivec3(1, 50, 1));
    EXPECT_EQ(islands[0].volume.Size(), glm::ivec3(3, 2, 3));
    EXPECT_EQ(islands[0].voxelCount, 18u);
    for (uint8_t id : islands[0].volume.Data()) EXPECT_EQ(id, kRock);
    EXPECT_EQ(CountWorldSolid(world, {1, 50, 1}, {4, 52, 4}), 0u);
    // The water was left alone.
    EXPECT_EQ(CountWorldSolid(world, {2, kGround, 2}, {3, 50, 3}), 10u);
    EXPECT_EQ(CountWorldSolid(world, {1, 52, 1}, {4, 53, 4}), 9u);
    EXPECT_EQ(world.GetVoxel({2, 45, 2}), kWater);
}

TEST(FloatingIslands, UndecidableRegionsReturnNothing) {
    Fixture f;
    World& world = *f.world;
    const glm::ivec3 blobMin(2, 50, 2), blobMax(5, 53, 5);
    world.FillBox(blobMin, blobMax, kRock);

    // Empty and inverted regions.
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {0, 45, 0}, {0, 60, 10}, 1000).empty());
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {10, 60, 10}, {0, 45, 0}, 1000).empty());
    // More than 192 voxels on an axis.
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {-100, 45, -8}, {93, 60, 24}, 1000).empty());
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {-2000000000, 45, -8}, {2000000000, 60, 24}, 1000).empty());
    // Partly outside the loaded window (chunks x >= 4 are not loaded).
    ASSERT_FALSE(world.IsChunkLoaded({4, 1, 0}));
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {-8, 45, -8}, {130, 60, 24}, 1000).empty());
    // Entirely outside it.
    EXPECT_TRUE(ExtractFloatingIslands(world, &f.materials, {100000, 45, 0}, {100032, 60, 32}, 1000).empty());

    EXPECT_EQ(CountWorldSolid(world, blobMin, blobMax), 27u);

    // A region of exactly 192 voxels inside the window is accepted.
    const std::vector<FloatingIsland> islands = ExtractFloatingIslands(world, &f.materials, {-96, 45, -96}, {96, 60, 96}, 1000);
    ASSERT_EQ(islands.size(), 1u);
    EXPECT_EQ(islands[0].voxelCount, 27u);
    EXPECT_EQ(CountWorldSolid(world, blobMin, blobMax), 0u);
}
