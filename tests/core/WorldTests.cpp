// Streaming world: loading, queries, editing, window movement, persistence, generation.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
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

    struct Fixture {
        explicit Fixture(std::filesystem::path saveDir = {}, glm::ivec3 window = {8, 8, 8}, uint32_t capacity = 512, float voxelSize = 0.5f)
            : store(capacity), jobs(3) {
            WorldConfig config;
            config.voxelSize = voxelSize;
            config.windowSize = window;
            config.saveDirectory = std::move(saveDir);
            Material hard;
            hard.hardness = 10.0f;
            materials.Set(kWorldPalette, kSurface, Material{}, "surface");
            materials.Set(kWorldPalette, kRock, hard, "rock");
            world = std::make_unique<World>(config, store, jobs, CreateFlatGenerator(kGround, kSurface, kRock), &materials);
        }
        ChunkStore store;
        JobSystem jobs;
        MaterialLibrary materials;
        std::unique_ptr<World> world;
    };

    /// Independent reference: walk the ray in small steps through World::GetVoxel.
    bool ReferenceMarch(const World& world, glm::dvec3 origin, glm::dvec3 dir, double maxDist, double& outT) {
        const double step = world.VoxelSize() * 0.02;
        for (double t = 0.0; t <= maxDist; t += step) {
            if (world.IsSolidAt(origin + dir * t)) {
                outT = t;
                return true;
            }
        }
        return false;
    }
}

TEST(World, LoadsWindowAndClassifiesChunks) {
    Fixture f;
    f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));     // y = 40 voxels: the window straddles the surface
    const WorldStats stats = f.world->Stats();
    EXPECT_EQ(stats.pendingChunks, 0u);
    EXPECT_EQ(stats.jobsInFlight, 0u);
    // Ground top is at voxel 40 = chunk 1 (voxels 32..63): exactly one dense layer.
    EXPECT_EQ(stats.denseChunks, 8u * 8u);
    EXPECT_GT(stats.uniformChunks, 0u);     // solid rock below needs no storage
    EXPECT_GT(stats.emptyChunks, 0u);       // air above neither
    EXPECT_EQ(f.store.UsedCount(), stats.denseChunks);
    // The column pass decides every row but the surface one: one generated chunk per column.
    EXPECT_EQ(stats.generatedChunks, 8u * 8u);
    EXPECT_EQ(stats.generatedDense, stats.denseChunks);
    EXPECT_GT(stats.generateMs, 0.0);

    EXPECT_EQ(f.world->GetVoxel({0, kGround - 1, 0}), kSurface);
    EXPECT_EQ(f.world->GetVoxel({0, kGround - 2, 0}), kRock);
    EXPECT_EQ(f.world->GetVoxel({0, kGround, 0}), 0);
    EXPECT_EQ(f.world->GetVoxel({5, 10, -7}), kRock);              // inside a uniform chunk
    EXPECT_EQ(f.world->GetVoxel({100000, 0, 0}), 0);               // outside the window: reported empty
    EXPECT_FALSE(f.world->IsChunkLoaded({1000, 0, 0}));
}

TEST(World, GridEncodingMatchesShaderContract) {
    Fixture f;
    f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    const glm::ivec3 size = f.world->WindowSize();
    const glm::ivec3 min = f.world->WindowMinChunk();
    const auto grid = f.world->Grid();
    ASSERT_EQ(grid.size(), static_cast<size_t>(size.x * size.y * size.z));
    for (int y = min.y; y < min.y + size.y; ++y)
        for (int z = min.z; z < min.z + size.z; ++z)
            for (int x = min.x; x < min.x + size.x; ++x) {
                const glm::ivec3 w = glm::ivec3(x, y, z) & (size - glm::ivec3(1));
                const uint32_t entry = grid[static_cast<size_t>((w.x + w.z * size.x) * size.y + w.y)];
                switch (f.world->StateOf({x, y, z})) {
                    case CellState::Empty:   EXPECT_EQ(entry, 0u); break;
                    case CellState::Uniform: EXPECT_EQ(entry, 0x80000000u | kRock); break;
                    case CellState::Dense:
                        ASSERT_NE(entry, 0u);
                        ASSERT_EQ(entry & 0x80000000u, 0u);
                        EXPECT_TRUE(f.store.IsAllocated(entry - 1u));
                        break;
                    default: ADD_FAILURE() << "chunk not loaded";
                }
            }
}

TEST(World, RaycastHitsGroundAndMatchesReference) {
    Fixture f;
    f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    const double groundY = kGround * 0.5;

    WorldRayHit hit = f.world->Raycast(glm::dvec3(1.3, groundY + 10.0, -2.1), glm::vec3(0, -1, 0), 100.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.distance, 10.0f, 1e-3f);
    EXPECT_EQ(hit.normal, glm::vec3(0, 1, 0));
    EXPECT_EQ(hit.voxel.y, kGround - 1);
    EXPECT_EQ(hit.material, kSurface);
    EXPECT_NEAR(hit.position.y, groundY, 1e-3);

    // Dig some holes and build some towers so rays have something interesting to hit, including
    // geometry in chunks that were empty or uniform before.
    f.world->CarveSphere(glm::vec3(4.5f, kGround - 2.0f, 6.5f), 6.0f, 100.0f);
    f.world->FillBox({-12, kGround, -3}, {-9, kGround + 30, 0}, kRock);
    f.world->FillSphere(glm::vec3(20.5f, kGround + 20.5f, -20.5f), 5.0f, kSurface);

    Rng rng(11);
    int hits = 0, bad = 0, total = 0;
    for (int i = 0; i < 1500; ++i) {
        const glm::dvec3 origin(rng.Range(-25.0f, 25.0f), groundY + rng.Range(0.5f, 20.0f), rng.Range(-25.0f, 25.0f));
        glm::vec3 dir(rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 0.3f), rng.Range(-1.0f, 1.0f));
        if (glm::length(dir) < 0.1f) continue;
        dir = glm::normalize(dir);
        const float maxDist = 25.0f;
        const WorldRayHit got = f.world->Raycast(origin, dir, maxDist);
        double refT = 0.0;
        const bool refHit = ReferenceMarch(*f.world, origin, glm::dvec3(dir), maxDist, refT);
        ++total;
        // The reference marches in steps of 1 cm, so it is late by at most one step.
        const bool same = got.hit == refHit && (!got.hit || std::abs(refT - got.distance) < 0.05);
        if (!same) ++bad;
        if (got.hit) {
            ++hits;
            EXPECT_NE(f.world->GetVoxel(got.voxel), 0) << "reported voxel must be solid";
            EXPECT_EQ(f.world->GetVoxel(got.voxel), got.material);
        }
    }
    EXPECT_GT(hits, 500);
    EXPECT_LE(bad, total / 200) << bad << " of " << total << " rays disagree with the reference march";
}

TEST(World, EditsMaterializeAndReportChanges) {
    Fixture f;
    f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    (void)f.world->TakeChangedChunks();
    const uint32_t denseBefore = f.world->Stats().denseChunks;

    // Edit inside an empty chunk (air): the chunk becomes dense.
    const glm::ivec3 inAir(3, kGround + 40, 3);
    ASSERT_EQ(f.world->StateOf(VoxelToChunk(inAir)), CellState::Empty);
    EXPECT_TRUE(f.world->SetVoxel(inAir, kSurface));
    EXPECT_EQ(f.world->GetVoxel(inAir), kSurface);
    EXPECT_EQ(f.world->StateOf(VoxelToChunk(inAir)), CellState::Dense);

    // Edit inside a uniform chunk (solid rock): it becomes dense and keeps its other voxels.
    const glm::ivec3 inRock(3, 5, 3);
    ASSERT_EQ(f.world->StateOf(VoxelToChunk(inRock)), CellState::Uniform);
    EXPECT_TRUE(f.world->SetVoxel(inRock, 0));
    EXPECT_EQ(f.world->GetVoxel(inRock), 0);
    EXPECT_EQ(f.world->GetVoxel(inRock + glm::ivec3(1, 0, 0)), kRock);
    EXPECT_EQ(f.world->Stats().denseChunks, denseBefore + 2);

    const auto changed = f.world->TakeChangedChunks();
    EXPECT_EQ(changed.size(), 2u);
    EXPECT_TRUE(f.world->TakeChangedChunks().empty());

    // Writing the value a voxel already has is not a change.
    EXPECT_TRUE(f.world->SetVoxel(inAir, kSurface));
    EXPECT_TRUE(f.world->TakeChangedChunks().empty());
    // Outside the loaded window edits are rejected, not silently lost.
    EXPECT_FALSE(f.world->SetVoxel({100000, 0, 0}, kSurface));

    std::vector<VoxelEdit> edits;
    const uint32_t filled = f.world->FillBox({0, kGround, 0}, {4, kGround + 3, 5}, kRock, &edits);
    EXPECT_EQ(filled, 4u * 3u * 5u);
    EXPECT_EQ(edits.size(), filled);
    for (const VoxelEdit& e : edits) {
        EXPECT_EQ(e.oldId, 0);
        EXPECT_EQ(e.newId, kRock);
    }
}

TEST(World, CarveRespectsHardness) {
    Fixture f;
    f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    std::vector<VoxelEdit> removed;
    // Weak blast: only the soft surface layer (hardness 1) goes, rock (hardness 10) stays.
    const uint32_t weak = f.world->CarveSphere(glm::vec3(0.5f, kGround - 0.5f, 0.5f), 4.0f, 2.0f, &removed);
    EXPECT_GT(weak, 0u);
    for (const VoxelEdit& e : removed) EXPECT_EQ(e.oldId, kSurface);
    EXPECT_EQ(f.world->GetVoxel({0, kGround - 2, 0}), kRock);
    // Strong blast removes rock too.
    const uint32_t strong = f.world->CarveSphere(glm::vec3(0.5f, kGround - 0.5f, 0.5f), 4.0f, 50.0f);
    EXPECT_GT(strong, weak);
    EXPECT_EQ(f.world->GetVoxel({0, kGround - 2, 0}), 0);
}

TEST(World, WindowMovementKeepsEditsAndFreesMemory) {
    Fixture f;
    const glm::dvec3 home(0.0, 20.0, 0.0);
    f.world->LoadBlocking(home);
    const glm::ivec3 marker(2, kGround + 5, -3);
    ASSERT_TRUE(f.world->SetVoxel(marker, kRock));
    const uint32_t usedAtHome = f.store.UsedCount();

    // Walk far away: the home chunks leave the window and their slots are released.
    f.world->LoadBlocking(home + glm::dvec3(4000.0, 0.0, 0.0));
    EXPECT_EQ(f.world->GetVoxel(marker), 0);                    // not loaded any more
    EXPECT_EQ(f.world->Stats().pendingChunks, 0u);
    EXPECT_LE(f.store.UsedCount(), usedAtHome);                 // no leak: old slots were freed
    EXPECT_EQ(f.world->GetVoxel(f.world->WorldToVoxel(home + glm::dvec3(4000.0, -0.25, 0.0))), kSurface);

    // Come back: the edit was saved when the chunk was unloaded and is restored.
    f.world->LoadBlocking(home);
    EXPECT_EQ(f.world->GetVoxel(marker), kRock);
    EXPECT_EQ(f.store.UsedCount(), usedAtHome);
}

TEST(World, SmallStepsOnlyReloadTheNewSlab) {
    Fixture f;
    f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    const uint64_t revision = f.world->GridRevision();
    const glm::ivec3 before = f.world->WindowMinChunk();
    // Move by exactly one chunk (32 voxels * 0.5 m).
    f.world->Update(glm::dvec3(16.0, 20.0, 0.0));
    EXPECT_EQ(f.world->WindowMinChunk(), before + glm::ivec3(1, 0, 0));
    EXPECT_NE(f.world->GridRevision(), revision);
    // Only one slab of 8 x 8 chunks has to be loaded, everything else stays in place.
    const WorldStats stats = f.world->Stats();
    EXPECT_EQ(stats.pendingChunks, 8u * 8u);
    f.world->LoadBlocking(glm::dvec3(16.0, 20.0, 0.0));
    EXPECT_EQ(f.world->Stats().pendingChunks, 0u);
}

TEST(World, PersistsEditsToDisk) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vortex_world_test";
    std::filesystem::remove_all(dir);
    const glm::ivec3 marker(-5, kGround + 2, 9);
    {
        Fixture f(dir);
        f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
        ASSERT_TRUE(f.world->SetVoxel(marker, kRock));
        ASSERT_TRUE(f.world->SetVoxel({0, kGround - 1, 0}, 0));
        // Destroying the world saves it.
    }
    ASSERT_TRUE(std::filesystem::exists(dir));
    {
        Fixture f(dir);
        f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
        EXPECT_EQ(f.world->GetVoxel(marker), kRock);
        EXPECT_EQ(f.world->GetVoxel({0, kGround - 1, 0}), 0);
        EXPECT_EQ(f.world->GetVoxel({1, kGround - 1, 0}), kSurface);
        // The two edited chunks came from the save: they are not counted as generated.
        EXPECT_EQ(f.world->Stats().generatedChunks, 8u * 8u - 2u);
    }
    std::filesystem::remove_all(dir);
}

// A save holds voxel ids of one voxel size; at another size they would be a different world
// (view-distance.md stage 3). The region file records the size and a world of another size
// refuses it whole: nothing of it is shown, and nothing overwrites it.
TEST(World, SavesOfAnotherVoxelSizeAreRefusedAndKept) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vortex_world_size_test";
    std::filesystem::remove_all(dir);
    const glm::ivec3 marker(-5, kGround + 2, 9);
    {
        Fixture f(dir, {8, 8, 8}, 512, 0.5f);
        f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
        ASSERT_TRUE(f.world->SetVoxel(marker, kRock));
    }
    const std::filesystem::path file = dir / "r.-1.0.0.vxr";
    ASSERT_TRUE(std::filesystem::exists(file));
    const auto bytesOf = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::vector<char> saved = bytesOf(file);
    ASSERT_GE(saved.size(), 12u);
    EXPECT_EQ(std::string(saved.data(), 4), "VXR2");
    uint32_t bits = 0;
    std::memcpy(&bits, saved.data() + 4, 4);
    EXPECT_EQ(bits, std::bit_cast<uint32_t>(0.5f));
    {
        // Same voxel coordinates, a quarter-metre world: the edit is not there, and the world's own
        // edit in that region is not written over the file when it is destroyed (it saves).
        Fixture f(dir, {8, 8, 8}, 512, 0.25f);
        f.world->LoadBlocking(glm::dvec3(0.0, 10.0, 0.0));
        EXPECT_EQ(f.world->GetVoxel(marker), 0);
        EXPECT_EQ(f.world->Stats().refusedRegions, 1u);
        ASSERT_TRUE(f.world->SetVoxel(marker + glm::ivec3(1, 0, 0), kRock));
        EXPECT_EQ(f.world->GetVoxel(marker + glm::ivec3(1, 0, 0)), kRock);     // kept in memory
        // A save says that the edit was not written.
        const WorldSaveResult saved = f.world->Save();
        EXPECT_EQ(saved.unwrittenRegions, 1u);
        EXPECT_EQ(f.world->Stats().unwrittenRegions, 1u);
    }
    EXPECT_EQ(bytesOf(file), saved);
    {
        Fixture f(dir, {8, 8, 8}, 512, 0.5f);
        f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
        EXPECT_EQ(f.world->GetVoxel(marker), kRock);
        EXPECT_EQ(f.world->GetVoxel(marker + glm::ivec3(1, 0, 0)), 0);
        EXPECT_EQ(f.world->Stats().refusedRegions, 0u);
    }
    std::filesystem::remove_all(dir);
}

// Region files written before the voxel size was recorded ("VXR1") were all 0.1 m worlds.
TEST(Region, OldFilesWithoutAVoxelSizeCountAsTenthOfAMetre) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vortex_region_legacy_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::vector<uint8_t> chunk = EncodeChunk(ChunkKind::Uniform, 7, nullptr);
    {
        std::ofstream out(dir / "r.0.0.0.vxr", std::ios::binary);
        const uint32_t count = 1, size = static_cast<uint32_t>(chunk.size());
        const uint16_t local = 1 + (2 << 4) + (3 << 8);
        out.write("VXR1", 4);
        out.write(reinterpret_cast<const char*>(&count), 4);
        out.write(reinterpret_cast<const char*>(&local), 2);
        out.write(reinterpret_cast<const char*>(&size), 4);
        out.write(reinterpret_cast<const char*>(chunk.data()), size);
    }
    EXPECT_EQ(RegionStore::kLegacyVoxelSize, 0.1f);
    {
        RegionStore tenth(dir, 0.1f);
        ASSERT_TRUE(tenth.Load({1, 2, 3}).has_value());
        EXPECT_EQ(*tenth.Load({1, 2, 3}), chunk);
        EXPECT_EQ(tenth.RefusedRegionCount(), 0u);
    }
    {
        RegionStore eighth(dir, 0.125f);
        EXPECT_FALSE(eighth.Contains({1, 2, 3}));
        EXPECT_FALSE(eighth.Load({1, 2, 3}).has_value());
        EXPECT_EQ(eighth.RefusedRegionCount(), 1u);
        eighth.Save({1, 2, 3}, EncodeChunk(ChunkKind::Uniform, 9, nullptr));
        EXPECT_EQ(eighth.Flush(), 0u);                       // the old file is never overwritten
        // A region with no file of another size is written as usual, with the size in its header.
        eighth.Save({20, 0, 0}, chunk);
        EXPECT_EQ(eighth.Flush(), 1u);
    }
    {
        RegionStore tenth(dir, 0.1f);
        ASSERT_TRUE(tenth.Load({1, 2, 3}).has_value());
        EXPECT_EQ(*tenth.Load({1, 2, 3}), chunk);
        EXPECT_FALSE(tenth.Load({20, 0, 0}).has_value());    // written at 0.125 m: refused here
        EXPECT_EQ(tenth.RefusedRegionCount(), 1u);
        RegionStore eighth(dir, 0.125f);
        ASSERT_TRUE(eighth.Load({20, 0, 0}).has_value());
        EXPECT_EQ(*eighth.Load({20, 0, 0}), chunk);
    }
    std::filesystem::remove_all(dir);
}

// A file this build cannot read - a newer format, a header cut short - may hold edits: it is
// refused like a file of another voxel size and never overwritten. Only an empty file is replaced.
TEST(Region, UnreadableFilesAreRefusedAndNeverOverwritten) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vortex_region_unreadable_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto write = [&](const char* name, const std::string& bytes) {
        std::ofstream out(dir / name, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    const auto bytesOf = [&](const char* name) {
        std::ifstream in(dir / name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::string newer = std::string("VXR3") + std::string(40, '\x5a');
    const std::string cut = std::string("VXR2") + std::string(2, '\0');
    const std::string shortMagic = "VX";
    write("r.0.0.0.vxr", newer);
    write("r.1.0.0.vxr", cut);
    write("r.2.0.0.vxr", shortMagic);
    write("r.3.0.0.vxr", "");
    const std::vector<uint8_t> chunk = EncodeChunk(ChunkKind::Uniform, 7, nullptr);
    {
        RegionStore store(dir, 0.125f);
        for (int r = 0; r < 4; ++r) {
            const glm::ivec3 c(r * 16 + 1, 2, 3);
            EXPECT_FALSE(store.Load(c).has_value()) << r;
            store.Save(c, chunk);
        }
        EXPECT_EQ(store.RefusedRegionCount(), 3u);
        EXPECT_EQ(store.UnwrittenRegionCount(), 3u);
        EXPECT_EQ(store.Flush(), 1u);                          // the empty file only
        EXPECT_EQ(store.UnwrittenRegionCount(), 3u);
    }
    EXPECT_EQ(bytesOf("r.0.0.0.vxr"), newer);
    EXPECT_EQ(bytesOf("r.1.0.0.vxr"), cut);
    EXPECT_EQ(bytesOf("r.2.0.0.vxr"), shortMagic);
    {
        RegionStore store(dir, 0.125f);
        ASSERT_TRUE(store.Load({3 * 16 + 1, 2, 3}).has_value());
        EXPECT_EQ(*store.Load({3 * 16 + 1, 2, 3}), chunk);
        EXPECT_EQ(store.RefusedRegionCount(), 0u);
    }
    std::filesystem::remove_all(dir);
}

TEST(World, StoreExhaustionDegradesGracefully) {
    // Far too few slots for the window: the world must stay usable (missing terrain, no crash).
    Fixture f({}, {8, 8, 8}, 10);
    f.world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
    EXPECT_EQ(f.world->Stats().pendingChunks, 0u);
    EXPECT_EQ(f.store.UsedCount(), 10u);
    EXPECT_FALSE(f.world->SetVoxel({3, kGround + 100, 3}, kRock) && f.world->GetVoxel({3, kGround + 100, 3}) != kRock);
}

TEST(Region, ChunkCodecRoundTrip) {
    Rng rng(3);
    auto chunk = std::make_unique<Chunk>();
    for (int i = 0; i < 5000; ++i)
        chunk->Set(static_cast<int>(rng.NextBounded(32)), static_cast<int>(rng.NextBounded(32)), static_cast<int>(rng.NextBounded(32)),
                   static_cast<uint8_t>(rng.NextBounded(6)));
    const std::vector<uint8_t> bytes = EncodeChunk(ChunkKind::Dense, 0, chunk.get());
    EXPECT_LT(bytes.size(), static_cast<size_t>(kChunkVolume));

    auto decoded = std::make_unique<Chunk>();
    ChunkKind kind = ChunkKind::Empty;
    uint8_t uniform = 0;
    ASSERT_TRUE(DecodeChunk(bytes, kind, uniform, *decoded));
    EXPECT_EQ(kind, ChunkKind::Dense);
    for (int i = 0; i < kChunkVolume; ++i) ASSERT_EQ(decoded->ids[i], chunk->ids[i]);
    EXPECT_EQ(decoded->occ.cellMask, chunk->occ.cellMask);
    EXPECT_EQ(decoded->CountSolid(), chunk->CountSolid());

    // Uniform and empty chunks are a couple of bytes.
    EXPECT_EQ(EncodeChunk(ChunkKind::Uniform, 7, nullptr).size(), 2u);
    EXPECT_EQ(EncodeChunk(ChunkKind::Empty, 0, nullptr).size(), 1u);
    ASSERT_TRUE(DecodeChunk(EncodeChunk(ChunkKind::Uniform, 7, nullptr), kind, uniform, *decoded));
    EXPECT_EQ(kind, ChunkKind::Uniform);
    EXPECT_EQ(uniform, 7);

    // Corrupt input is rejected instead of producing garbage.
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
    EXPECT_FALSE(DecodeChunk(truncated, kind, uniform, *decoded));
    EXPECT_FALSE(DecodeChunk(std::vector<uint8_t>{9}, kind, uniform, *decoded));
    EXPECT_FALSE(DecodeChunk(std::vector<uint8_t>{}, kind, uniform, *decoded));
}

TEST(Terrain, GenerationIsDeterministicAndSeamless) {
    MaterialLibrary materials;
    TerrainSettings settings;
    settings.materials = RegisterDefaultWorldMaterials(materials);
    settings.voxelSize = 0.25f;
    settings.seed = 77;
    const auto a = CreateTerrainGenerator(settings);
    const auto b = CreateTerrainGenerator(settings);

    auto c1 = std::make_unique<Chunk>();
    auto c2 = std::make_unique<Chunk>();
    int dense = 0;
    for (int cz = -2; cz <= 2; ++cz)
        for (int cy = -3; cy <= 4; ++cy)
            for (int cx = -2; cx <= 2; ++cx) {
                c1->Clear();
                c2->Clear();
                const ChunkContent k1 = a->Generate({cx, cy, cz}, *c1);
                const ChunkContent k2 = b->Generate({cx, cy, cz}, *c2);
                ASSERT_EQ(k1.kind, k2.kind);
                ASSERT_EQ(k1.uniformId, k2.uniformId);
                if (k1.kind != ChunkKind::Dense) continue;
                ++dense;
                for (int i = 0; i < kChunkVolume; ++i) ASSERT_EQ(c1->ids[i], c2->ids[i]) << "chunk " << cx << "," << cy << "," << cz;
            }
    EXPECT_GT(dense, 10);

    // A different seed gives a different world.
    settings.seed = 78;
    const auto other = CreateTerrainGenerator(settings);
    int differing = 0;
    for (int cx = -2; cx <= 2; ++cx) {
        c1->Clear();
        c2->Clear();
        a->Generate({cx, 0, 0}, *c1);
        other->Generate({cx, 0, 0}, *c2);
        for (int i = 0; i < kChunkVolume; ++i) differing += c1->ids[i] != c2->ids[i];
    }
    EXPECT_GT(differing, 1000);
}

TEST(Terrain, StreamsThroughTheWorld) {
    ChunkStore store(4096);
    JobSystem jobs(4);
    MaterialLibrary materials;
    TerrainSettings settings;
    settings.materials = RegisterDefaultWorldMaterials(materials);
    settings.voxelSize = 0.25f;
    WorldConfig config;
    config.voxelSize = settings.voxelSize;
    config.windowSize = {16, 8, 16};
    World world(config, store, jobs, CreateTerrainGenerator(settings), &materials);
    world.LoadBlocking(glm::dvec3(0.0, 8.0, 0.0));

    const WorldStats stats = world.Stats();
    EXPECT_EQ(stats.pendingChunks, 0u);
    EXPECT_GT(stats.denseChunks, 50u);
    EXPECT_GT(stats.emptyChunks, 0u);

    // There is ground somewhere below the sky everywhere in the window.
    int columnsWithGround = 0;
    for (int x = -20; x <= 20; x += 5)
        for (int z = -20; z <= 20; z += 5) {
            const WorldRayHit hit = world.Raycast(glm::dvec3(x, 60.0, z), glm::vec3(0, -1, 0), 200.0f);
            columnsWithGround += hit.hit;
        }
    EXPECT_GT(columnsWithGround, 60);
}

TEST(World, WindowSizeNeedNotBePowerOfTwo) {
    Fixture f({}, {6, 5, 12}, 512);
    f.world->LoadBlocking(glm::dvec3(-37.0, 20.0, 91.0));     // negative coordinates exercise the wrap-around
    EXPECT_EQ(f.world->Stats().pendingChunks, 0u);
    EXPECT_EQ(f.world->Stats().denseChunks, 6u * 12u);
    Rng rng(21);
    int hits = 0;
    for (int i = 0; i < 400; ++i) {
        const glm::dvec3 origin(-37.0 + rng.Range(-20.0f, 20.0f), 20.0 + rng.Range(1.0f, 15.0f), 91.0 + rng.Range(-40.0f, 40.0f));
        const glm::vec3 dir = glm::normalize(glm::vec3(rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, -0.2f), rng.Range(-1.0f, 1.0f)));
        const WorldRayHit hit = f.world->Raycast(origin, dir, 30.0f);
        double refT = 0.0;
        const bool ref = ReferenceMarch(*f.world, origin, glm::dvec3(dir), 30.0, refT);
        ASSERT_EQ(hit.hit, ref);
        if (hit.hit) {
            ++hits;
            EXPECT_NEAR(hit.distance, refT, 0.05);
        }
    }
    EXPECT_GT(hits, 200);
    // Moving the window keeps everything consistent.
    f.world->LoadBlocking(glm::dvec3(-37.0 + 16.0 * 7, 20.0, 91.0));
    EXPECT_EQ(f.world->Stats().denseChunks, 6u * 12u);
}
