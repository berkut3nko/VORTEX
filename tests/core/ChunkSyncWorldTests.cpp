// The World calls networking stage 7a's chunk sync is built on (docs/design/networking.md "Stage 7a
// plan", "World and sim APIs"): an edit clipped to some chunks, a chunk encoded and installed whole
// into any cell state, the list of cells that streaming loaded, and the window's revision.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <tuple>
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

    /// A flat generator that can hold one chunk's generation until released (a chunk "still
    /// loading"); classifies no column, so every row of the window gets a job.
    class GatedGenerator final : public IWorldGenerator {
    public:
        GatedGenerator() : m_Flat(CreateFlatGenerator(kGround, kSurface, kRock)) {}
        ChunkContent Generate(const glm::ivec3& coord, Chunk& chunk) const override {
            std::unique_lock lock(m_Mutex);
            m_Cv.wait(lock, [&] { return !m_Holding || coord != m_Held; });
            lock.unlock();
            return m_Flat->Generate(coord, chunk);
        }
        HeightBounds VerticalBounds() const override { return {-16.0f, 47.0f}; }
        /// The flat generator's classes: sky and rock rows are installed by the column pass (no job).
        ColumnClass ClassifyColumn(int cx, int cz) const override { return m_Flat->ClassifyColumn(cx, cz); }
        void Hold(const glm::ivec3& c) {
            std::scoped_lock lock(m_Mutex);
            m_Holding = true;
            m_Held = c;
        }
        void Release() {
            {
                std::scoped_lock lock(m_Mutex);
                m_Holding = false;
            }
            m_Cv.notify_all();
        }

    private:
        std::shared_ptr<IWorldGenerator> m_Flat;
        mutable std::mutex m_Mutex;
        mutable std::condition_variable m_Cv;
        bool m_Holding = false;
        glm::ivec3 m_Held{0};
    };

    /// Releases a held generator however the test leaves its scope (review of stage 7a: a failed
    /// ASSERT with the generator held made the World's jobs wait for ever, a hang instead of a failure).
    struct ReleaseOnExit {
        GatedGenerator& generator;
        ~ReleaseOnExit() { generator.Release(); }
    };

    /// A full-height flat world at 0.5 m (16 m chunks): rows -1..2, the build ceiling at 48 m.
    struct Bench {
        explicit Bench(uint32_t capacity = 1024, bool reportLoads = false, bool keepUnloadedEdits = true, int32_t slack = 0)
            : store(capacity), jobs(3), generator(std::make_shared<GatedGenerator>()) {
            WorldConfig config;
            config.voxelSize = 0.5f;
            config.windowSize = {6, 4, 6};
            config.fullHeight = true;
            config.heightRange = {-16.0f, 48.0f};
            config.reportLoads = reportLoads;
            config.keepUnloadedEdits = keepUnloadedEdits;
            config.recenterSlackChunks = slack;
            Material rock;
            rock.hardness = 10.0f;
            materials.Set(kWorldPalette, kSurface, Material{}, "surface");
            materials.Set(kWorldPalette, kRock, rock, "rock");
            world = std::make_unique<World>(config, store, jobs, generator, &materials);
        }
        void Load(const glm::dvec3& at = glm::dvec3(0.0, 20.0, 0.0)) { world->LoadBlocking(at); }
        uint64_t Hash() const { return WorldHash(*world, world->WindowMinChunk(), world->WindowMinChunk() + world->WindowSize()).hash; }

        ChunkStore store;
        JobSystem jobs;
        MaterialLibrary materials;
        std::shared_ptr<GatedGenerator> generator;
        std::unique_ptr<World> world;
    };

    uint64_t HashOf(const World& w, const glm::ivec3& c) {
        Chunk chunk;
        EXPECT_TRUE(w.ReadChunk(c, chunk));
        return ChunkHash(chunk);
    }

    bool SameEdits(const std::vector<VoxelEdit>& a, const std::vector<VoxelEdit>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].voxel != b[i].voxel || a[i].oldId != b[i].oldId || a[i].newId != b[i].newId) return false;
        return true;
    }

    /// One edit, whole or clipped: the kinds a replicated operation can be.
    enum class Kind { Carve, Fill, Swap, Box, Clear };
    struct Edit {
        Kind kind = Kind::Carve;
        SphereQ sphere;
        glm::ivec3 min{0}, max{0};
        VoxelMask mask;
        uint32_t Run(World& w, std::vector<VoxelEdit>* e, EditReport* r, const ChunkClip* clip) const {
            switch (kind) {
                case Kind::Carve: return w.CarveSphereQ(sphere, 400, e, r, clip);
                case Kind::Fill: return w.FillSphereQ(sphere, kRock, e, r, clip);
                case Kind::Swap: return w.SwapSphereQ(sphere, kSurface, kRock, e, r, clip);
                case Kind::Box: return w.FillBox(min, max, kRock, e, r, clip);
                case Kind::Clear: return w.ClearMask(min, mask, e, r, clip);
            }
            return 0;
        }
        void Range(glm::ivec3& lo, glm::ivec3& hi) const {
            glm::ivec3 a, b;
            if (kind == Kind::Box) a = min, b = max;
            else if (kind == Kind::Clear) a = min, b = min + mask.Size();
            else SphereVoxelBounds(sphere, a, b);
            lo = VoxelToChunk(a);
            hi = VoxelToChunk(b - glm::ivec3(1));
        }
    };
}

// One clipped call per chunk of an edit's box, in (y, z, x) order, equals the whole call: the same
// voxels, the same edit list in the same order, the same report; an edit that would build above the
// ceiling is refused whole even when clipped to a chunk far below it.
TEST(World, EditClipEqualsTheWholeEditChunkByChunk) {
    Bench whole, clipped;
    whole.Load();
    clipped.Load();
    ASSERT_EQ(whole.Hash(), clipped.Hash());

    std::vector<Edit> edits;
    edits.push_back({Kind::Carve, SphereQ{glm::ivec3(31, 39, 2) * 16, 9 * 16}});      // across x / y / z borders
    edits.push_back({Kind::Fill, SphereQ{glm::ivec3(-33, 41, 31) * 16 + glm::ivec3(5), 7 * 16}});
    edits.push_back({Kind::Swap, SphereQ{glm::ivec3(0, 39, 0) * 16, 12 * 16}});
    edits.push_back({Kind::Box, {}, glm::ivec3(-20, 30, -5), glm::ivec3(40, 45, 3)});
    Edit clear{Kind::Clear};
    clear.min = glm::ivec3(20, 25, -10);
    clear.mask = VoxelMask(glm::ivec3(30, 20, 40));
    for (int z = 0; z < 40; z += 3)
        for (int y = 0; y < 20; y += 2)
            for (int x = (y + z) % 5; x < 30; x += 4) clear.mask.Set(x, y, z, true);
    edits.push_back(clear);
    // Partly outside the window (x from 80 voxels = chunk 5 > the window's 2): skipped chunks too.
    edits.push_back({Kind::Box, {}, glm::ivec3(60, 38, 0), glm::ivec3(120, 42, 4)});

    for (const Edit& e : edits) {
        std::vector<VoxelEdit> a, b;
        EditReport ra, rb;
        const uint32_t ca = e.Run(*whole.world, &a, &ra, nullptr);
        uint32_t cb = 0;
        glm::ivec3 lo, hi;
        e.Range(lo, hi);
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const ChunkClip clip = ChunkClip::One({x, y, z});
                    cb += e.Run(*clipped.world, &b, &rb, &clip);
                }
        EXPECT_GT(ca, 0u);
        EXPECT_EQ(ca, cb);
        EXPECT_TRUE(SameEdits(a, b));
        EXPECT_EQ(ra.skipped, rb.skipped);
        EXPECT_EQ(ra.unsynced, rb.unsynced);
        EXPECT_EQ(ra.changed, rb.changed);
        EXPECT_FALSE(ra.changed.empty());
        EXPECT_EQ(whole.Hash(), clipped.Hash());
    }

    // Above the ceiling (48 m = voxel 96): a box from the ground up to voxel 100 is refused whole, and
    // so is the same box clipped to its lowest chunk (row 1), whose voxels are all below the ceiling.
    EditReport refused, refusedClipped;
    EXPECT_EQ(whole.world->FillBox(glm::ivec3(0, 40, 0), glm::ivec3(4, 100, 4), kRock, nullptr, &refused), 0u);
    EXPECT_TRUE(refused.outOfBounds);
    const ChunkClip low = ChunkClip::One({0, 1, 0});
    EXPECT_EQ(clipped.world->FillBox(glm::ivec3(0, 40, 0), glm::ivec3(4, 100, 4), kRock, nullptr, &refusedClipped, &low), 0u);
    EXPECT_TRUE(refusedClipped.outOfBounds);
    EXPECT_EQ(whole.Hash(), clipped.Hash());
    whole.jobs.WaitIdle();
    clipped.jobs.WaitIdle();
}

// SetChunkEncoded installs into a cell in any state; it is a content change (revision, change list,
// dirty slot); a job still running for a Loading cell is discarded when it ends, and LoadBlocking
// still finishes; with no free slot nothing changes.
TEST(World, SetChunkEncodedInstallsAnyChunkAndIsAContentChange) {
    Bench src;
    src.Load();
    // Source content: a carved surface chunk (Dense), a rock chunk (Uniform), sky (Empty).
    src.world->CarveSphereQ(SphereQ{glm::ivec3(8, 40, 8) * 16, 6 * 16}, 400);
    const glm::ivec3 dense(0, 1, 0), uniform(1, -1, 1), empty(2, 2, 2);
    ASSERT_EQ(src.world->StateOf(dense), CellState::Dense);
    ASSERT_EQ(src.world->StateOf(uniform), CellState::Uniform);
    ASSERT_EQ(src.world->StateOf(empty), CellState::Empty);
    std::vector<uint8_t> denseBytes, uniformBytes, emptyBytes;
    ASSERT_TRUE(src.world->EncodeChunkAt(dense, denseBytes));
    ASSERT_TRUE(src.world->EncodeChunkAt(uniform, uniformBytes));
    ASSERT_TRUE(src.world->EncodeChunkAt(empty, emptyBytes));

    // Installed states: Dense over Dense (in place), Dense over Empty, Empty over Dense, Uniform over Empty.
    Bench dst;
    dst.Load();
    (void)dst.store.TakeDirty();
    (void)dst.world->TakeChunkChanges();
    struct Case {
        glm::ivec3 at;
        const std::vector<uint8_t>* bytes;
        uint64_t expect;
    };
    const Case cases[] = {{dense, &denseBytes, HashOf(*src.world, dense)},
                          {glm::ivec3(0, 2, 0), &denseBytes, HashOf(*src.world, dense)},
                          {glm::ivec3(1, 1, 1), &emptyBytes, kAirChunkHash},
                          {glm::ivec3(2, 2, 1), &uniformBytes, HashOf(*src.world, uniform)}};
    for (const Case& c : cases) {
        const uint64_t revision = dst.world->ChunkRevision(c.at);
        ASSERT_EQ(dst.world->SetChunkEncoded(c.at, *c.bytes), SetChunkResult::Installed);
        EXPECT_EQ(HashOf(*dst.world, c.at), c.expect);
        EXPECT_NE(dst.world->ChunkRevision(c.at), revision);
        const std::vector<ChunkChange> changes = dst.world->TakeChunkChanges();
        const auto change = std::find_if(changes.begin(), changes.end(), [&](const ChunkChange& x) { return x.coord == c.at; });
        ASSERT_NE(change, changes.end());
        // The cell was loaded: its old collider is stale everywhere (a body must not be simulated
        // against it), so the change carries the whole chunk as its edit box.
        EXPECT_TRUE(change->Edited());
        EXPECT_EQ(change->editMin, ChunkToVoxel(c.at));
        EXPECT_EQ(change->editMax, ChunkToVoxel(c.at) + glm::ivec3(kChunkSize));
        if (dst.world->StateOf(c.at) == CellState::Dense) {
            const std::vector<uint32_t> dirty = dst.store.TakeDirty();
            EXPECT_FALSE(dirty.empty()) << "a Dense install marks its slot dirty (the GPU copy must follow)";
        }
    }
    EXPECT_EQ(dst.world->TakeLoadedChunks().size(), 0u) << "installs are not loads";
    EXPECT_EQ(dst.world->SetChunkEncoded(glm::ivec3(40, 0, 0), denseBytes), SetChunkResult::OutsideWindow);
    const std::vector<uint8_t> broken = {2, 7, 1, 0};
    EXPECT_EQ(dst.world->SetChunkEncoded(dense, broken), SetChunkResult::Malformed);
    EXPECT_EQ(HashOf(*dst.world, dense), HashOf(*src.world, dense));

    // A Loading cell: its generation is held on a worker; the install wins, the late result is
    // dropped, and LoadBlocking (which waits for the surface rows) still returns.
    Bench late;
    const glm::ivec3 held(0, 1, 0);
    late.generator->Hold(held);
    const ReleaseOnExit release{*late.generator};
    for (int i = 0; i < 200 && late.world->StateOf(held) != CellState::Loading; ++i) {
        late.world->Update(glm::dvec3(0.0, 20.0, 0.0));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(late.world->StateOf(held), CellState::Loading);
    ASSERT_EQ(late.world->SetChunkEncoded(held, denseBytes), SetChunkResult::Installed);
    late.generator->Release();
    // Bounded, on this thread (the regression is a LoadBlocking that never returns: surfaceLeft not
    // settled); LoadBlocking only once the window is filled.
    bool filled = false;
    for (int i = 0; i < 5000 && !filled; ++i) {
        late.world->Update(glm::dvec3(0.0, 20.0, 0.0));
        filled = late.world->IsFilled(LoadScope::Surface) && late.world->IsFilled(LoadScope::Everything);
        if (!filled) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(filled) << "the window never fills: surfaceLeft not settled (LoadBlocking would hang)";
    late.Load();
    late.world->Update(glm::dvec3(0.0, 20.0, 0.0));
    EXPECT_EQ(HashOf(*late.world, held), HashOf(*src.world, dense)) << "the stale job result overwrote the install";
    // An Unloaded cell (the window moved there, nothing submitted yet) takes an install too.
    late.world->Update(glm::dvec3(16.0 * 6.0, 20.0, 0.0));
    const glm::ivec3 far(8, 1, 0);
    if (late.world->StateOf(far) == CellState::Unloaded || late.world->StateOf(far) == CellState::Loading) {
        ASSERT_EQ(late.world->SetChunkEncoded(far, denseBytes), SetChunkResult::Installed);
        late.Load(glm::dvec3(16.0 * 6.0, 20.0, 0.0));
        EXPECT_EQ(HashOf(*late.world, far), HashOf(*src.world, dense));
    }

    // No free slot: a Dense install over a Uniform cell changes nothing.
    Bench full(512);
    full.Load();
    while (full.store.Alloc() != ChunkStore::kInvalidSlot) {}
    const uint64_t before = full.Hash();
    EXPECT_EQ(full.world->SetChunkEncoded(uniform, denseBytes), SetChunkResult::StoreFull);
    EXPECT_EQ(full.world->StateOf(uniform), CellState::Uniform);
    EXPECT_EQ(full.Hash(), before);
    src.jobs.WaitIdle();
    dst.jobs.WaitIdle();
    late.jobs.WaitIdle();
    full.jobs.WaitIdle();
}

// What EncodeChunkAt gives, SetChunkEncoded installs as the same chunk, for every loaded state
// (Packed included: its bytes are its own).
TEST(World, EncodeChunkAtRoundTripsThroughSetChunkEncoded) {
    ChunkStore store(4096), store2(4096);
    JobSystem jobs(3);
    MaterialLibrary materials;
    WorldConfig config;
    config.voxelSize = 0.5f;
    config.windowSize = {8, 8, 8};
    config.fullHeight = true;
    config.heightRange = {-64.0f, 64.0f};
    config.packHidden = true;
    config.nearRadius = 4.0f;
    TerrainSettings terrain;
    terrain.seed = 1337;
    terrain.voxelSize = 0.5f;
    terrain.materials = RegisterDefaultWorldMaterials(materials);
    World a(config, store, jobs, CreateTerrainGenerator(terrain), &materials);
    World b(config, store2, jobs, CreateFlatGenerator(kGround, kSurface, kRock), &materials);
    a.LoadBlocking(glm::dvec3(0.0, 30.0, 0.0));
    b.LoadBlocking(glm::dvec3(0.0, 30.0, 0.0));
    std::set<CellState> seen;
    const glm::ivec3 min = a.WindowMinChunk(), size = a.WindowSize();
    for (int y = 0; y < size.y; ++y)
        for (int z = 0; z < size.z; ++z)
            for (int x = 0; x < size.x; ++x) {
                const glm::ivec3 c = min + glm::ivec3(x, y, z);
                std::vector<uint8_t> bytes;
                ASSERT_TRUE(a.EncodeChunkAt(c, bytes));
                if (a.StateOf(c) == CellState::Packed) {
                    const std::span<const uint8_t> own = a.PackedData(c);
                    EXPECT_TRUE(std::equal(own.begin(), own.end(), bytes.begin(), bytes.end()));
                }
                seen.insert(a.StateOf(c));
                ASSERT_EQ(b.SetChunkEncoded(c, bytes), SetChunkResult::Installed);
                EXPECT_EQ(HashOf(b, c), HashOf(a, c));
            }
    EXPECT_TRUE(seen.contains(CellState::Dense));
    EXPECT_TRUE(seen.contains(CellState::Packed));
    EXPECT_TRUE(seen.contains(CellState::Uniform));
    EXPECT_TRUE(seen.contains(CellState::Empty));
    EXPECT_EQ(WorldHash(a, min, min + size).hash, WorldHash(b, min, min + size).hash);
    std::vector<uint8_t> none;
    EXPECT_FALSE(a.EncodeChunkAt(min + glm::ivec3(100, 0, 0), none));
    EXPECT_TRUE(none.empty());
    jobs.WaitIdle();
}

// With reportLoads every cell is reported once when streaming loads it (job results and the cells
// the column pass installs without a job alike); a window step reports only the entering slab; edits,
// installs, packing are never reported; WindowRevision counts window moves.
TEST(World, TakeLoadedChunksReportsEveryLoadOnce) {
    Bench b(2048, true);
    EXPECT_EQ(b.world->WindowRevision(), 0u);
    b.Load();
    EXPECT_EQ(b.world->WindowRevision(), 1u);
    std::vector<glm::ivec3> loaded = b.world->TakeLoadedChunks();
    const glm::ivec3 size = b.world->WindowSize();
    EXPECT_EQ(loaded.size(), static_cast<size_t>(size.x * size.y * size.z));
    std::set<std::tuple<int, int, int>> unique;
    for (const glm::ivec3& c : loaded) unique.insert({c.x, c.y, c.z});
    EXPECT_EQ(unique.size(), loaded.size()) << "a cell reported twice";

    b.world->FillBox(glm::ivec3(0, 30, 0), glm::ivec3(10, 50, 10), kRock);
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(b.world->EncodeChunkAt({0, 1, 0}, bytes));
    ASSERT_EQ(b.world->SetChunkEncoded({1, 1, 1}, bytes), SetChunkResult::Installed);
    EXPECT_TRUE(b.world->TakeLoadedChunks().empty());

    // One chunk (16 m) along +x: the entering slab, one column of chunks wide.
    b.Load(glm::dvec3(16.0, 20.0, 0.0));
    EXPECT_EQ(b.world->WindowRevision(), 2u);
    loaded = b.world->TakeLoadedChunks();
    EXPECT_EQ(loaded.size(), static_cast<size_t>(size.y * size.z));
    const int enteringX = b.world->WindowMinChunk().x + size.x - 1;
    for (const glm::ivec3& c : loaded) EXPECT_EQ(c.x, enteringX);
    b.world->Update(glm::dvec3(16.0, 20.0, 0.0));
    EXPECT_EQ(b.world->WindowRevision(), 2u) << "no move, no revision";

    Bench off(2048, false);
    off.Load();
    EXPECT_TRUE(off.world->TakeLoadedChunks().empty());
    b.jobs.WaitIdle();
    off.jobs.WaitIdle();
}

// Review of stage 7a: a session client's window follows with one chunk of slack (a player stepping
// back and forth across a chunk border moves nothing), and with reportLoads every step is listed in
// order (TakeWindowSteps), also two between two takes.
TEST(World, TheWindowFollowsWithSlackAndListsEveryStep) {
    Bench b(2048, true, false, 1);
    b.Load();
    const glm::ivec3 home = b.world->WindowMinChunk();
    std::vector<glm::ivec3> steps = b.world->TakeWindowSteps();
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0], home);
    // Back and forth across the borders of the centre chunk (16 m chunks): within one chunk of it.
    for (const double x : {16.5, -0.5, 16.5, -15.5, 0.5}) b.world->Update(glm::dvec3(x, 20.0, 0.0));
    EXPECT_EQ(b.world->WindowRevision(), 1u);
    EXPECT_EQ(b.world->WindowMinChunk(), home);
    EXPECT_TRUE(b.world->TakeWindowSteps().empty());
    // Two chunks off: it moves, centred on the focus chunk.
    b.world->Update(glm::dvec3(33.0, 20.0, 0.0));
    EXPECT_EQ(b.world->WindowMinChunk(), home + glm::ivec3(2, 0, 0));
    EXPECT_EQ(b.world->WindowRevision(), 2u);
    steps = b.world->TakeWindowSteps();
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0], home + glm::ivec3(2, 0, 0));
    // Two steps before the next take: both, in order.
    b.world->Update(glm::dvec3(81.0, 20.0, 0.0));
    b.world->Update(glm::dvec3(33.0, 20.0, 0.0));
    steps = b.world->TakeWindowSteps();
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0], home + glm::ivec3(5, 0, 0));
    EXPECT_EQ(steps[1], home + glm::ivec3(2, 0, 0));
    EXPECT_EQ(b.world->WindowRevision(), 4u);
    // Without slack (offline) it moves at the first border; without reportLoads nothing is listed.
    Bench plain(2048, false);
    plain.Load();
    plain.world->Update(glm::dvec3(16.5, 20.0, 0.0));
    EXPECT_EQ(plain.world->WindowRevision(), 2u);
    EXPECT_TRUE(plain.world->TakeWindowSteps().empty());
    b.jobs.WaitIdle();
    plain.jobs.WaitIdle();
}

// Recheck of stage 7a, 2: with slack the focus may sit up to one chunk off the window's centre chunk,
// so the window's edge comes a chunk nearer on that side; the view radius (edge fog, fill buckets, the
// "unloads inside the view radius" count) leaves the slack out, so the cut edge never shows inside it.
TEST(World, TheViewRadiusNeverReachesPastTheWindowWithSlack) {
    for (const int32_t slack : {0, 1}) {
        Bench b(2048, true, false, slack);
        b.Load();
        const double chunkMetres = 16.0;
        // The worst places the window stays for: the far side of the chunk one off the centre (any
        // chunk off it without slack is a move), on each axis and both diagonals.
        const double off = static_cast<double>(slack) * chunkMetres + chunkMetres - 0.01;
        for (const glm::dvec2 at : {glm::dvec2(off, 0.0), glm::dvec2(-static_cast<double>(slack) * chunkMetres + 0.01, 0.0), glm::dvec2(0.0, off), glm::dvec2(off, off)}) {
            b.world->Update(glm::dvec3(at.x, 20.0, at.y));
            const glm::dvec3 lo = glm::dvec3(b.world->WindowMinChunk()) * chunkMetres;
            const glm::dvec3 hi = glm::dvec3(b.world->WindowMinChunk() + b.world->WindowSize()) * chunkMetres;
            const double edge = std::min({at.x - lo.x, hi.x - at.x, at.y - lo.z, hi.z - at.y});
            EXPECT_GE(edge, b.world->ViewRadius()) << "slack " << slack << " at (" << at.x << ", " << at.y << "): the window's edge " << edge
                                                   << " m away, view radius " << b.world->ViewRadius() << " m";
        }
        // Window 6: half 3, so 2 chunks of radius without slack, 1 with it.
        EXPECT_DOUBLE_EQ(b.world->ViewRadius(), static_cast<double>(2 - slack) * chunkMetres);
        b.jobs.WaitIdle();
    }
}

// keepUnloadedEdits false (a session client): an edited chunk that leaves the window comes back as
// the generator makes it; by default it comes back with its edit.
TEST(World, UnloadedEditsAreDroppedWithoutKeepUnloadedEdits) {
    for (const bool keep : {true, false}) {
        Bench b(2048, false, keep);
        b.Load();
        const uint64_t pristine = HashOf(*b.world, {0, 1, 0});
        b.world->CarveSphereQ(SphereQ{glm::ivec3(8, 40, 8) * 16, 6 * 16}, 400);
        const uint64_t edited = HashOf(*b.world, {0, 1, 0});
        ASSERT_NE(edited, pristine);
        b.Load(glm::dvec3(16.0 * 20.0, 20.0, 0.0));
        b.Load(glm::dvec3(0.0, 20.0, 0.0));
        EXPECT_EQ(HashOf(*b.world, {0, 1, 0}), keep ? edited : pristine);
        b.jobs.WaitIdle();
    }
}
