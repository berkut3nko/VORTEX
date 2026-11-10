#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

import vortex.core;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    /// Recomputes the occupancy pyramid from scratch and compares it with the incrementally maintained one.
    void ExpectPyramidConsistent(const Chunk& chunk) {
        auto reference = std::make_unique<Chunk>(chunk);
        reference->RebuildOccupancy();
        EXPECT_EQ(reference->occ.cellMask, chunk.occ.cellMask);
        for (int i = 0; i < 8; ++i) EXPECT_EQ(reference->occ.blockMask[i], chunk.occ.blockMask[i]) << "block word " << i;
        for (int i = 0; i < kBlocksPerChunk; ++i) ASSERT_EQ(reference->occ.voxelMask[i], chunk.occ.voxelMask[i]) << "block " << i;
    }
}

TEST(Chunk, LayoutMatchesGpuContract) {
    EXPECT_EQ(sizeof(ChunkOccupancy), 4168u);
    EXPECT_EQ(kOccupancyWords, 1042u);
    EXPECT_EQ(sizeof(Chunk), 4168u + 32768u);
    // Voxel index = block * 64 + local, as assumed by VX_VOXEL_ID in the shaders.
    EXPECT_EQ(Chunk::VoxelIndex(0, 0, 0), 0u);
    EXPECT_EQ(Chunk::VoxelIndex(3, 3, 3), 63u);
    EXPECT_EQ(Chunk::VoxelIndex(4, 0, 0), 64u);
    EXPECT_EQ(Chunk::VoxelIndex(31, 31, 31), 32767u);
}

TEST(Chunk, SetGetRoundTrip) {
    auto chunk = std::make_unique<Chunk>();
    Rng rng(42);
    std::vector<uint8_t> reference(kChunkVolume, 0);
    for (int i = 0; i < 20000; ++i) {
        const int x = static_cast<int>(rng.NextBounded(32)), y = static_cast<int>(rng.NextBounded(32)), z = static_cast<int>(rng.NextBounded(32));
        const uint8_t id = static_cast<uint8_t>(rng.NextBounded(4));   // plenty of removals
        chunk->Set(x, y, z, id);
        reference[static_cast<size_t>(x + 32 * (y + 32 * z))] = id;
    }
    uint32_t solid = 0;
    for (int z = 0; z < 32; ++z)
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const uint8_t expected = reference[static_cast<size_t>(x + 32 * (y + 32 * z))];
                ASSERT_EQ(chunk->Get(x, y, z), expected);
                ASSERT_EQ(chunk->IsSolid(x, y, z), expected != 0);
                solid += expected != 0;
            }
    EXPECT_EQ(chunk->CountSolid(), solid);
    ExpectPyramidConsistent(*chunk);
}

TEST(Chunk, RemovingLastVoxelClearsPyramid) {
    auto chunk = std::make_unique<Chunk>();
    EXPECT_TRUE(chunk->IsEmpty());
    EXPECT_TRUE(chunk->Set(17, 5, 30, 7));
    EXPECT_FALSE(chunk->Set(17, 5, 30, 7));        // unchanged
    EXPECT_FALSE(chunk->IsEmpty());
    EXPECT_TRUE(chunk->Set(17, 5, 30, 0));
    EXPECT_TRUE(chunk->IsEmpty());
    EXPECT_EQ(chunk->occ.cellMask, 0u);
    for (uint64_t w : chunk->occ.blockMask) EXPECT_EQ(w, 0u);
}

TEST(Chunk, OccupancyWordsMatchBits) {
    auto chunk = std::make_unique<Chunk>();
    chunk->Set(9, 14, 22, 3);
    const uint32_t block = Chunk::BlockIndex(9, 14, 22);
    const uint32_t local = Chunk::LocalIndex(9, 14, 22);
    // The GPU reads the pyramid as uint32 words: cell mask at 0, block mask at 2, voxel masks at 18.
    const uint32_t cell = (9 >> 3) + (14 >> 3) * 4 + (22 >> 3) * 16;
    EXPECT_NE((chunk->OccupancyWord(cell >> 5) >> (cell & 31u)) & 1u, 0u);
    EXPECT_NE((chunk->OccupancyWord(2u + (block >> 5)) >> (block & 31u)) & 1u, 0u);
    EXPECT_NE((chunk->OccupancyWord(18u + block * 2u + (local >> 5)) >> (local & 31u)) & 1u, 0u);
}

TEST(Chunk, UniformDetection) {
    auto chunk = std::make_unique<Chunk>();
    EXPECT_EQ(chunk->UniformId(), 0);
    chunk->Fill(9);
    EXPECT_EQ(chunk->UniformId(), 9);
    EXPECT_EQ(chunk->CountSolid(), static_cast<uint32_t>(kChunkVolume));
    chunk->Set(1, 2, 3, 0);
    EXPECT_EQ(chunk->UniformId(), -1);
}

TEST(Chunk, CoordinateHelpersFloorCorrectly) {
    EXPECT_EQ(VoxelToChunk(glm::ivec3(0, 31, 32)), glm::ivec3(0, 0, 1));
    EXPECT_EQ(VoxelToChunk(glm::ivec3(-1, -32, -33)), glm::ivec3(-1, -1, -2));
    EXPECT_EQ(VoxelToLocal(glm::ivec3(-1, -32, 33)), glm::ivec3(31, 0, 1));
    const glm::ivec3 v(-70, 5, 1000);
    EXPECT_EQ(ChunkToVoxel(VoxelToChunk(v)) + VoxelToLocal(v), v);
}

TEST(ChunkStore, AllocFreeAndDirtyTracking) {
    ChunkStore store(4);
    EXPECT_EQ(store.UsedCount(), 0u);
    const uint32_t a = store.Alloc();
    const uint32_t b = store.Alloc();
    EXPECT_EQ(a, 0u);            // low slots first keeps GPU buffers compact
    EXPECT_EQ(b, 1u);
    EXPECT_EQ(store.HighWaterMark(), 2u);
    auto dirty = store.TakeDirty();
    EXPECT_EQ(dirty.size(), 2u);
    EXPECT_FALSE(store.HasDirty());

    store.Get(a).Set(1, 1, 1, 5);
    store.MarkDirty(a);
    store.MarkDirty(a);          // deduplicated
    EXPECT_EQ(store.TakeDirty().size(), 1u);

    store.Free(a);
    EXPECT_FALSE(store.IsAllocated(a));
    EXPECT_EQ(store.Alloc(), a); // slot reuse
    EXPECT_TRUE(store.Get(a).IsEmpty());

    store.Alloc();
    store.Alloc();
    EXPECT_EQ(store.Alloc(), ChunkStore::kInvalidSlot);   // exhausted, reported instead of crashing
}
