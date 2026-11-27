module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

module vortex.voxel;

import vortex.core;
import vortex.math;

namespace vortex::voxel {

    // State shared with worker jobs. Jobs hold a shared_ptr to it, so a World can be destroyed
    // while jobs are still queued: their results are simply dropped.
    struct World::Shared {
        struct Result {
            uint32_t cellIndex = 0;
            glm::ivec3 coord{0};
            uint32_t generation = 0;
            ChunkKind kind = ChunkKind::Empty;
            uint8_t uniformId = 0;
            bool fromSave = false;
            std::unique_ptr<Chunk> chunk;
            std::vector<uint8_t> packed;     // RLE of a Dense result, when the world packs hidden chunks
            uint8_t packedId = 0;            // its most frequent solid id
        };
        struct ColumnResult {
            uint32_t columnIndex = 0;
            glm::ivec2 coord{0};
            uint32_t generation = 0;
            ColumnClass cls;
            int rowMin = 0, rowMax = 0;      // rows checked for saved chunks
            std::vector<int> savedRows;      // rows outside the band that hold a saved chunk
        };

        Shared(std::filesystem::path saveDir, float voxelSize) : regions(std::move(saveDir), voxelSize) {}

        std::shared_ptr<const IWorldGenerator> generator;
        RegionStore regions;
        std::mutex mutex;
        std::vector<Result> results;
        std::vector<ColumnResult> columnResults;
        std::atomic<uint32_t> inFlight{0};
        // Chunks the generator produced (not loaded from a save), counted by the worker that made them.
        std::atomic<uint32_t> generated{0};
        std::atomic<uint32_t> generatedDense{0};
        std::atomic<uint64_t> generateNs{0};   // worker time spent in IWorldGenerator::Generate
    };

    namespace {
        // Grid word of a band cell whose chunk is not loaded yet (full-height window): a ray stops
        // there and shows the fog / horizon colour (uniform flag with material 0).
        constexpr uint32_t kGridPending = 0x80000000u;
        constexpr int kMaxRows = 128;

        bool TestBit(const uint64_t (&bits)[2], int i) { return ((bits[i >> 6] >> (i & 63)) & 1u) != 0; }
        void SetBit(uint64_t (&bits)[2], int i) { bits[i >> 6] |= uint64_t{1} << (i & 63); }
        void ClearBit(uint64_t (&bits)[2], int i) { bits[i >> 6] &= ~(uint64_t{1} << (i & 63)); }
        bool AnyBit(const uint64_t (&bits)[2]) { return (bits[0] | bits[1]) != 0; }

        double Seconds(std::chrono::steady_clock::time_point since) {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
        }

        bool IsDenseWord(uint32_t word) { return word != 0u && (word & 0x80000000u) == 0u; }

        /// The id a Packed chunk's grid word shows: its most frequent solid id (0 if all air).
        uint8_t MostFrequentSolidId(const std::vector<uint8_t>& rle) {
            uint32_t counts[256] = {};
            for (size_t p = 1; p + 2 < rle.size(); p += 3) counts[rle[p]] += static_cast<uint32_t>(rle[p + 1] | (rle[p + 2] << 8));
            uint8_t best = 0;
            for (uint32_t id = 1; id < 256; ++id) {
                if (counts[id] > 0 && (best == 0 || counts[id] > counts[best])) best = static_cast<uint8_t>(id);
            }
            return best;
        }

        std::atomic<uint64_t> g_NextWorldId{1};
    }

    World::World(const WorldConfig& config, ChunkStore& store, JobSystem& jobs,
                 std::shared_ptr<const IWorldGenerator> generator, const MaterialLibrary* materials)
        : m_Config(config), m_Store(store), m_Jobs(jobs), m_Materials(materials) {
        Verify(m_Config.voxelSize > 0.0f, "WorldConfig::voxelSize must be positive");
        Verify(generator != nullptr, "World needs a generator");

        // Full height: the rows of the vertical range replace windowSize.y, and stay put.
        if (m_Config.fullHeight) {
            HeightBounds bounds;
            if (m_Config.heightRange.y > m_Config.heightRange.x) {
                bounds = {m_Config.heightRange.x, m_Config.heightRange.y};
            } else {
                bounds = generator->VerticalBounds();
                bounds.max += m_Config.buildHeadroom;
            }
            if (bounds.Known() && std::isfinite(bounds.min) && std::isfinite(bounds.max)) {
                const double chunkMetres = static_cast<double>(kChunkSize) * m_Config.voxelSize;
                const double snap = m_Config.heightSnap > 0.0f ? static_cast<double>(m_Config.heightSnap) : chunkMetres;
                const double lo = std::floor(static_cast<double>(bounds.min) / snap) * snap;
                const double hi = std::ceil(static_cast<double>(bounds.max) / snap) * snap;
                const int rowMin = static_cast<int>(std::floor(lo / chunkMetres + 1.0e-6));
                const int rowMax = static_cast<int>(std::ceil(hi / chunkMetres - 1.0e-6));
                m_Config.windowSize.y = rowMax - rowMin;
                m_WindowMin.y = rowMin;
                m_FullHeight = true;
            } else {
                Log::Warn("WorldConfig::fullHeight: the generator declares no vertical bounds and no heightRange is set; "
                          "the window stays camera-centred ({} rows)", m_Config.windowSize.y);
            }
        }
        m_Pack = m_FullHeight && m_Config.packHidden;

        const glm::ivec3 s = m_Config.windowSize;
        Verify(s.x > 0 && s.y > 0 && s.z > 0, "WorldConfig::windowSize components must be positive");
        Verify(s.y <= kMaxRows, "World: at most 128 rows of chunks");

        m_Shared = std::make_shared<Shared>(m_Config.saveDirectory, m_Config.voxelSize);
        m_Shared->generator = std::move(generator);

        m_CellCount = static_cast<uint32_t>(static_cast<size_t>(s.x) * static_cast<size_t>(s.y) * static_cast<size_t>(s.z));
        m_ColumnCount = static_cast<uint32_t>(s.x * s.z);
        m_Cells.resize(m_CellCount);
        m_Packed.resize(m_CellCount);
        m_Fallback.assign(m_CellCount, 0u);
        m_InstanceId = g_NextWorldId.fetch_add(1);
        m_Columns.resize(m_ColumnCount);
        m_RegionsX = static_cast<uint32_t>((s.x + 3) / 4);
        m_RegionCount = m_RegionsX * static_cast<uint32_t>((s.z + 3) / 4);
        m_Grid.assign(static_cast<size_t>(m_CellCount) + m_ColumnCount + 1u + m_RegionCount, 0u);
        m_GridPages.assign((m_Grid.size() + kGridPageWords - 1) / kGridPageWords, 1u);
        m_MaxJobs = m_Config.maxJobsInFlight != 0 ? m_Config.maxJobsInFlight : std::max(2u, m_Jobs.ThreadCount() * 8u);
    }

    World::~World() {
        if (!m_Config.saveDirectory.empty()) Save();
        for (Cell& cell : m_Cells) {
            if (cell.state == CellState::Dense && cell.slot != ChunkStore::kInvalidSlot) m_Store.Free(cell.slot);
        }
    }

    // --- coordinates --------------------------------------------------------------------------

    glm::ivec3 World::WorldToVoxel(const glm::dvec3& p) const {
        const double inv = 1.0 / static_cast<double>(m_Config.voxelSize);
        return glm::ivec3(glm::floor(p * inv));
    }

    glm::dvec3 World::VoxelToWorld(const glm::ivec3& v) const { return glm::dvec3(v) * static_cast<double>(m_Config.voxelSize); }

    glm::dvec3 World::VoxelCenter(const glm::ivec3& v) const {
        return (glm::dvec3(v) + glm::dvec3(0.5)) * static_cast<double>(m_Config.voxelSize);
    }

    glm::dvec3 World::WindowOrigin() const { return VoxelToWorld(ChunkToVoxel(m_WindowMin)); }

    double World::BuildCeiling() const {
        if (!m_FullHeight) return std::numeric_limits<double>::infinity();
        return static_cast<double>(m_WindowMin.y + m_Config.windowSize.y) * kChunkSize * static_cast<double>(m_Config.voxelSize);
    }

    double World::ViewRadius() const {
        const int half = std::min(m_Config.windowSize.x, m_Config.windowSize.z) / 2;
        // The follow slack lets the focus sit that many chunks off the centre chunk, so the edge on
        // that side comes as many chunks nearer (recheck of networking 7a, 2).
        const int slack = std::max(m_Config.recenterSlackChunks, 0);
        return static_cast<double>(std::max(half - 1 - slack, 0)) * kChunkSize * static_cast<double>(m_Config.voxelSize);
    }

    uint32_t World::CellIndex(const glm::ivec3& chunk) const {
        const glm::ivec3 s = m_Config.windowSize;
        const glm::ivec3 w = glm::ivec3(math::FloorMod(chunk.x, s.x), math::FloorMod(chunk.y, s.y), math::FloorMod(chunk.z, s.z));
        // Must match VxTraceWorld in shaders/shared/trace.glsl.
        // Column-major: the rows of a column are adjacent, so moving the window touches a few
        // contiguous runs. Must match VxTraceWorld in shaders/shared/trace.glsl.
        return static_cast<uint32_t>((w.x + w.z * s.x) * s.y + w.y);
    }

    uint32_t World::ColumnIndex(int cx, int cz) const {
        const glm::ivec3 s = m_Config.windowSize;
        return static_cast<uint32_t>(math::FloorMod(cx, s.x) + math::FloorMod(cz, s.z) * s.x);
    }

    World::Cell* World::FindCell(const glm::ivec3& chunk) {
        Cell& cell = m_Cells[CellIndex(chunk)];
        return (cell.assigned && cell.coord == chunk) ? &cell : nullptr;
    }

    const World::Cell* World::FindCell(const glm::ivec3& chunk) const {
        const Cell& cell = m_Cells[CellIndex(chunk)];
        return (cell.assigned && cell.coord == chunk) ? &cell : nullptr;
    }

    World::Column* World::FindColumn(int cx, int cz) {
        Column& column = m_Columns[ColumnIndex(cx, cz)];
        return (column.assigned && column.coord == glm::ivec2(cx, cz)) ? &column : nullptr;
    }

    const World::Column* World::FindColumn(int cx, int cz) const {
        const Column& column = m_Columns[ColumnIndex(cx, cz)];
        return (column.assigned && column.coord == glm::ivec2(cx, cz)) ? &column : nullptr;
    }

    glm::ivec3 World::WindowMinFor(const glm::ivec3& focusChunk) const {
        glm::ivec3 min = focusChunk - m_Config.windowSize / 2;
        if (m_FullHeight) min.y = m_WindowMin.y;      // fixed vertical range
        return min;
    }

    glm::ivec3 World::FollowMinFor(const glm::ivec3& focusChunk) const {
        const glm::ivec3 min = WindowMinFor(focusChunk);
        const int32_t slack = m_Config.recenterSlackChunks;
        if (!m_HasWindow || slack <= 0 || min.y != m_WindowMin.y) return min;
        // The centre chunk of the current window is the focus chunk it was placed for.
        const glm::ivec3 centre = m_WindowMin + m_Config.windowSize / 2;
        if (std::abs(focusChunk.x - centre.x) <= slack && std::abs(focusChunk.z - centre.z) <= slack) return m_WindowMin;
        return min;
    }

    double World::DistanceToFocus(const glm::ivec3& chunk) const {
        const double size = static_cast<double>(kChunkSize) * m_Config.voxelSize;
        const glm::dvec3 lo = glm::dvec3(chunk) * size;
        const glm::dvec3 nearest = glm::clamp(m_Focus, lo, lo + glm::dvec3(size));
        return glm::length(m_Focus - nearest);
    }

    double World::HorizontalDistanceToFocus(int cx, int cz) const {
        const double size = static_cast<double>(kChunkSize) * m_Config.voxelSize;
        const glm::dvec2 lo = glm::dvec2(cx, cz) * size;
        const glm::dvec2 focus(m_Focus.x, m_Focus.z);
        return glm::length(focus - glm::clamp(focus, lo, lo + glm::dvec2(size)));
    }

    // Near-sphere guarantee (every chunk within nearRadius of the focus is resident): PromoteNear
    // scans nearRadius + h whenever the focus moved more than h since its last scan, and a load
    // result is packed only beyond nearRadius + 2h. Between two scans the focus stays within h of
    // the last one, and within 2h of where any chunk was packed since. h = a quarter chunk.
    double World::NearSlack() const { return 0.25 * static_cast<double>(kChunkSize) * m_Config.voxelSize; }

    // --- streaming ----------------------------------------------------------------------------

    void World::Update(const glm::dvec3& focus) {
        m_Focus = focus;
        if (glm::length(focus - m_NearScanFocus) > NearSlack()) m_NearPromotePending = true;
        m_FocusChunk = VoxelToChunk(WorldToVoxel(focus));
        const glm::ivec3 newMin = FollowMinFor(m_FocusChunk);
        if (!m_HasWindow || newMin != m_WindowMin) Recenter(newMin);
        IntegrateResults(m_Config.maxIntegrationsPerUpdate);
        m_DecodeBudget = m_Config.maxDecodesPerUpdate;
        PromoteNear();
        ProcessReveals();
        Repack();
        SubmitJobs();
        RefreshContentTop();
        FreeGarbage();
    }

    bool World::IsFilled(LoadScope scope) const {
        if (!m_HasWindow) return false;
        for (const Column& column : m_Columns) {
            if (column.classState != Column::Class::Known) return false;
            if (scope == LoadScope::Surface ? column.surfaceLeft > 0 : (AnyBit(column.surfaceRows) || AnyBit(column.caveRows))) return false;
        }
        if (scope == LoadScope::Surface) return true;
        if (m_Shared->inFlight.load() != 0) return false;
        std::scoped_lock lock(m_Shared->mutex);
        return m_Shared->results.empty() && m_Shared->columnResults.empty();
    }

    bool World::LoadComplete(LoadScope scope) const {
        for (const Column& column : m_Columns) {
            if (column.classState != Column::Class::Known) return false;
            if (scope == LoadScope::Surface && column.surfaceLeft > 0) return false;
        }
        if (scope == LoadScope::Everything) {
            for (const Cell& cell : m_Cells) {
                if (cell.state == CellState::Unloaded || cell.state == CellState::Loading) return false;
            }
            return true;
        }
        // Surface: and everything near the focus.
        const double size = static_cast<double>(kChunkSize) * m_Config.voxelSize;
        const double r = m_Config.nearRadius;
        const glm::ivec3 lo = glm::ivec3(glm::floor((m_Focus - glm::dvec3(r)) / size));
        const glm::ivec3 hi = glm::ivec3(glm::floor((m_Focus + glm::dvec3(r)) / size));
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const glm::ivec3 c(x, y, z);
                    const Cell* cell = FindCell(c);
                    if (!cell || DistanceToFocus(c) > r) continue;
                    if (cell->state == CellState::Unloaded || cell->state == CellState::Loading) return false;
                }
        return true;
    }

    void World::LoadBlocking(const glm::dvec3& focus, LoadScope scope) {
        const uint32_t savedDecodes = m_Config.maxDecodesPerUpdate;
        m_Config.maxDecodesPerUpdate = 0xFFFFFFFFu;
        for (;;) {
            m_Focus = focus;
            if (glm::length(focus - m_NearScanFocus) > NearSlack()) m_NearPromotePending = true;
            m_FocusChunk = VoxelToChunk(WorldToVoxel(focus));
            const glm::ivec3 newMin = FollowMinFor(m_FocusChunk);
            if (!m_HasWindow || newMin != m_WindowMin) Recenter(newMin);
            IntegrateResults(0xFFFFFFFFu);
            m_DecodeBudget = 0xFFFFFFFFu;
            PromoteNear();
            ProcessReveals();
            Repack();
            SubmitJobs();

            bool pendingResults;
            {
                std::scoped_lock lock(m_Shared->mutex);
                pendingResults = !m_Shared->results.empty() || !m_Shared->columnResults.empty();
            }
            const bool idle = m_Shared->inFlight.load() == 0 && !pendingResults;
            if (LoadComplete(scope) && (idle || scope == LoadScope::Surface) && !m_NearPromotePending && m_Reveal.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        RefreshContentTop();
        m_Config.maxDecodesPerUpdate = savedDecodes;
    }

    void World::Recenter(const glm::ivec3& newMin) {
        const auto start = std::chrono::steady_clock::now();
        const glm::ivec3 s = m_Config.windowSize;
        const glm::ivec3 oldMin = m_WindowMin;
        const bool first = !m_HasWindow;
        const bool teleport = first || std::abs(newMin.x - oldMin.x) >= s.x || std::abs(newMin.z - oldMin.z) >= s.z;
        const bool vertical = !first && newMin.y != oldMin.y;
        m_WindowMin = newMin;
        m_HasWindow = true;
        ++m_GridRevision;
        ++m_WindowRevision;
        if (m_Config.reportLoads) m_WindowSteps.push_back(newMin);

        // Only columns whose chunk coordinate changed are touched: on a step of one chunk that is
        // one slab of columns (all their rows), not the whole window.
        for (int wz = 0; wz < s.z; ++wz)
            for (int wx = 0; wx < s.x; ++wx) {
                const uint32_t ci = static_cast<uint32_t>(wx + wz * s.x);
                const glm::ivec2 target(newMin.x + math::FloorMod(wx - newMin.x, s.x), newMin.z + math::FloorMod(wz - newMin.z, s.z));
                Column& column = m_Columns[ci];
                if (!column.assigned || column.coord != target || first) {
                    RetargetColumn(ci, target);
                    if (!first) ++m_Counters.retargetedColumns;
                    continue;
                }
                if (!vertical) continue;
                // Camera-centred window moved vertically: re-target the cells whose row changed.
                // They held chunks inside the view radius: that is the unload the full-height window avoids.
                for (int wy = 0; wy < s.y; ++wy) {
                    const uint32_t index = ci * static_cast<uint32_t>(s.y) + static_cast<uint32_t>(wy);
                    const int row = newMin.y + math::FloorMod(wy - newMin.y, s.y);
                    Cell& cell = m_Cells[index];
                    if (cell.assigned && cell.coord.y == row) continue;
                    RetargetCell(index, glm::ivec3(target.x, row, target.y));
                }
                // Rows moved relative to the window: rebuild the column's queues and top. Rows
                // the column job did not look at for saved chunks get a job each.
                column.savedRows[0] = column.savedRows[1] = ~uint64_t{0};
                column.surfaceRows[0] = column.surfaceRows[1] = 0;
                column.caveRows[0] = column.caveRows[1] = 0;
                column.surfaceLeft = 0;
                column.top = 0;
                for (int wy = 0; wy < s.y; ++wy) {
                    const uint32_t index = ci * static_cast<uint32_t>(s.y) + static_cast<uint32_t>(wy);
                    const int rel = m_Cells[index].coord.y - newMin.y;
                    if (m_Grid[index] != 0u) column.top = static_cast<uint16_t>(std::max<int>(column.top, rel + 1));
                    if (column.classState != Column::Class::Known) continue;
                    if (m_Cells[index].state == CellState::Unloaded) PlaceCell(index, column, rel);
                    else if (m_Cells[index].state == CellState::Loading)
                        ++column.surfaceLeft;      // every row counts as saved (surface) after a vertical move
                }
                m_Grid[m_CellCount + ci] = column.top;
                m_GridPages[(m_CellCount + ci) / kGridPageWords] = m_GridRevision;
                m_ContentTopStale = true;
            }
        Rebucket();
        RebuildRegionTops();       // the regions are window-relative: the mapping moved with the window
        m_NearPromotePending = true;
        m_ContentTopStale = true;
        RefreshContentTop();

        FreeGarbage();
        const float ms = static_cast<float>(Seconds(start) * 1000.0);
        ++m_Counters.recenters;
        m_Counters.lastRecenterMs = ms;
        // The gate is the cost of a step of one chunk (walking, flying): larger jumps are counted apart.
        if (!teleport && std::abs(newMin.x - oldMin.x) <= 1 && std::abs(newMin.z - oldMin.z) <= 1)
            m_Counters.maxRecenterMs = std::max(m_Counters.maxRecenterMs, ms);
    }

    void World::FreeGarbage() {
        if (m_Garbage.chunks.empty() && m_Garbage.bytes.empty()) return;
        auto garbage = std::make_shared<Garbage>(std::move(m_Garbage));
        m_Garbage = {};
        m_Jobs.Submit([garbage = std::move(garbage)]() mutable { garbage.reset(); });
    }

    void World::RetargetCell(uint32_t index, const glm::ivec3& coord) {
        Cell& cell = m_Cells[index];
        // The hill bug, measured by what is lost: a chunk whose column is still inside the view
        // radius. Window moves of the right size never do that (the column that leaves is
        // farther than ViewRadius()); a wrong window placement or a vertical move does.
        if (cell.assigned && cell.state != CellState::Unloaded && HorizontalDistanceToFocus(cell.coord.x, cell.coord.z) < ViewRadius())
            ++m_Counters.unloadsInsideRadius;
        if (cell.assigned && (cell.state == CellState::Empty || cell.state == CellState::Unloaded) && !cell.modified && m_Grid[index] == 0u) {
            ++cell.generation;     // sky: nothing to save, free or report (most cells of a full-height column)
        } else if (cell.assigned) {
            UnloadCell(index);
        } else {
            ++cell.generation;
        }
        cell.coord = coord;
        cell.assigned = true;
        cell.state = CellState::Unloaded;
        cell.fromSave = false;
        cell.serverVisibility = 0;
        BumpRevision(cell);
    }

    void World::RetargetColumn(uint32_t ci, const glm::ivec2& coord) {
        const glm::ivec3 s = m_Config.windowSize;
        Column& column = m_Columns[ci];
        column.coord = coord;
        column.assigned = true;
        ++column.generation;
        column.classState = Column::Class::None;
        column.cls = ColumnClass{};
        column.surfaceRows[0] = column.surfaceRows[1] = 0;
        column.caveRows[0] = column.caveRows[1] = 0;
        column.surfaceLeft = 0;
        column.savedRows[0] = column.savedRows[1] = 0;
        column.top = 0;          // every cell goes: no search for the next top on each unload
        for (int wy = 0; wy < s.y; ++wy) {
            const uint32_t index = ci * static_cast<uint32_t>(s.y) + static_cast<uint32_t>(wy);
            const int row = m_WindowMin.y + math::FloorMod(wy - m_WindowMin.y, s.y);
            RetargetCell(index, glm::ivec3(coord.x, row, coord.y));
        }
        if (m_Grid[m_CellCount + ci] != 0u) {
            m_Grid[m_CellCount + ci] = 0u;
            m_GridPages[(m_CellCount + ci) / kGridPageWords] = ++m_GridRevision;
            m_ContentTopStale = true;
            m_RegionTopsStale = true;
        }
    }

    void World::Rebucket() {
        // Counting sort of the columns by horizontal distance (whole chunks) from the focus column.
        const int maxD2 = m_Config.windowSize.x * m_Config.windowSize.x + m_Config.windowSize.z * m_Config.windowSize.z;
        if (m_BucketOfD2.size() != static_cast<size_t>(maxD2) + 1) {
            m_BucketOfD2.resize(static_cast<size_t>(maxD2) + 1);
            for (int d2 = 0; d2 <= maxD2; ++d2) m_BucketOfD2[static_cast<size_t>(d2)] = static_cast<uint16_t>(std::sqrt(static_cast<double>(d2)));
            m_BucketStart.assign(static_cast<size_t>(m_BucketOfD2.back()) + 2, 0u);
            m_BucketOrder.resize(m_ColumnCount);
        }
        std::fill(m_BucketStart.begin(), m_BucketStart.end(), 0u);
        for (uint32_t ci = 0; ci < m_ColumnCount; ++ci) {
            Column& column = m_Columns[ci];
            const int dx = std::min(std::abs(column.coord.x - m_FocusChunk.x), m_Config.windowSize.x);
            const int dz = std::min(std::abs(column.coord.y - m_FocusChunk.z), m_Config.windowSize.z);
            column.bucket = m_BucketOfD2[static_cast<size_t>(dx * dx + dz * dz)];
            ++m_BucketStart[column.bucket + 1u];
        }
        for (size_t b = 1; b < m_BucketStart.size(); ++b) m_BucketStart[b] += m_BucketStart[b - 1];
        std::vector<uint32_t>& cursor = m_BucketCursor;
        cursor.assign(m_BucketStart.begin(), m_BucketStart.end());
        for (uint32_t ci = 0; ci < m_ColumnCount; ++ci) m_BucketOrder[cursor[m_Columns[ci].bucket]++] = ci;
    }

    void World::PersistCell(Cell& cell) {
        ChunkKind kind = ChunkKind::Empty;
        uint8_t uniformId = 0;
        const Chunk* chunk = nullptr;
        if (cell.state == CellState::Dense) {
            chunk = &m_Store.Get(cell.slot);
            const int uniform = chunk->UniformId();
            if (chunk->IsEmpty()) {
                chunk = nullptr;
            } else if (uniform > 0) {
                kind = ChunkKind::Uniform;
                uniformId = static_cast<uint8_t>(uniform);
                chunk = nullptr;
            } else {
                kind = ChunkKind::Dense;
            }
        } else if (cell.state == CellState::Uniform) {
            kind = ChunkKind::Uniform;
            uniformId = cell.uniformId;
        }
        m_Shared->regions.Save(cell.coord, EncodeChunk(kind, uniformId, chunk));
        cell.modified = false;
    }

    void World::UnloadCell(uint32_t index) {
        Cell& cell = m_Cells[index];
        const bool hadContent = cell.state == CellState::Dense || cell.state == CellState::Uniform || cell.state == CellState::Packed;
        // A Packed chunk is never modified (an edit makes it resident first).
        // (A session client drops it: keepUnloadedEdits false, the server sends it again.)
        if (m_Config.keepUnloadedEdits && cell.modified &&
            (cell.state == CellState::Dense || cell.state == CellState::Uniform || cell.state == CellState::Empty))
            PersistCell(cell);
        // Memory goes to a worker to be released (Recenter frees a whole slab of columns: hundreds
        // of heap blocks allocated by other threads).
        if (cell.state == CellState::Dense && cell.slot != ChunkStore::kInvalidSlot) m_Garbage.chunks.push_back(m_Store.Release(cell.slot));
        DropKeptBytes(index);       // Packed bytes, or the copy a resident buried chunk keeps
        // The cell is about to represent a different chunk: report the old one directly.
        // (An entry queued before keeps the old coordinate and its edit box.)
        if (hadContent && !cell.changeQueued) m_ChangedChunks.push_back({cell.coord});

        cell.slot = ChunkStore::kInvalidSlot;
        cell.state = CellState::Unloaded;
        cell.uniformId = 0;
        cell.packedId = 0;
        cell.modified = false;
        cell.changeQueued = false;
        cell.fromSave = false;
        cell.serverVisibility = 0;
        ++cell.generation;   // results of jobs still in flight for the old chunk are now stale
        BumpRevision(cell);
        WriteGrid(index, 0u);
    }

    void World::SetGrid(uint32_t index, const Cell& cell) {
        uint32_t entry = 0u;
        if (cell.state == CellState::Dense) entry = cell.slot + 1u;
        else if (cell.state == CellState::Uniform || cell.state == CellState::Packed) entry = 0x80000000u | cell.uniformId;
        WriteGrid(index, entry);
    }

    void World::DropKeptBytes(uint32_t index) {
        if (m_Packed[index].empty() && m_Packed[index].capacity() == 0) return;
        m_Counters.packedBytes -= m_Packed[index].size();
        m_Garbage.bytes.push_back(std::move(m_Packed[index]));
        m_Packed[index] = {};
    }

    void World::WriteGrid(uint32_t index, uint32_t entry) {
        if (m_Grid[index] == entry) return;
        // What the cell showed before it became dense, for a renderer that has not uploaded the slot yet.
        if (IsDenseWord(entry) && !IsDenseWord(m_Grid[index])) m_Fallback[index] = m_Grid[index];
        const bool wasEmpty = m_Grid[index] == 0u;
        m_Grid[index] = entry;
        m_GridPages[index / kGridPageWords] = ++m_GridRevision;
        if (wasEmpty != (entry == 0u)) {
            const glm::ivec3 s = m_Config.windowSize;
            const int wy = static_cast<int>(index % static_cast<uint32_t>(s.y));
            UpdateColumnTop(index / static_cast<uint32_t>(s.y), math::FloorMod(wy - m_WindowMin.y, s.y), entry != 0u);
        }
    }

    void World::UpdateColumnTop(uint32_t ci, int rel, bool nonEmpty) {
        Column& column = m_Columns[ci];
        int top = column.top;
        if (nonEmpty) {
            if (rel + 1 <= top) return;
            top = rel + 1;
        } else {
            if (rel + 1 != top) return;
            // The highest non-empty cell went: search down for the next one.
            const glm::ivec3 s = m_Config.windowSize;
            while (top > 0) {
                const int wy = math::FloorMod(m_WindowMin.y + top - 1, s.y);
                if (m_Grid[ci * static_cast<uint32_t>(s.y) + static_cast<uint32_t>(wy)] != 0u) break;
                --top;
            }
            m_ContentTopStale = true;
            m_RegionTopsStale = true;
        }
        column.top = static_cast<uint16_t>(top);
        const uint32_t word = m_CellCount + ci;
        m_Grid[word] = static_cast<uint32_t>(top);
        m_GridPages[word / kGridPageWords] = ++m_GridRevision;
        // Raised tops are applied at once (a ray must never skip content); lowered ones lazily
        // (a top that is too high only costs steps).
        uint32_t& contentTop = m_Grid[m_CellCount + m_ColumnCount];
        if (static_cast<uint32_t>(top) > contentTop) {
            contentTop = static_cast<uint32_t>(top);
            m_GridPages[(m_CellCount + m_ColumnCount) / kGridPageWords] = m_GridRevision;
        }
        const uint32_t regionWord = m_CellCount + m_ColumnCount + 1u + RegionOfColumn(ci);
        if (static_cast<uint32_t>(top) > m_Grid[regionWord]) {
            m_Grid[regionWord] = static_cast<uint32_t>(top);
            m_GridPages[regionWord / kGridPageWords] = m_GridRevision;
        }
    }

    uint32_t World::RegionOfColumn(uint32_t ci) const {
        const glm::ivec3 s = m_Config.windowSize;
        const int wx = static_cast<int>(ci % static_cast<uint32_t>(s.x)), wz = static_cast<int>(ci / static_cast<uint32_t>(s.x));
        const int cx = math::FloorMod(wx - m_WindowMin.x, s.x), cz = math::FloorMod(wz - m_WindowMin.z, s.z);
        return static_cast<uint32_t>(cx / 4) + static_cast<uint32_t>(cz / 4) * m_RegionsX;
    }

    void World::RebuildRegionTops() {
        m_RegionTopsStale = false;
        const size_t base = static_cast<size_t>(m_CellCount) + m_ColumnCount + 1u;
        const glm::ivec3 s = m_Config.windowSize;
        std::vector<uint32_t>& tops = m_RegionScratch;
        tops.assign(m_RegionCount, 0u);
        // Walk the window in window-relative order, wrapping the toroidal column index by hand.
        int wz = math::FloorMod(m_WindowMin.z, s.z);
        for (int cz = 0; cz < s.z; ++cz) {
            int wx = math::FloorMod(m_WindowMin.x, s.x);
            uint32_t* row = tops.data() + static_cast<size_t>(cz / 4) * m_RegionsX;
            const uint32_t* columnTops = m_Grid.data() + m_CellCount + static_cast<size_t>(wz) * static_cast<size_t>(s.x);
            for (int cx = 0; cx < s.x; ++cx) {
                row[cx / 4] = std::max(row[cx / 4], columnTops[wx]);
                if (++wx == s.x) wx = 0;
            }
            if (++wz == s.z) wz = 0;
        }
        for (uint32_t r = 0; r < m_RegionCount; ++r) {
            if (m_Grid[base + r] == tops[r]) continue;
            m_Grid[base + r] = tops[r];
            m_GridPages[(base + r) / kGridPageWords] = ++m_GridRevision;
        }
    }

    void World::RefreshContentTop() {
        if (m_RegionTopsStale) RebuildRegionTops();
        if (!m_ContentTopStale) return;
        m_ContentTopStale = false;
        uint32_t top = 0;
        for (uint32_t ci = 0; ci < m_ColumnCount; ++ci) top = std::max(top, m_Grid[m_CellCount + ci]);
        uint32_t& contentTop = m_Grid[m_CellCount + m_ColumnCount];
        if (contentTop != top) {
            contentTop = top;
            m_GridPages[(m_CellCount + m_ColumnCount) / kGridPageWords] = ++m_GridRevision;
        }
    }

    ChunkChange& World::QueueChange(Cell& cell, bool shape) {
        BumpRevision(cell);       // every caller changed the chunk's content (an edit, a load)
        if (!cell.changeQueued) {
            cell.changeQueued = true;
            cell.changeIndex = static_cast<uint32_t>(m_ChangedChunks.size());
            ChunkChange change;
            change.coord = cell.coord;
            change.shapeChanged = shape;
            m_ChangedChunks.push_back(change);
            return m_ChangedChunks.back();
        }
        ChunkChange& change = m_ChangedChunks[cell.changeIndex];
        change.shapeChanged = change.shapeChanged || shape;
        return change;
    }

    std::vector<ChunkChange> World::TakeChunkChanges() {
        std::vector<ChunkChange> out = std::move(m_ChangedChunks);
        m_ChangedChunks.clear();
        for (const ChunkChange& change : out) {
            if (Cell* cell = FindCell(change.coord)) cell->changeQueued = false;
        }
        return out;
    }

    const std::array<uint8_t, 256>& World::ShapeClasses() {
        const uint64_t revision = m_Materials != nullptr ? m_Materials->Revision() : 1;
        if (revision == m_ShapeClassRevision) return m_ShapeClass;
        m_ShapeClassRevision = revision;
        for (uint32_t id = 0; id < 256; ++id) {
            uint8_t c = id == 0 ? 0 : 1;
            if (id != 0 && m_Materials != nullptr && (m_Materials->Get(kWorldPalette, static_cast<uint8_t>(id)).flags & kNonColliderFlags) != 0) c = 2;
            m_ShapeClass[id] = c;
        }
        return m_ShapeClass;
    }

    std::vector<glm::ivec3> World::TakeEditedChunks() {
        std::vector<glm::ivec3> out = std::move(m_EditedChunks);
        m_EditedChunks.clear();
        return out;
    }

    std::vector<glm::ivec3> World::TakeChangedChunks() {
        std::vector<glm::ivec3> out;
        for (const ChunkChange& change : TakeChunkChanges()) out.push_back(change.coord);
        return out;
    }

    // --- column pass ------------------------------------------------------------------------

    bool World::IsSurfaceRow(const Column& column, int row) const {
        return !column.cls.known || (row >= column.cls.surfaceFrom && row < column.cls.emptyFrom);
    }

    bool World::IsBandRow(const Column& column, int row) const {
        return !column.cls.known || (row >= column.cls.solidBelow && row < column.cls.emptyFrom);
    }

    void World::PlaceCell(uint32_t index, Column& column, int rel) {
        Cell& cell = m_Cells[index];
        if (cell.state != CellState::Unloaded) return;
        const int row = cell.coord.y;
        const bool saved = TestBit(column.savedRows, rel);
        if (!saved && !IsBandRow(column, row)) {
            if (row >= column.cls.emptyFrom) {
                cell.state = CellState::Empty;
            } else {
                cell.state = CellState::Uniform;
                cell.uniformId = column.cls.solidId;
                if (cell.uniformId == 0) cell.state = CellState::Empty;
            }
            cell.modified = false;
            BumpRevision(cell);
            SetGrid(index, cell);
            if (cell.state == CellState::Uniform) QueueChange(cell);
            if (m_Config.reportLoads) m_LoadedChunks.push_back(cell.coord);
            return;
        }
        if (saved || IsSurfaceRow(column, row)) {
            SetBit(column.surfaceRows, rel);
            ++column.surfaceLeft;
        } else {
            SetBit(column.caveRows, rel);
        }
        if (m_FullHeight) WriteGrid(index, kGridPending);
    }

    void World::ApplyColumnClass(uint32_t ci) {
        const glm::ivec3 s = m_Config.windowSize;
        Column& column = m_Columns[ci];
        column.classState = Column::Class::Known;
        for (int wy = 0; wy < s.y; ++wy) {
            const uint32_t index = ci * static_cast<uint32_t>(s.y) + static_cast<uint32_t>(wy);
            PlaceCell(index, column, m_Cells[index].coord.y - m_WindowMin.y);
        }
        if (m_Pack) RecheckNeighbours(ci);
    }

    void World::RecheckNeighbours(uint32_t ci) {
        // A neighbour's chunks may have been packed while this column was outside the window or
        // not classified (Visible ignores both): judge them again now that it is known. Only
        // this column's arrival can change their answer, so unknown columns are left to their
        // own arrival (Visible(cell, false)).
        const Column& column = m_Columns[ci];
        const glm::ivec3 s = m_Config.windowSize;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                const Column* n = FindColumn(column.coord.x + dx, column.coord.y + dz);
                if (!n || n->classState != Column::Class::Known) continue;
                const uint32_t ni = ColumnIndex(n->coord.x, n->coord.y);
                for (int wy = 0; wy < s.y; ++wy) {
                    const uint32_t index = ni * static_cast<uint32_t>(s.y) + static_cast<uint32_t>(wy);
                    const Cell& cell = m_Cells[index];
                    if (cell.state == CellState::Packed && Visible(cell, false)) m_Reveal.push_back(index);
                }
            }
    }

    bool World::NeighboursKnown(const Column& column, bool submitMissing) {
        bool known = true;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                Column* n = FindColumn(column.coord.x + dx, column.coord.y + dz);
                if (!n || n->classState == Column::Class::Known) continue;     // outside the window: ignored
                known = false;
                if (submitMissing && n->classState == Column::Class::None) SubmitColumnJob(ColumnIndex(n->coord.x, n->coord.y));
            }
        return known;
    }

    bool World::Visible(const Cell& cell, bool unknownVisible) const {
        // Geometric rule (view-distance.md, stage 1 as built): a chunk is visible if its row
        // reaches down to the lowest surface voxel of some column of its 3 x 3 neighbourhood (the
        // row above ClassifyColumn's surfaceFrom). Below that it is buried: no ray reaches it but
        // through a cave, and a cave mouth farther than nearRadius looks plugged. (The design's
        // extra chunk under the surface held about 600 more slots at the spawn and no ray needs it.)
        if (cell.modified || cell.fromSave) return true;
        // Server terrain mode (networking stage 7b): the server decided. This world's column classes
        // come from a generator that differs from the server's and are not asked.
        if (cell.serverVisibility == kServerShown) return true;
        if (cell.serverVisibility == kServerHidden) return FaceNeighbourEdited(cell.coord);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const Column* n = FindColumn(cell.coord.x + dx, cell.coord.z + dz);
                if (!n) continue;          // outside the window: RecheckNeighbours runs when it is classified
                if (n->classState != Column::Class::Known) {
                    if (unknownVisible) return true;
                    continue;
                }
                if (!n->cls.known) return true;
                if (cell.coord.y >= n->cls.surfaceFrom + 1) return true;
            }
        // A ray through an edit (a crater, a tunnel, a saved one) reaches the face behind it.
        return FaceNeighbourEdited(cell.coord);
    }

    bool World::FaceNeighbourEdited(const glm::ivec3& chunk) const {
        static constexpr glm::ivec3 kFaces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const glm::ivec3& f : kFaces) {
            const Cell* n = FindCell(chunk + f);
            if (n && (n->modified || n->fromSave)) return true;
        }
        return false;
    }

    void World::ColumnJob(const std::shared_ptr<Shared>& shared, uint32_t columnIndex, glm::ivec2 coord, uint32_t generation, int rowMin,
                          int rows) {
        Shared::ColumnResult result;
        result.columnIndex = columnIndex;
        result.coord = coord;
        result.generation = generation;
        result.cls = shared->generator->ClassifyColumn(coord.x, coord.y);
        result.rowMin = rowMin;
        result.rowMax = rowMin + rows;
        if (result.cls.known) {
            // Rows that would be installed without a job: a saved edit there still needs one.
            for (int row = rowMin; row < rowMin + rows; ++row) {
                if (row >= result.cls.solidBelow && row < result.cls.emptyFrom) continue;
                if (shared->regions.Contains(glm::ivec3(coord.x, row, coord.y))) result.savedRows.push_back(row);
            }
        }
        {
            std::scoped_lock lock(shared->mutex);
            shared->columnResults.push_back(std::move(result));
        }
        shared->inFlight.fetch_sub(1);
    }

    void World::LoadJob(const std::shared_ptr<Shared>& shared, uint32_t cellIndex, glm::ivec3 coord, uint32_t generation, bool pack) {
        // One scratch chunk per worker thread: Empty / Uniform results never allocate.
        thread_local std::unique_ptr<Chunk> scratch;
        if (!scratch) scratch = std::make_unique<Chunk>();

        Shared::Result result;
        result.cellIndex = cellIndex;
        result.coord = coord;
        result.generation = generation;

        bool loaded = false;
        if (auto bytes = shared->regions.Load(coord)) {
            scratch->Clear();
            loaded = DecodeChunk(*bytes, result.kind, result.uniformId, *scratch);
            if (!loaded) Log::Warn("Saved chunk ({}, {}, {}) is corrupt; regenerating", coord.x, coord.y, coord.z);
            result.fromSave = loaded;
        }
        if (!loaded) {
            scratch->Clear();
            const auto generateStart = std::chrono::steady_clock::now();
            const ChunkContent content = shared->generator->Generate(coord, *scratch);
            shared->generateNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - generateStart).count()),
                                         std::memory_order_relaxed);
            result.kind = content.kind;
            result.uniformId = content.uniformId;
            if (result.kind == ChunkKind::Dense) {
                scratch->RebuildOccupancy();
                // A generator may report Dense for a chunk that turned out trivial: demote it.
                if (scratch->IsEmpty()) {
                    result.kind = ChunkKind::Empty;
                } else if (const int uniform = scratch->UniformId(); uniform > 0) {
                    result.kind = ChunkKind::Uniform;
                    result.uniformId = static_cast<uint8_t>(uniform);
                }
            }
        }
        if (result.kind == ChunkKind::Dense) {
            result.chunk = std::make_unique<Chunk>(*scratch);
            if (pack && !result.fromSave) {
                // The bytes a Packed cell keeps (maximal runs), and the id its grid word shows.
                result.packed = EncodeChunk(ChunkKind::Dense, 0, scratch.get());
                result.packedId = MostFrequentSolidId(result.packed);
            }
        }
        if (result.kind == ChunkKind::Uniform && result.uniformId == 0) result.kind = ChunkKind::Empty;
        if (!loaded) {
            shared->generated.fetch_add(1, std::memory_order_relaxed);
            if (result.kind == ChunkKind::Dense) shared->generatedDense.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::scoped_lock lock(shared->mutex);
            shared->results.push_back(std::move(result));
        }
        shared->inFlight.fetch_sub(1);
    }

    bool World::SubmitColumnJob(uint32_t ci) {
        if (m_Shared->inFlight.load() >= m_MaxJobs) return false;
        Column& column = m_Columns[ci];
        column.classState = Column::Class::Classifying;
        m_Shared->inFlight.fetch_add(1);
        m_Jobs.Submit([shared = m_Shared, ci, coord = column.coord, generation = column.generation, rowMin = m_WindowMin.y,
                       rows = m_Config.windowSize.y] { ColumnJob(shared, ci, coord, generation, rowMin, rows); });
        return true;
    }

    bool World::SubmitCellJob(uint32_t index) {
        if (m_Shared->inFlight.load() >= m_MaxJobs) return false;
        Cell& cell = m_Cells[index];
        cell.state = CellState::Loading;
        m_Shared->inFlight.fetch_add(1);
        m_Jobs.Submit([shared = m_Shared, index, coord = cell.coord, generation = cell.generation, pack = m_Pack] {
            LoadJob(shared, index, coord, generation, pack);
        });
        return true;
    }

    bool World::SubmitColumnRows(uint32_t ci, bool caves, bool nearOnly) {
        Column& column = m_Columns[ci];
        if (column.classState == Column::Class::None) return SubmitColumnJob(ci);
        if (column.classState == Column::Class::Classifying) return true;
        if (!AnyBit(column.surfaceRows) && !AnyBit(column.caveRows)) return true;
        // The pack decision looks at the neighbouring columns: classify them first.
        if (m_Pack && !NeighboursKnown(column, true)) return m_Shared->inFlight.load() < m_MaxJobs;

        const glm::ivec3 s = m_Config.windowSize;
        const auto submitRows = [&](uint64_t (&rows)[2]) {
            if (!AnyBit(rows)) return true;
            // Surface first from the top down; a column with no class from the focus row outwards.
            int order[kMaxRows];
            int count = 0;
            for (int rel = s.y - 1; rel >= 0; --rel) {
                if (TestBit(rows, rel)) order[count++] = rel;
            }
            if (!column.cls.known) {
                const int focusRel = m_FocusChunk.y - m_WindowMin.y;
                std::stable_sort(order, order + count, [focusRel](int a, int b) { return std::abs(a - focusRel) < std::abs(b - focusRel); });
            }
            for (int i = 0; i < count; ++i) {
                const int rel = order[i];
                const uint32_t index = ci * static_cast<uint32_t>(s.y) + static_cast<uint32_t>(math::FloorMod(m_WindowMin.y + rel, s.y));
                if (nearOnly && DistanceToFocus(m_Cells[index].coord) > m_Config.nearRadius) continue;
                if (m_Cells[index].state != CellState::Unloaded) {
                    ClearBit(rows, rel);
                    continue;
                }
                if (!SubmitCellJob(index)) return false;
                ClearBit(rows, rel);
            }
            return true;
        };
        if (!submitRows(column.surfaceRows)) return false;
        if (caves || nearOnly) return submitRows(column.caveRows);
        return true;
    }

    void World::SubmitJobs() {
        if (!m_HasWindow) return;
        // Columns whose surface is not loaded: the backlog. While it is large, cave rows wait and
        // the surface of the whole window comes first (generation is about 4 times slower than
        // the surface band of a fast flight needs, view-distance.md "Stage 1 as built"). Reported:
        // the ones inside 0.85 x the view radius.
        const double chunkMetres = static_cast<double>(kChunkSize) * m_Config.voxelSize;
        const size_t fillBucket = static_cast<size_t>(0.85 * ViewRadius() / chunkMetres);
        uint32_t inside = 0, backlog = 0, unclassified = 0;
        for (const uint32_t ci : m_BucketOrder) {
            const Column& column = m_Columns[ci];
            const bool known = column.classState == Column::Class::Known;
            if (!known) ++unclassified;
            if (known && column.surfaceLeft == 0) continue;
            ++backlog;
            if (column.bucket <= fillBucket) ++inside;
        }
        m_Counters.unfilledColumns = inside;
        m_Counters.columnsPending = unclassified;
        if (m_Shared->inFlight.load() >= m_MaxJobs) return;

        // 1. Everything within nearRadius of the focus (colliders, blasts, the player's view).
        const size_t nearBucket = static_cast<size_t>(m_Config.nearRadius / chunkMetres) + 1;
        for (const uint32_t ci : m_BucketOrder) {
            if (m_Columns[ci].bucket > nearBucket) break;
            if (!SubmitColumnRows(ci, false, true)) return;
        }
        // 1b. Boxes an edit asked for (RequestLoad): a blast's island search needs all of them.
        if (!SubmitRequestedBoxes()) return;
        // 2. Columns by horizontal distance: classification and the surface band; the cave rows
        //    of a column right after its surface only while the backlog is small. With the
        //    default (0) every surface of the window comes before any cave outside the near radius.
        const bool caves = backlog <= m_Config.surfaceFirstBacklog;
        for (const uint32_t ci : m_BucketOrder) {
            if (!SubmitColumnRows(ci, caves, false)) return;
        }
    }

    void World::RequestLoad(const glm::ivec3& minChunk, const glm::ivec3& maxChunk) {
        if (glm::any(glm::lessThan(maxChunk, minChunk))) return;
        constexpr size_t kMaxBoxes = 16;
        if (m_Requested.size() == kMaxBoxes) m_Requested.erase(m_Requested.begin());
        m_Requested.push_back({minChunk, maxChunk});
    }

    bool World::SubmitRequestedBoxes() {
        const glm::ivec3 s = m_Config.windowSize;
        for (size_t b = 0; b < m_Requested.size();) {
            // Clipped to the window (columns outside it are never loaded).
            const glm::ivec3 lo = glm::max(m_Requested[b].min, m_WindowMin);
            const glm::ivec3 hi = glm::min(m_Requested[b].max, m_WindowMin + s - glm::ivec3(1));
            bool done = true;
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) {
                    Column* column = FindColumn(x, z);
                    if (!column) continue;
                    const uint32_t ci = ColumnIndex(x, z);
                    if (column->classState != Column::Class::Known) {
                        done = false;
                        if (column->classState == Column::Class::None && !SubmitColumnJob(ci)) return false;
                        continue;
                    }
                    // The pack decision reads the neighbouring columns: classify them first.
                    if (m_Pack && !NeighboursKnown(*column, true)) {
                        done = false;
                        continue;
                    }
                    for (int y = lo.y; y <= hi.y; ++y) {
                        const Cell* found = FindCell(glm::ivec3(x, y, z));
                        if (!found) continue;
                        const Cell& cell = *found;
                        const uint32_t index = CellIndex(cell.coord);
                        if (cell.state == CellState::Loading) done = false;
                        if (cell.state != CellState::Unloaded) continue;
                        done = false;
                        if (!SubmitCellJob(index)) return false;
                        const int rel = y - m_WindowMin.y;
                        ClearBit(column->surfaceRows, rel);
                        ClearBit(column->caveRows, rel);
                    }
                }
            if (done) m_Requested.erase(m_Requested.begin() + static_cast<std::ptrdiff_t>(b));
            else ++b;
        }
        return true;
    }

    void World::IntegrateResults(uint32_t budget) {
        std::vector<Shared::ColumnResult> columns;
        std::vector<Shared::Result> ready;
        {
            std::scoped_lock lock(m_Shared->mutex);
            columns.swap(m_Shared->columnResults);
            // The budget counts Dense results only: Empty and Uniform ones cost nothing to install.
            uint32_t dense = 0;
            size_t take = 0;
            for (; take < m_Shared->results.size(); ++take) {
                if (m_Shared->results[take].kind == ChunkKind::Dense) {
                    if (dense == budget) break;
                    ++dense;
                }
            }
            if (take == m_Shared->results.size()) {
                ready.swap(m_Shared->results);
            } else {
                ready.assign(std::make_move_iterator(m_Shared->results.begin()),
                             std::make_move_iterator(m_Shared->results.begin() + static_cast<std::ptrdiff_t>(take)));
                m_Shared->results.erase(m_Shared->results.begin(), m_Shared->results.begin() + static_cast<std::ptrdiff_t>(take));
            }
        }

        for (Shared::ColumnResult& r : columns) {
            Column& column = m_Columns[r.columnIndex];
            if (!column.assigned || column.coord != r.coord || column.generation != r.generation ||
                column.classState != Column::Class::Classifying)
                continue;
            column.cls = r.cls;
            column.savedRows[0] = column.savedRows[1] = 0;
            for (int row : r.savedRows) {
                const int rel = row - m_WindowMin.y;
                if (rel >= 0 && rel < m_Config.windowSize.y) SetBit(column.savedRows, rel);
            }
            if (!m_FullHeight && r.cls.known) {
                // A camera-centred window may have moved since the job looked for saved chunks:
                // rows it did not check are generated (the job loads a saved chunk if there is one).
                for (int rel = 0; rel < m_Config.windowSize.y; ++rel) {
                    const int row = m_WindowMin.y + rel;
                    if (row < r.rowMin || row >= r.rowMax) SetBit(column.savedRows, rel);
                }
            }
            ApplyColumnClass(r.columnIndex);
        }

        bool storeFull = false;
        const double packBeyond = m_Config.nearRadius + 2.0 * NearSlack();
        for (Shared::Result& r : ready) {
            Cell& cell = m_Cells[r.cellIndex];
            // The window may have moved on while the job ran.
            if (!cell.assigned || cell.coord != r.coord || cell.generation != r.generation || cell.state != CellState::Loading) continue;

            cell.modified = false;
            cell.uniformId = 0;
            cell.packedId = 0;
            cell.slot = ChunkStore::kInvalidSlot;
            cell.fromSave = r.fromSave;
            cell.serverVisibility = 0;
            switch (r.kind) {
                case ChunkKind::Empty:
                    cell.state = CellState::Empty;
                    break;
                case ChunkKind::Uniform:
                    cell.state = CellState::Uniform;
                    cell.uniformId = r.uniformId;
                    break;
                case ChunkKind::Dense: {
                    const bool canPack = m_Pack && !r.packed.empty() && r.packedId != 0;
                    // Visible() also answers true next to an edit or a saved chunk (a crater wall).
                    const bool hidden = canPack && !Visible(cell);
                    if (hidden && DistanceToFocus(cell.coord) > packBeyond) {
                        cell.state = CellState::Packed;
                        cell.uniformId = r.packedId;
                        m_Counters.packedBytes += r.packed.size();
                        m_Packed[r.cellIndex] = std::move(r.packed);
                        break;
                    }
                    std::vector<uint8_t> rle = std::move(r.packed);
                    // (Adopt destroys the chunk when it fails: test for a free slot first.)
                    storeFull = storeFull || m_Store.UsedCount() >= m_Store.Capacity();
                    const uint32_t slot = storeFull ? ChunkStore::kInvalidSlot : m_Store.Adopt(std::move(r.chunk));
                    if (slot != ChunkStore::kInvalidSlot) {
                        cell.state = CellState::Dense;
                        cell.slot = slot;
                        if (hidden) {
                            // Buried but near the focus: keep the bytes, so that Repack can
                            // pack it again without encoding once the focus has moved on.
                            cell.packedId = r.packedId;
                            m_Counters.packedBytes += rle.size();
                            m_Packed[r.cellIndex] = std::move(rle);
                            m_Resident.push_back(r.cellIndex);
                        }
                        break;
                    }
                    storeFull = true;
                    ++m_Counters.storeFullLoads;
                    static bool warned = false;
                    if (!warned) {
                        Log::Warn("ChunkStore is full ({} chunks): chunks that need a slot are kept packed and drawn as rock. "
                                  "Raise the chunk capacity or shrink WorldConfig::windowSize.", m_Store.Capacity());
                        warned = true;
                    }
                    // Never install it as Empty: a saved chunk would vanish, and the next edit
                    // would overwrite the save with an empty one. Packed keeps every voxel.
                    if (rle.empty()) rle = EncodeChunk(ChunkKind::Dense, 0, r.chunk.get());
                    cell.state = CellState::Packed;
                    cell.uniformId = canPack ? r.packedId : MostFrequentSolidId(rle);
                    m_Counters.packedBytes += rle.size();
                    m_Packed[r.cellIndex] = std::move(rle);
                    // Shown as rock although it may be visible: ProcessReveals decodes it if it is
                    // visible and a slot is free (otherwise counted in storeFullDecodes).
                    m_Reveal.push_back(r.cellIndex);
                    break;
                }
            }
            BumpRevision(cell);
            SetGrid(r.cellIndex, cell);
            if (cell.state == CellState::Dense || cell.state == CellState::Uniform || cell.state == CellState::Packed) QueueChange(cell);
            if (m_Config.reportLoads) m_LoadedChunks.push_back(cell.coord);
            // An edit of an earlier visit (a crater, a tunnel) exposes its face neighbours,
            // whichever of them loaded first.
            if (cell.fromSave && m_Pack) PromoteFaceNeighbours(cell.coord);

            const glm::ivec3 s = m_Config.windowSize;
            Column& column = m_Columns[r.cellIndex / static_cast<uint32_t>(s.y)];
            const int rel = cell.coord.y - m_WindowMin.y;
            if (column.classState == Column::Class::Known && column.surfaceLeft > 0 &&
                (TestBit(column.savedRows, rel) || IsSurfaceRow(column, cell.coord.y)))
                --column.surfaceLeft;
        }
    }

    // --- Packed chunks ------------------------------------------------------------------------

    bool World::Unpack(uint32_t index) {
        Cell& cell = m_Cells[index];
        if (cell.state != CellState::Packed) return cell.state == CellState::Dense;
        const uint32_t slot = m_Store.Alloc();
        if (slot == ChunkStore::kInvalidSlot) return false;
        const auto start = std::chrono::steady_clock::now();
        ChunkKind kind = ChunkKind::Empty;
        uint8_t id = 0;
        const bool ok = DecodeChunk(m_Packed[index], kind, id, m_Store.Get(slot));
        Verify(ok && kind == ChunkKind::Dense, "World: a Packed chunk does not decode");
        m_Counters.decodeMs += Seconds(start) * 1000.0;
        ++m_Counters.decodes;
        cell.state = CellState::Dense;
        cell.slot = slot;
        cell.packedId = cell.uniformId;
        cell.uniformId = 0;
        if (cell.fromSave || !m_Pack) {
            DropKeptBytes(index);       // a saved chunk is never packed again (it is an edit)
        } else {
            m_Resident.push_back(index);    // keeps its bytes: Repack may pack it again (an edit drops them)
        }
        SetGrid(index, cell);
        return true;
    }

    void World::ProcessReveals() {
        if (m_Reveal.empty()) return;
        size_t i = 0;
        for (; i < m_Reveal.size() && m_DecodeBudget > 0; ++i) {
            const uint32_t index = m_Reveal[i];
            // Decoded or unloaded since, or re-targeted to a chunk that is hidden.
            if (m_Cells[index].state != CellState::Packed || !Visible(m_Cells[index])) continue;
            if (!Unpack(index)) {
                ++m_Counters.storeFullDecodes;
                continue;
            }
            ++m_Counters.reveals;
            --m_DecodeBudget;
        }
        m_Reveal.erase(m_Reveal.begin(), m_Reveal.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void World::Repack() {
        // Resident buried chunks that the focus left behind give their slot back (a session that
        // stays in one area would otherwise fill the store with chunks PromoteNear decoded).
        if (!m_Pack || m_Resident.empty()) return;
        const double beyond = m_Config.nearRadius + std::max(static_cast<double>(m_Config.repackMargin), 2.0 * NearSlack());
        size_t examined = 0;
        uint32_t budget = std::max(m_Config.maxDecodesPerUpdate, 1u);
        const size_t limit = std::min<size_t>(128, m_Resident.size());     // a round-robin share per Update
        while (!m_Resident.empty() && examined < limit && budget > 0) {
            if (m_ResidentCursor >= m_Resident.size()) m_ResidentCursor = 0;
            const uint32_t index = m_Resident[m_ResidentCursor];
            Cell& cell = m_Cells[index];
            ++examined;
            // No longer a candidate: unloaded, edited or packed (its bytes went with it).
            if (cell.state != CellState::Dense || m_Packed[index].empty() || cell.modified || cell.fromSave) {
                if (cell.state == CellState::Dense && !m_Packed[index].empty() && (cell.modified || cell.fromSave)) DropKeptBytes(index);
                m_Resident[m_ResidentCursor] = m_Resident.back();
                m_Resident.pop_back();
                continue;
            }
            if (DistanceToFocus(cell.coord) <= beyond || Visible(cell)) {
                ++m_ResidentCursor;
                continue;
            }
            // Pack it again: the slot goes, the bytes it kept become the cell's content.
            m_Garbage.chunks.push_back(m_Store.Release(cell.slot));
            cell.slot = ChunkStore::kInvalidSlot;
            cell.state = CellState::Packed;
            cell.uniformId = cell.packedId;
            SetGrid(index, cell);
            ++m_Counters.repacks;
            --budget;
            m_Resident[m_ResidentCursor] = m_Resident.back();
            m_Resident.pop_back();
        }
    }

    void World::PromoteNear() {
        if (!m_Pack || !m_HasWindow) m_NearPromotePending = false;
        if (!m_NearPromotePending) return;
        m_NearScanFocus = m_Focus;
        const double size = static_cast<double>(kChunkSize) * m_Config.voxelSize;
        const double r = m_Config.nearRadius + NearSlack();     // see NearSlack()
        const glm::ivec3 lo = glm::ivec3(glm::floor((m_Focus - glm::dvec3(r)) / size));
        const glm::ivec3 hi = glm::ivec3(glm::floor((m_Focus + glm::dvec3(r)) / size));
        uint32_t& budget = m_DecodeBudget;
        bool leftover = false;
        for (int y = lo.y; y <= hi.y && !leftover; ++y)
            for (int z = lo.z; z <= hi.z && !leftover; ++z)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const glm::ivec3 c(x, y, z);
                    const Cell* cell = FindCell(c);
                    if (!cell || cell->state != CellState::Packed || DistanceToFocus(c) > r) continue;
                    if (budget == 0) {
                        leftover = true;
                        break;
                    }
                    if (!Unpack(CellIndex(c))) {          // store full: try again when the focus moves
                        ++m_Counters.storeFullDecodes;
                        break;
                    }
                    --budget;
                }
        m_NearPromotePending = leftover;
    }

    void World::PromoteFaceNeighbours(const glm::ivec3& chunk) {
        static constexpr glm::ivec3 kFaces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const glm::ivec3& f : kFaces) {
            const glm::ivec3 c = chunk + f;
            const Cell* cell = FindCell(c);
            if (cell && cell->state == CellState::Packed && !Unpack(CellIndex(c))) {
                ++m_Counters.storeFullDecodes;
                Log::Warn("ChunkStore is full: chunk ({}, {}, {}) next to an edit stays packed", c.x, c.y, c.z);
            }
        }
    }

    uint32_t World::EnsureResident(const glm::ivec3& minChunk, const glm::ivec3& maxChunk) {
        uint32_t failed = 0;
        for (int y = minChunk.y; y <= maxChunk.y; ++y)
            for (int z = minChunk.z; z <= maxChunk.z; ++z)
                for (int x = minChunk.x; x <= maxChunk.x; ++x) {
                    const glm::ivec3 c(x, y, z);
                    const Cell* cell = FindCell(c);
                    if (cell && cell->state == CellState::Packed && !Unpack(CellIndex(c))) ++failed;
                }
        return failed;
    }

    std::span<const uint8_t> World::PackedData(const glm::ivec3& chunk) const {
        const Cell* cell = FindCell(chunk);
        if (!cell || cell->state != CellState::Packed) return {};
        return m_Packed[CellIndex(chunk)];
    }

    WorldSaveResult World::Save() {
        for (Cell& cell : m_Cells) {
            if (!cell.assigned || !cell.modified) continue;
            if (cell.state == CellState::Dense || cell.state == CellState::Uniform || cell.state == CellState::Empty) PersistCell(cell);
        }
        WorldSaveResult result;
        result.files = static_cast<uint32_t>(m_Shared->regions.Flush());
        result.unwrittenRegions = static_cast<uint32_t>(m_Shared->regions.UnwrittenRegionCount());
        if (result.files > 0) Log::Info("World saved ({} region file(s))", result.files);
        return result;
    }

    WorldStats World::Stats() const {
        WorldStats stats = m_Counters;
        for (const Cell& cell : m_Cells) {
            switch (cell.state) {
                case CellState::Dense:   ++stats.denseChunks; break;
                case CellState::Uniform: ++stats.uniformChunks; break;
                case CellState::Empty:   ++stats.emptyChunks; break;
                case CellState::Packed:  ++stats.packedChunks; break;
                default:                 ++stats.pendingChunks; break;
            }
        }
        stats.jobsInFlight = m_Shared->inFlight.load();
        stats.generatedChunks = m_Shared->generated.load(std::memory_order_relaxed);
        stats.generatedDense = m_Shared->generatedDense.load(std::memory_order_relaxed);
        stats.generateMs = static_cast<double>(m_Shared->generateNs.load(std::memory_order_relaxed)) * 1.0e-6;
        stats.refusedRegions = static_cast<uint32_t>(m_Shared->regions.RefusedRegionCount());
        stats.unwrittenRegions = static_cast<uint32_t>(m_Shared->regions.UnwrittenRegionCount());
        return stats;
    }

    // --- queries ------------------------------------------------------------------------------

    CellState World::StateOf(const glm::ivec3& chunk) const {
        const Cell* cell = FindCell(chunk);
        return cell ? cell->state : CellState::Unloaded;
    }

    uint64_t World::ChunkRevision(const glm::ivec3& chunk) const {
        const Cell* cell = FindCell(chunk);
        if (!cell) return 0;
        const CellState state = cell->state;
        const bool loaded = state == CellState::Empty || state == CellState::Uniform || state == CellState::Dense || state == CellState::Packed;
        return loaded ? cell->revision : 0;
    }

    bool World::IsChunkLoaded(const glm::ivec3& chunk) const {
        const CellState state = StateOf(chunk);
        return state == CellState::Empty || state == CellState::Uniform || state == CellState::Dense || state == CellState::Packed;
    }

    uint8_t World::GetVoxel(const glm::ivec3& v) const {
        const Cell* cell = FindCell(VoxelToChunk(v));
        if (!cell) return 0;
        const glm::ivec3 l = VoxelToLocal(v);
        if (cell->state == CellState::Dense) return m_Store.Get(cell->slot).Get(l.x, l.y, l.z);
        if (cell->state == CellState::Packed) {
            // Walk the runs to the voxel's block-ordered index.
            const std::vector<uint8_t>& bytes = m_Packed[CellIndex(cell->coord)];
            const int target = static_cast<int>(Chunk::VoxelIndex(l.x, l.y, l.z));
            int i = 0;
            for (size_t p = 1; p + 2 < bytes.size(); p += 3) {
                i += bytes[p + 1] | (bytes[p + 2] << 8);
                if (target < i) return bytes[p];
            }
            return 0;
        }
        return cell->state == CellState::Uniform ? cell->uniformId : uint8_t{0};
    }

    const Chunk* World::GetChunk(const glm::ivec3& chunk, uint8_t* uniformId) const {
        if (uniformId) *uniformId = 0;
        const Cell* cell = FindCell(chunk);
        if (!cell) return nullptr;
        if (cell->state == CellState::Dense) return &m_Store.Get(cell->slot);
        if (cell->state == CellState::Uniform && uniformId) *uniformId = cell->uniformId;
        return nullptr;
    }

    bool World::ReadChunk(const glm::ivec3& chunk, Chunk& out) const {
        const Cell* cell = FindCell(chunk);
        if (!cell) return false;
        switch (cell->state) {
            case CellState::Dense: out = m_Store.Get(cell->slot); return true;
            case CellState::Uniform: out.Fill(cell->uniformId); return true;
            case CellState::Empty: out.Clear(); return true;
            case CellState::Packed: {
                ChunkKind kind = ChunkKind::Empty;
                uint8_t id = 0;
                return DecodeChunk(m_Packed[CellIndex(chunk)], kind, id, out);
            }
            default: return false;
        }
    }

    TraceScene World::MakeTraceScene() const {
        TraceScene scene;
        scene.store = &m_Store;
        scene.grid = m_Grid.data();
        scene.gridSize = m_Config.windowSize;
        scene.gridMinChunk = m_WindowMin;
        scene.gridOrigin = glm::vec3(0.0f);
        scene.voxelSize = m_Config.voxelSize;
        scene.columnTops = m_Grid.data() + m_CellCount;
        scene.contentTop = m_Grid[m_CellCount + m_ColumnCount];
        scene.regionTops = m_Grid.data() + m_CellCount + m_ColumnCount + 1;
        return scene;
    }

    WorldRayHit World::Raycast(const glm::dvec3& origin, const glm::vec3& direction, float maxDistance) const {
        WorldRayHit out;
        if (!m_HasWindow) return out;
        const float len = glm::length(direction);
        if (len <= 0.0f) return out;
        const glm::vec3 dir = direction / len;

        // Trace relative to the window origin: full float precision no matter how far from
        // the world origin the player has travelled.
        const TraceScene scene = MakeTraceScene();
        const glm::vec3 local = glm::vec3(origin - WindowOrigin());
        const RayHit hit = TraceWorld(scene, local, dir, 0.0f, maxDistance);
        // Material 0: the ray stopped at a chunk that is not loaded yet (pending); nothing was hit.
        if (!hit.hit || hit.material == 0) return out;

        out.hit = true;
        out.distance = hit.t;
        out.position = origin + glm::dvec3(dir) * static_cast<double>(hit.t);
        out.normal = hit.normal;
        out.voxel = hit.voxel;
        out.material = hit.material;
        return out;
    }

    // --- editing ------------------------------------------------------------------------------

    bool World::Materialize(uint32_t index) {
        Cell& cell = m_Cells[index];
        if (cell.state == CellState::Dense) return true;
        if (cell.state == CellState::Packed) {
            if (Unpack(index)) return true;
            Log::Warn("ChunkStore is full: cannot edit chunk ({}, {}, {})", cell.coord.x, cell.coord.y, cell.coord.z);
            return false;
        }
        if (cell.state != CellState::Empty && cell.state != CellState::Uniform) return false;
        const uint32_t slot = m_Store.Alloc();
        if (slot == ChunkStore::kInvalidSlot) {
            Log::Warn("ChunkStore is full: cannot edit chunk ({}, {}, {})", cell.coord.x, cell.coord.y, cell.coord.z);
            return false;
        }
        if (cell.state == CellState::Uniform) m_Store.Get(slot).Fill(cell.uniformId);
        cell.slot = slot;
        cell.state = CellState::Dense;
        cell.uniformId = 0;
        SetGrid(index, cell);
        return true;
    }

    template <typename Fn, typename Touches, typename Rows>
    uint32_t World::EditRegion(const glm::ivec3& min, const glm::ivec3& max, Fn&& decide, Touches&& touches,
                               std::vector<VoxelEdit>* edits, EditReport* report, const ChunkClip* clip, Rows rows) {
        if (max.x <= min.x || max.y <= min.y || max.z <= min.z) return 0;

        // Build ceiling (full-height window): an edit that would put solid voxels above the top
        // of the range is refused whole. Above it everything is air, so the test is the edit's
        // own decision for an air voxel: integers only, the same on every machine.
        if (m_FullHeight) {
            const int ceiling = (m_WindowMin.y + m_Config.windowSize.y) * kChunkSize;
            if (max.y > ceiling) {
                const glm::ivec3 lo(min.x, std::max(min.y, ceiling), min.z);
                if (touches(lo, max)) {
                    for (int z = lo.z; z < max.z; ++z)
                        for (int y = lo.y; y < max.y; ++y) {
                            if (!rows(glm::ivec3(lo.x, y, z), max.x - lo.x)) continue;
                            for (int x = lo.x; x < max.x; ++x) {
                                if (decide(glm::ivec3(x, y, z), uint8_t{0}) > 0) {
                                    if (report) report->outOfBounds = true;
                                    return 0;
                                }
                            }
                        }
                }
            }
        }

        const std::array<uint8_t, 256>& shapeClass = ShapeClasses();
        uint32_t changedTotal = 0;
        glm::ivec3 cmin = VoxelToChunk(min);
        glm::ivec3 cmax = VoxelToChunk(max - glm::ivec3(1));
        // A clip restricts the chunks visited, never the decisions: the ceiling was decided on the
        // whole operation above, and each voxel decides from (voxel, old id) alone.
        if (clip) {
            cmin = glm::max(cmin, clip->min);
            cmax = glm::min(cmax, clip->max);
        }
        std::vector<glm::ivec3> changedChunks;

        for (int cy = cmin.y; cy <= cmax.y; ++cy)
            for (int cz = cmin.z; cz <= cmax.z; ++cz)
                for (int cx = cmin.x; cx <= cmax.x; ++cx) {
                    const glm::ivec3 coord(cx, cy, cz);
                    const glm::ivec3 origin = ChunkToVoxel(coord);
                    const glm::ivec3 lo = glm::max(min, origin) - origin;
                    const glm::ivec3 hi = glm::min(max, origin + glm::ivec3(kChunkSize)) - origin;

                    Cell* cell = FindCell(coord);
                    if (!cell || (cell->state != CellState::Dense && cell->state != CellState::Empty && cell->state != CellState::Uniform &&
                                  cell->state != CellState::Packed)) {
                        // Not loaded: outside the window, Unloaded or Loading.
                        if (report && touches(origin + lo, origin + hi)) report->skipped.push_back(coord);
                        continue;
                    }
                    const uint32_t index = CellIndex(coord);
                    uint32_t changed = 0;
                    bool failed = false;
                    glm::ivec3 changedMin(kChunkSize), changedMax(0);   // chunk-local box of the changed voxels
                    // Whether a voxel turned between air and solid (the occupancy pyramid changes: a whole upload), whether
                    // one changed its collider class (physics re-cooks), and the id blocks that changed.
                    bool occupancyChanged = false, shapeChanged = false;
                    BlockMask changedBlocks{};
                    // A Packed chunk is read from a decoded copy and made resident at its first
                    // change only, exactly like an Empty / Uniform one: the result is the same
                    // as if it had never been packed.
                    if (cell->state == CellState::Packed) {
                        if (!touches(origin + lo, origin + hi)) continue;
                        if (!m_Scratch) m_Scratch = std::make_unique<Chunk>();
                        ChunkKind kind = ChunkKind::Empty;
                        uint8_t id = 0;
                        Verify(DecodeChunk(m_Packed[index], kind, id, *m_Scratch), "World: a Packed chunk does not decode");
                    }

                    for (int z = lo.z; z < hi.z && !failed; ++z)
                        for (int y = lo.y; y < hi.y && !failed; ++y) {
                            if (!rows(origin + glm::ivec3(lo.x, y, z), hi.x - lo.x)) continue;
                            for (int x = lo.x; x < hi.x; ++x) {
                                uint8_t oldId;
                                if (cell->state == CellState::Dense) oldId = m_Store.Get(cell->slot).Get(x, y, z);
                                else if (cell->state == CellState::Packed) oldId = m_Scratch->Get(x, y, z);
                                else oldId = cell->state == CellState::Uniform ? cell->uniformId : uint8_t{0};

                                const glm::ivec3 voxel = origin + glm::ivec3(x, y, z);
                                const int newId = decide(voxel, oldId);
                                if (newId < 0 || newId == oldId) continue;

                                // A non-dense cell is materialised at its first change, so a
                                // failure leaves the chunk exactly as it was.
                                if (cell->state != CellState::Dense && !Materialize(index)) {
                                    failed = true;
                                    break;
                                }
                                m_Store.Get(cell->slot).Set(x, y, z, static_cast<uint8_t>(newId));
                                if (edits) edits->push_back({voxel, oldId, static_cast<uint8_t>(newId)});
                                occupancyChanged = occupancyChanged || (oldId != 0) != (newId != 0);
                                shapeChanged = shapeChanged || shapeClass[oldId] != shapeClass[static_cast<uint8_t>(newId)];
                                const uint32_t block = Chunk::BlockIndex(x, y, z);
                                changedBlocks[block >> 6] |= uint64_t{1} << (block & 63u);
                                changedMin = glm::min(changedMin, glm::ivec3(x, y, z));
                                changedMax = glm::max(changedMax, glm::ivec3(x + 1, y + 1, z + 1));
                                ++changed;
                            }
                        }

                    if (failed && report) report->unsynced.push_back(coord);
                    if (changed > 0) {
                        if (report) report->changed.push_back(coord);
                        // A chunk made Dense by this edit is whole-dirty already (its new slot): the blocks add nothing.
                        if (occupancyChanged) m_Store.MarkEdited(cell->slot);
                        else m_Store.MarkDirtyBlocks(cell->slot, changedBlocks);
                        cell->modified = true;
                        DropKeptBytes(index);     // the bytes a resident buried chunk kept are stale now
                        ChunkChange& change = QueueChange(*cell, shapeChanged);
                        if (m_Config.reportEdits) m_EditedChunks.push_back(coord);
                        const bool first = !change.Edited();
                        change.editMin = first ? origin + changedMin : glm::min(change.editMin, origin + changedMin);
                        change.editMax = first ? origin + changedMax : glm::max(change.editMax, origin + changedMax);
                        changedTotal += changed;
                        if (m_Pack) changedChunks.push_back(coord);
                    }
                }
        // A ray leaving a changed chunk enters a face neighbour first: those must show their
        // real voxels now, not the uniform stand-in of a Packed chunk.
        for (const glm::ivec3& coord : changedChunks) PromoteFaceNeighbours(coord);
        RefreshContentTop();
        return changedTotal;
    }

    // --- chunk sync (networking stage 7a) -----------------------------------------------------

    bool World::EncodeChunkAt(const glm::ivec3& chunk, std::vector<uint8_t>& out) const {
        out.clear();
        const Cell* cell = FindCell(chunk);
        if (!cell) return false;
        switch (cell->state) {
            case CellState::Packed: {
                const std::vector<uint8_t>& bytes = m_Packed[CellIndex(chunk)];
                out.assign(bytes.begin(), bytes.end());
                return true;
            }
            case CellState::Dense: {
                const Chunk& c = m_Store.Get(cell->slot);
                if (c.IsEmpty()) out = EncodeChunk(ChunkKind::Empty, 0, nullptr);
                else if (const int uniform = c.UniformId(); uniform > 0) out = EncodeChunk(ChunkKind::Uniform, static_cast<uint8_t>(uniform), nullptr);
                else out = EncodeChunk(ChunkKind::Dense, 0, &c);
                return true;
            }
            case CellState::Uniform: out = EncodeChunk(ChunkKind::Uniform, cell->uniformId, nullptr); return true;
            case CellState::Empty: out = EncodeChunk(ChunkKind::Empty, 0, nullptr); return true;
            default: return false;
        }
    }

    void World::SettleLoading(uint32_t index) {
        // Trap (networking stage 7a, notes/voxel.md): a cell that streaming still owes is counted in
        // its column (row bits, surfaceLeft) and may have a job in flight. Installing a chunk there
        // must take it out of both, or the job overwrites it, or LoadBlocking waits for ever.
        Cell& cell = m_Cells[index];
        ++cell.generation;            // a job in flight for it is stale now
        const glm::ivec3 s = m_Config.windowSize;
        Column& column = m_Columns[index / static_cast<uint32_t>(s.y)];
        if (column.classState != Column::Class::Known) return;   // PlaceCell skips a cell that is not Unloaded
        const int rel = cell.coord.y - m_WindowMin.y;
        const bool counted = cell.state == CellState::Loading ? (TestBit(column.savedRows, rel) || IsSurfaceRow(column, cell.coord.y))
                                                              : TestBit(column.surfaceRows, rel);
        ClearBit(column.surfaceRows, rel);
        ClearBit(column.caveRows, rel);
        if (counted && column.surfaceLeft > 0) --column.surfaceLeft;
    }

    SetChunkResult World::SetChunkEncoded(const glm::ivec3& chunk, std::span<const uint8_t> rle, InstallMode mode) {
        Cell* cell = FindCell(chunk);
        if (!cell) return SetChunkResult::OutsideWindow;
        if (!m_Scratch) m_Scratch = std::make_unique<Chunk>();
        ChunkKind kind = ChunkKind::Empty;
        uint8_t id = 0;
        if (!DecodeChunk(rle, kind, id, *m_Scratch)) return SetChunkResult::Malformed;
        if (kind == ChunkKind::Dense) {
            if (m_Scratch->IsEmpty()) {
                kind = ChunkKind::Empty;
            } else if (const int uniform = m_Scratch->UniformId(); uniform > 0) {
                kind = ChunkKind::Uniform;
                id = static_cast<uint8_t>(uniform);
            }
        }
        const uint32_t index = CellIndex(chunk);
        const bool wasLoaded = cell->state != CellState::Unloaded && cell->state != CellState::Loading;
        // Server terrain mode (networking stage 7b): a chunk the server holds packed is installed
        // packed - decided here, before any slot is taken - unless this world packs nothing or the
        // cell is an edit here (then it is shown, as an edit is on the server).
        const bool keepEdit = wasLoaded && cell->modified;
        // ... and unless a face neighbour is an edit here: the server shows such a chunk (an edit next
        // to it unpacked it), and nothing would reveal it after this install (review of stage 7b: the
        // neighbour's edit ran while this cell was still waiting for its data).
        if (mode == InstallMode::HiddenContent && (!m_Pack || keepEdit || FaceNeighbourEdited(chunk))) mode = InstallMode::Content;
        const bool pack = mode == InstallMode::HiddenContent && kind == ChunkKind::Dense;
        uint32_t slot = ChunkStore::kInvalidSlot;
        if (kind == ChunkKind::Dense && !pack) {
            slot = cell->state == CellState::Dense ? cell->slot : m_Store.Alloc();
            if (slot == ChunkStore::kInvalidSlot) return SetChunkResult::StoreFull;
            m_Store.Get(slot) = *m_Scratch;
            m_Store.MarkDirty(slot);
        } else if (cell->state == CellState::Dense && cell->slot != ChunkStore::kInvalidSlot) {
            m_Garbage.chunks.push_back(m_Store.Release(cell->slot));
        }
        DropKeptBytes(index);         // Packed bytes, or the copy a resident buried chunk kept
        if (!wasLoaded) SettleLoading(index);
        cell->slot = slot;
        cell->state = pack ? CellState::Packed
                           : kind == ChunkKind::Dense ? CellState::Dense : kind == ChunkKind::Uniform ? CellState::Uniform : CellState::Empty;
        cell->uniformId = kind == ChunkKind::Uniform ? id : uint8_t{0};
        cell->packedId = 0;
        if (pack) {
            // The message's own bytes are the cell's (a valid Dense stream: DecodeChunk took it, and
            // its hash is canonical over runs); the grid word shows its most frequent solid id.
            std::vector<uint8_t> bytes(rle.begin(), rle.end());
            cell->uniformId = MostFrequentSolidId(bytes);
            m_Counters.packedBytes += bytes.size();
            m_Packed[index] = std::move(bytes);
        }
        if (mode == InstallMode::Edit) {
            cell->modified = true;    // not the generator's output (never packed, kept as an edit)
            cell->serverVisibility = 0;
        } else {
            cell->modified = keepEdit;   // content, not an edit: buried neighbours stay packed
            cell->serverVisibility = mode == InstallMode::HiddenContent ? kServerHidden : kServerShown;
        }
        cell->fromSave = false;
        SetGrid(index, *cell);
        ChunkChange& change = QueueChange(*cell);
        if (wasLoaded) {
            // Content replaced: the old collider is stale everywhere in the chunk (physics simulates no
            // body against a stale part of a collider; an empty box would mean "only loaded").
            change.editMin = ChunkToVoxel(chunk);
            change.editMax = change.editMin + glm::ivec3(kChunkSize);
        }
        if (m_Pack && mode == InstallMode::Edit) PromoteFaceNeighbours(chunk);
        // A packed chunk near the focus is made resident like a loaded one (PromoteNear; Repack packs
        // it again once the focus has moved on and no edit shows it).
        if (pack) m_NearPromotePending = true;
        RefreshContentTop();
        return SetChunkResult::Installed;
    }

    uint8_t World::ServerVisibility(const glm::ivec3& chunk) const {
        const Cell* cell = FindCell(chunk);
        return cell ? cell->serverVisibility : uint8_t{0};
    }

    void World::ShowServerHiddenNeighbours(const glm::ivec3& chunk) {
        static constexpr glm::ivec3 kFaces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const glm::ivec3& f : kFaces) {
            Cell* cell = FindCell(chunk + f);
            if (!cell || cell->serverVisibility != kServerHidden) continue;
            cell->serverVisibility = kServerShown;
            if (cell->state == CellState::Packed) m_Reveal.push_back(CellIndex(chunk + f));
        }
    }

    std::vector<glm::ivec3> World::TakeWindowSteps() {
        std::vector<glm::ivec3> out = std::move(m_WindowSteps);
        m_WindowSteps.clear();
        return out;
    }

    std::vector<glm::ivec3> World::TakeLoadedChunks() {
        std::vector<glm::ivec3> out = std::move(m_LoadedChunks);
        m_LoadedChunks.clear();
        return out;
    }

    // --- quantised edits ----------------------------------------------------------------------

    namespace {
        constexpr int64_t kQ = kQuantaPerVoxel;
        // The centre is an int32 in q: +-2^27 voxels.
        constexpr double kMaxCenterQ = 2147483647.0;

        int32_t QuantizeCoordinate(double voxels) {
            if (!std::isfinite(voxels)) return 0;
            const double q = std::clamp(voxels * static_cast<double>(kQ), -kMaxCenterQ, kMaxCenterQ);
            return static_cast<int32_t>(std::llround(q));
        }

        /// floor(a / b) and ceil(a / b) for b > 0, any sign of a.
        int64_t FloorDiv64(int64_t a, int64_t b) { return a / b - ((a % b != 0) && (a < 0) ? 1 : 0); }
        int64_t CeilDiv64(int64_t a, int64_t b) { return a / b + ((a % b != 0) && (a > 0) ? 1 : 0); }

        /// Smallest squared distance (q^2) from the sphere centre to the centre of a voxel of the
        /// voxel range [lo, hi) along one axis; -1 if it exceeds radius^2 already.
        int64_t AxisDistance2(int64_t center, int64_t radius, int32_t lo, int32_t hi) {
            // The voxel whose centre is nearest: 16 v + 8 closest to `center`.
            const int64_t nearest = std::clamp<int64_t>(FloorDiv64(center, kQ), lo, static_cast<int64_t>(hi) - 1);
            const int64_t d = kQ * nearest + kQ / 2 - center;
            const int64_t a = d < 0 ? -d : d;
            if (a > radius) return -1;
            return a * a;
        }
    }

    std::array<bool, 256> CarvableIdsFor(const MaterialLibrary* materials, uint16_t power) {
        std::array<bool, 256> carvable{};
        for (uint32_t id = 1; id < 256; ++id) {
            if (!materials) {
                carvable[id] = true;
                continue;
            }
            const Material& m = materials->Get(kWorldPalette, static_cast<uint8_t>(id));
            // hardness * 16 is exact in float (a power of two), and so is the conversion of
            // a 16-bit integer: this compare has no rounding.
            const bool tooHard = m.hardness * static_cast<float>(kPowerQuantaPerHardness) > static_cast<float>(power);
            carvable[id] = (m.flags & MaterialFlag_Indestructible) == 0 && !tooHard;
        }
        return carvable;
    }

    bool SphereTouchesBox(const SphereQ& s, const glm::ivec3& lo, const glm::ivec3& hi) {
        if (hi.x <= lo.x || hi.y <= lo.y || hi.z <= lo.z) return false;
        const int64_t r = s.radius;
        int64_t sum = 0;
        for (int a = 0; a < 3; ++a) {
            const int64_t d2 = AxisDistance2(s.center[a], r, lo[a], hi[a]);
            if (d2 < 0) return false;
            sum += d2;
        }
        return sum <= r * r;
    }

    SphereQ QuantizeSphere(const glm::dvec3& centerVoxels, double radiusVoxels) {
        SphereQ s;
        s.center = glm::ivec3(QuantizeCoordinate(centerVoxels.x), QuantizeCoordinate(centerVoxels.y), QuantizeCoordinate(centerVoxels.z));
        if (!std::isfinite(radiusVoxels) || !std::isfinite(centerVoxels.x) || !std::isfinite(centerVoxels.y) ||
            !std::isfinite(centerVoxels.z)) {
            s.radius = 0;
            return s;
        }
        const double r = std::clamp(radiusVoxels * static_cast<double>(kQ), 0.0, 65535.0);
        s.radius = static_cast<uint16_t>(std::llround(r));
        return s;
    }

    uint16_t QuantizePower(float power) {
        if (!(power > 0.0f)) return 0;      // also NaN
        const double p = std::min(static_cast<double>(power) * kPowerQuantaPerHardness, 65535.0);
        return static_cast<uint16_t>(std::llround(p));
    }

    bool InSphereQ(const SphereQ& s, const glm::ivec3& v) {
        const int64_t r = s.radius;
        int64_t sum = 0;
        for (int a = 0; a < 3; ++a) {
            const int64_t d = kQ * static_cast<int64_t>(v[a]) + kQ / 2 - static_cast<int64_t>(s.center[a]);
            const int64_t m = d < 0 ? -d : d;
            if (m > r) return false;           // also keeps the squares below 2^34
            sum += m * m;
        }
        return sum <= r * r;
    }

    void SphereVoxelBounds(const SphereQ& s, glm::ivec3& min, glm::ivec3& max) {
        for (int a = 0; a < 3; ++a) {
            const int64_t c = s.center[a];
            const int64_t r = s.radius;
            // 16 v + 8 >= c - r  and  16 v + 8 <= c + r
            int64_t lo = CeilDiv64(c - r - kQ / 2, kQ);
            int64_t hi = FloorDiv64(c + r - kQ / 2, kQ) + 1;
            lo = std::clamp<int64_t>(lo, -kMaxEditVoxel, kMaxEditVoxel);
            hi = std::clamp<int64_t>(hi, lo, kMaxEditVoxel);
            min[a] = static_cast<int32_t>(lo);
            max[a] = static_cast<int32_t>(hi);
        }
    }

    namespace {
        // The squares below stay under 2^62: a capsule's axis is at most kMaxCapsuleAxisQ per axis,
        // and the voxel centre is first checked against the capsule's box.
        __extension__ typedef __int128 Int128;

        bool CapsuleTooLong(const ShapeQ& s) {
            for (int a = 0; a < 3; ++a) {
                const int64_t u = static_cast<int64_t>(s.b[a]) - s.a[a];
                if (u > kMaxCapsuleAxisQ || -u > kMaxCapsuleAxisQ) return true;
            }
            return false;
        }

        bool InCapsuleQ(const ShapeQ& s, const glm::ivec3& v) {
            if (CapsuleTooLong(s)) return false;
            const int64_t r = s.radiusQ;
            int64_t d[3], u[3];
            for (int a = 0; a < 3; ++a) {
                const int64_t c = kQ * static_cast<int64_t>(v[a]) + kQ / 2;
                const int64_t lo = std::min<int64_t>(s.a[a], s.b[a]) - r;
                const int64_t hi = std::max<int64_t>(s.a[a], s.b[a]) + r;
                if (c < lo || c > hi) return false;
                d[a] = c - s.a[a];
                u[a] = static_cast<int64_t>(s.b[a]) - s.a[a];
            }
            const int64_t rr = r * r;
            const int64_t dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            const int64_t uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
            const int64_t t = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
            if (uu == 0 || t <= 0) return dd <= rr;
            if (t >= uu) {
                int64_t ee = 0;
                for (int a = 0; a < 3; ++a) ee += (d[a] - u[a]) * (d[a] - u[a]);
                return ee <= rr;
            }
            // The squared distance to the segment is dd - t^2 / uu.
            return static_cast<Int128>(dd) * uu - static_cast<Int128>(t) * t <= static_cast<Int128>(rr) * uu;
        }

        /// Voxels [min, max) whose centre 16 v + 8 lies in [loQ, hiQ], clipped to +-kMaxEditVoxel.
        void QToVoxelRange(int64_t loQ, int64_t hiQ, int32_t& min, int32_t& max) {
            int64_t lo = CeilDiv64(loQ - kQ / 2, kQ);
            int64_t hi = FloorDiv64(hiQ - kQ / 2, kQ) + 1;
            lo = std::clamp<int64_t>(lo, -kMaxEditVoxel, kMaxEditVoxel);
            hi = std::clamp<int64_t>(hi, lo, kMaxEditVoxel);
            min = static_cast<int32_t>(lo);
            max = static_cast<int32_t>(hi);
        }
    }

    bool InShapeQ(const ShapeQ& s, const glm::ivec3& v) {
        switch (s.kind) {
            case kShapeSphere: return InSphereQ(SphereQ{s.a, s.radiusQ}, v);
            case kShapeCapsule: return InCapsuleQ(s, v);
            case kShapeBox:
                return v.x >= s.a.x && v.x < s.b.x && v.y >= s.a.y && v.y < s.b.y && v.z >= s.a.z && v.z < s.b.z;
            case kShapeCylinder: {
                const int64_t r = s.radiusQ;
                const int64_t h = s.halfHeightQ;
                const int64_t dx = kQ * static_cast<int64_t>(v.x) + kQ / 2 - s.a.x;
                const int64_t dy = kQ * static_cast<int64_t>(v.y) + kQ / 2 - s.a.y;
                const int64_t dz = kQ * static_cast<int64_t>(v.z) + kQ / 2 - s.a.z;
                if (dx > r || -dx > r || dz > r || -dz > r || dy > h || -dy > h) return false;
                return dx * dx + dz * dz <= r * r;
            }
            default: return false;
        }
    }

    void ShapeVoxelBounds(const ShapeQ& s, glm::ivec3& min, glm::ivec3& max) {
        switch (s.kind) {
            case kShapeSphere:
                SphereVoxelBounds(SphereQ{s.a, s.radiusQ}, min, max);
                return;
            case kShapeCapsule:
                if (CapsuleTooLong(s)) break;
                for (int a = 0; a < 3; ++a) {
                    QToVoxelRange(std::min<int64_t>(s.a[a], s.b[a]) - s.radiusQ, std::max<int64_t>(s.a[a], s.b[a]) + s.radiusQ,
                                  min[a], max[a]);
                }
                return;
            case kShapeBox:
                min = glm::clamp(s.a, glm::ivec3(-kMaxEditVoxel), glm::ivec3(kMaxEditVoxel));
                max = glm::clamp(s.b, min, glm::ivec3(kMaxEditVoxel));
                return;
            case kShapeCylinder:
                for (int a = 0; a < 3; ++a) {
                    const int64_t extent = a == 1 ? s.halfHeightQ : s.radiusQ;
                    QToVoxelRange(static_cast<int64_t>(s.a[a]) - extent, static_cast<int64_t>(s.a[a]) + extent, min[a], max[a]);
                }
                return;
            default: break;
        }
        min = max = glm::ivec3(0);
    }

    bool World::SetVoxel(const glm::ivec3& v, uint8_t id) {
        if (!IsChunkLoaded(VoxelToChunk(v))) return false;
        if (GetVoxel(v) == id) return true;
        return EditRegion(v, v + glm::ivec3(1), [id](const glm::ivec3&, uint8_t) { return static_cast<int>(id); },
                          [](const glm::ivec3&, const glm::ivec3&) { return true; }, nullptr, nullptr, nullptr) == 1;
    }

    uint32_t World::FillBox(const glm::ivec3& min, const glm::ivec3& max, uint8_t id, std::vector<VoxelEdit>* edits, EditReport* report,
                            const ChunkClip* clip) {
        // Clipped to the editable range: no integer overflow at chunk borders near the int limits.
        const glm::ivec3 lo = glm::clamp(min, glm::ivec3(-kMaxEditVoxel), glm::ivec3(kMaxEditVoxel));
        const glm::ivec3 hi = glm::clamp(max, glm::ivec3(-kMaxEditVoxel), glm::ivec3(kMaxEditVoxel));
        return EditRegion(lo, hi, [id](const glm::ivec3&, uint8_t) { return static_cast<int>(id); },
                          [](const glm::ivec3&, const glm::ivec3&) { return true; }, edits, report, clip);
    }

    uint32_t World::FillSphereQ(const SphereQ& sphere, uint8_t id, std::vector<VoxelEdit>* edits, EditReport* report, const ChunkClip* clip) {
        glm::ivec3 mn, mx;
        SphereVoxelBounds(sphere, mn, mx);
        return EditRegion(mn, mx, [&sphere, id](const glm::ivec3& v, uint8_t) { return InSphereQ(sphere, v) ? static_cast<int>(id) : -1; },
                          [&sphere](const glm::ivec3& lo, const glm::ivec3& hi) { return SphereTouchesBox(sphere, lo, hi); }, edits, report,
                          clip);
    }

    uint32_t World::CarveSphereQ(const SphereQ& sphere, uint16_t power, std::vector<VoxelEdit>* removed, EditReport* report,
                                 const ChunkClip* clip) {
        glm::ivec3 mn, mx;
        SphereVoxelBounds(sphere, mn, mx);
        const std::array<bool, 256> carvable = CarvableIdsFor(m_Materials, power);
        return EditRegion(mn, mx, [&sphere, &carvable](const glm::ivec3& v, uint8_t oldId) {
            if (oldId == 0 || !carvable[oldId] || !InSphereQ(sphere, v)) return -1;
            return 0;
        }, [&sphere](const glm::ivec3& lo, const glm::ivec3& hi) { return SphereTouchesBox(sphere, lo, hi); }, removed, report, clip);
    }

    uint32_t World::CarveSphereQInChunk(const SphereQ& sphere, uint16_t power, const glm::ivec3& chunk, std::vector<VoxelEdit>* removed,
                                        EditReport* report) {
        // A carve never puts solid voxels anywhere, so the ceiling (now decided on the whole
        // sphere) never refused one: the same result as the old clip of the box.
        const ChunkClip clip = ChunkClip::One(chunk);
        return CarveSphereQ(sphere, power, removed, report, &clip);
    }

    uint32_t World::SwapSphereQ(const SphereQ& sphere, uint8_t from, uint8_t to, std::vector<VoxelEdit>* edits, EditReport* report,
                                const ChunkClip* clip) {
        if (from == to) return 0;
        glm::ivec3 mn, mx;
        SphereVoxelBounds(sphere, mn, mx);
        return EditRegion(mn, mx, [&sphere, from, to](const glm::ivec3& v, uint8_t oldId) {
            return oldId == from && InSphereQ(sphere, v) ? static_cast<int>(to) : -1;
        }, [&sphere](const glm::ivec3& lo, const glm::ivec3& hi) { return SphereTouchesBox(sphere, lo, hi); }, edits, report, clip);
    }

    uint32_t World::ClearMask(const glm::ivec3& origin, const VoxelMask& mask, std::vector<VoxelEdit>* removed, EditReport* report,
                              const ChunkClip* clip) {
        const glm::ivec3 size = mask.Size();
        if (size.x <= 0 || size.y <= 0 || size.z <= 0) return 0;
        // The box must stay inside the editable range (no integer overflow of origin + size).
        for (int a = 0; a < 3; ++a) {
            if (origin[a] < -kMaxEditVoxel || static_cast<int64_t>(origin[a]) + size[a] > kMaxEditVoxel) return 0;
        }
        // Bits of a row along +x are consecutive in the mask: rows without one are skipped whole.
        const auto anyInRow = [&mask, origin](const glm::ivec3& first, int length) {
            const glm::ivec3 p = first - origin;
            const size_t i = mask.Index(p.x, p.y, p.z);
            return mask.AnyInRange(i, i + static_cast<size_t>(length));
        };
        return EditRegion(origin, origin + size, [&mask, origin](const glm::ivec3& v, uint8_t oldId) {
            return oldId != 0 && mask.Get(v - origin) ? 0 : -1;
        }, [&anyInRow](const glm::ivec3& lo, const glm::ivec3& hi) {
            for (int z = lo.z; z < hi.z; ++z)
                for (int y = lo.y; y < hi.y; ++y) {
                    if (anyInRow(glm::ivec3(lo.x, y, z), hi.x - lo.x)) return true;
                }
            return false;
        }, removed, report, clip, anyInRow);
    }

    uint32_t World::MapShapeQ(const ShapeQ& shape, const TurningMap& map, std::vector<VoxelEdit>* edits, EditReport* report,
                              const ChunkClip* clip) {
        glm::ivec3 mn, mx;
        ShapeVoxelBounds(shape, mn, mx);
        return EditRegion(mn, mx, [&shape, &map](const glm::ivec3& v, uint8_t oldId) {
            return map[oldId] != oldId && InShapeQ(shape, v) ? static_cast<int>(map[oldId]) : -1;
        }, [](const glm::ivec3&, const glm::ivec3&) { return true; }, edits, report, clip);
    }

    uint32_t World::CarveCapsuleQ(const ShapeQ& capsule, uint16_t power, std::vector<VoxelEdit>* removed, EditReport* report,
                                  const ChunkClip* clip) {
        if (capsule.kind != kShapeCapsule) return 0;
        glm::ivec3 mn, mx;
        ShapeVoxelBounds(capsule, mn, mx);
        const std::array<bool, 256> carvable = CarvableIdsFor(m_Materials, power);
        return EditRegion(mn, mx, [&capsule, &carvable](const glm::ivec3& v, uint8_t oldId) {
            return oldId != 0 && carvable[oldId] && InShapeQ(capsule, v) ? 0 : -1;
        }, [](const glm::ivec3&, const glm::ivec3&) { return true; }, removed, report, clip);
    }

    uint32_t World::MapMaskQ(const glm::ivec3& origin, const VoxelMask& mask, const TurningMap& map, std::vector<VoxelEdit>* edits,
                             EditReport* report, const ChunkClip* clip) {
        const glm::ivec3 size = mask.Size();
        if (size.x <= 0 || size.y <= 0 || size.z <= 0) return 0;
        for (int a = 0; a < 3; ++a) {
            if (origin[a] < -kMaxEditVoxel || static_cast<int64_t>(origin[a]) + size[a] > kMaxEditVoxel) return 0;
        }
        const auto anyInRow = [&mask, origin](const glm::ivec3& first, int length) {
            const glm::ivec3 p = first - origin;
            const size_t i = mask.Index(p.x, p.y, p.z);
            return mask.AnyInRange(i, i + static_cast<size_t>(length));
        };
        return EditRegion(origin, origin + size, [&mask, &map, origin](const glm::ivec3& v, uint8_t oldId) {
            return map[oldId] != oldId && mask.Get(v - origin) ? static_cast<int>(map[oldId]) : -1;
        }, [&anyInRow](const glm::ivec3& lo, const glm::ivec3& hi) {
            for (int z = lo.z; z < hi.z; ++z)
                for (int y = lo.y; y < hi.y; ++y) {
                    if (anyInRow(glm::ivec3(lo.x, y, z), hi.x - lo.x)) return true;
                }
            return false;
        }, edits, report, clip, anyInRow);
    }

    uint32_t World::EditVoxelsInChunk(const glm::ivec3& chunk, const uint8_t* newIds, std::vector<VoxelEdit>* edits, EditReport* report) {
        if (!IsChunkLoaded(chunk)) {
            if (report) report->skipped.push_back(chunk);
            return 0;
        }
        const glm::ivec3 origin = ChunkToVoxel(chunk);
        const ChunkClip clip = ChunkClip::One(chunk);
        return EditRegion(origin, origin + glm::ivec3(kChunkSize), [newIds, origin](const glm::ivec3& v, uint8_t) {
            const glm::ivec3 l = v - origin;
            return static_cast<int>(newIds[Chunk::VoxelIndex(l.x, l.y, l.z)]);
        }, [](const glm::ivec3&, const glm::ivec3&) { return true; }, edits, report, &clip);
    }

    uint32_t World::FillSphere(const glm::vec3& center, float radius, uint8_t id, std::vector<VoxelEdit>* edits) {
        return FillSphereQ(QuantizeSphere(glm::dvec3(center), static_cast<double>(radius)), id, edits);
    }

    uint32_t World::CarveSphere(const glm::vec3& center, float radius, float power, std::vector<VoxelEdit>* removed) {
        return CarveSphereQ(QuantizeSphere(glm::dvec3(center), static_cast<double>(radius)), QuantizePower(power), removed);
    }
}
