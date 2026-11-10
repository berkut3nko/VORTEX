module;

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
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
            std::unique_ptr<Chunk> chunk;
        };

        explicit Shared(std::filesystem::path saveDir) : regions(std::move(saveDir)) {}

        std::shared_ptr<const IWorldGenerator> generator;
        RegionStore regions;
        std::mutex mutex;
        std::vector<Result> results;
        std::atomic<uint32_t> inFlight{0};
    };

    World::World(const WorldConfig& config, ChunkStore& store, JobSystem& jobs,
                 std::shared_ptr<const IWorldGenerator> generator, const MaterialLibrary* materials)
        : m_Config(config), m_Store(store), m_Jobs(jobs), m_Materials(materials) {
        const glm::ivec3 s = m_Config.windowSize;
        Verify(s.x > 0 && s.y > 0 && s.z > 0, "WorldConfig::windowSize components must be positive");
        Verify(m_Config.voxelSize > 0.0f, "WorldConfig::voxelSize must be positive");
        Verify(generator != nullptr, "World needs a generator");

        m_Shared = std::make_shared<Shared>(m_Config.saveDirectory);
        m_Shared->generator = std::move(generator);

        const size_t cellCount = static_cast<size_t>(s.x) * static_cast<size_t>(s.y) * static_cast<size_t>(s.z);
        m_Cells.resize(cellCount);
        m_Grid.assign(cellCount, 0u);
        m_MaxJobs = m_Config.maxJobsInFlight != 0 ? m_Config.maxJobsInFlight : std::max(2u, m_Jobs.ThreadCount() * 2u);
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

    uint32_t World::CellIndex(const glm::ivec3& chunk) const {
        const glm::ivec3 s = m_Config.windowSize;
        const glm::ivec3 w = glm::ivec3(math::FloorMod(chunk.x, s.x), math::FloorMod(chunk.y, s.y), math::FloorMod(chunk.z, s.z));
        // Must match VxTraceWorld in shaders/shared/trace.glsl.
        return static_cast<uint32_t>(w.x + w.z * s.x + w.y * s.x * s.z);
    }

    World::Cell* World::FindCell(const glm::ivec3& chunk) {
        Cell& cell = m_Cells[CellIndex(chunk)];
        return (cell.assigned && cell.coord == chunk) ? &cell : nullptr;
    }

    const World::Cell* World::FindCell(const glm::ivec3& chunk) const {
        const Cell& cell = m_Cells[CellIndex(chunk)];
        return (cell.assigned && cell.coord == chunk) ? &cell : nullptr;
    }

    // --- streaming ----------------------------------------------------------------------------

    void World::Update(const glm::dvec3& focus) {
        const glm::ivec3 focusChunk = VoxelToChunk(WorldToVoxel(focus));
        const glm::ivec3 newMin = focusChunk - m_Config.windowSize / 2;
        if (!m_HasWindow || newMin != m_WindowMin) Recenter(newMin, focusChunk);
        IntegrateResults(m_Config.maxIntegrationsPerUpdate);
        SubmitJobs();
    }

    void World::LoadBlocking(const glm::dvec3& focus) {
        for (;;) {
            const glm::ivec3 focusChunk = VoxelToChunk(WorldToVoxel(focus));
            const glm::ivec3 newMin = focusChunk - m_Config.windowSize / 2;
            if (!m_HasWindow || newMin != m_WindowMin) Recenter(newMin, focusChunk);
            IntegrateResults(0xFFFFFFFFu);
            SubmitJobs();

            bool pendingResults;
            {
                std::scoped_lock lock(m_Shared->mutex);
                pendingResults = !m_Shared->results.empty();
            }
            if (m_LoadCursor >= m_LoadOrder.size() && m_Shared->inFlight.load() == 0 && !pendingResults) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void World::Recenter(const glm::ivec3& newMin, const glm::ivec3& focusChunk) {
        const glm::ivec3 s = m_Config.windowSize;
        m_WindowMin = newMin;
        m_HasWindow = true;
        ++m_GridRevision;

        // Every array index holds the one chunk coordinate inside the new window that maps to it.
        for (int wy = 0; wy < s.y; ++wy)
            for (int wz = 0; wz < s.z; ++wz)
                for (int wx = 0; wx < s.x; ++wx) {
                    const glm::ivec3 target(newMin.x + math::FloorMod(wx - newMin.x, s.x),
                                            newMin.y + math::FloorMod(wy - newMin.y, s.y),
                                            newMin.z + math::FloorMod(wz - newMin.z, s.z));
                    const uint32_t index = static_cast<uint32_t>(wx + wz * s.x + wy * s.x * s.z);
                    Cell& cell = m_Cells[index];
                    if (cell.assigned && cell.coord == target) continue;
                    if (cell.assigned) UnloadCell(index);
                    cell.coord = target;
                    cell.assigned = true;
                    cell.state = CellState::Unloaded;
                    ++cell.generation;
                }

        // Load order: nearest chunks first, so what the player stands on appears before the horizon.
        m_LoadOrder.clear();
        for (uint32_t i = 0; i < m_Cells.size(); ++i) {
            if (m_Cells[i].state == CellState::Unloaded) m_LoadOrder.push_back(i);
        }
        auto distance2 = [this, focusChunk](uint32_t index) {
            const glm::ivec3 d = m_Cells[index].coord - focusChunk;
            return d.x * d.x + d.y * d.y + d.z * d.z;
        };
        std::sort(m_LoadOrder.begin(), m_LoadOrder.end(),
                  [&distance2](uint32_t a, uint32_t b) { return distance2(a) < distance2(b); });
        m_LoadCursor = 0;
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
        const bool hadContent = cell.state == CellState::Dense || cell.state == CellState::Uniform;
        if (cell.modified && (hadContent || cell.state == CellState::Empty)) PersistCell(cell);
        if (cell.state == CellState::Dense && cell.slot != ChunkStore::kInvalidSlot) m_Store.Free(cell.slot);
        // The cell is about to represent a different chunk: report the old one directly.
        if (hadContent && !cell.changeQueued) m_ChangedChunks.push_back(cell.coord);

        cell.slot = ChunkStore::kInvalidSlot;
        cell.state = CellState::Unloaded;
        cell.uniformId = 0;
        cell.modified = false;
        cell.changeQueued = false;
        ++cell.generation;   // results of jobs still in flight for the old chunk are now stale
        if (m_Grid[index] != 0u) {
            m_Grid[index] = 0u;
            ++m_GridRevision;
        }
    }

    void World::SetGrid(uint32_t index, const Cell& cell) {
        uint32_t entry = 0u;
        if (cell.state == CellState::Dense) entry = cell.slot + 1u;
        else if (cell.state == CellState::Uniform) entry = 0x80000000u | cell.uniformId;
        if (m_Grid[index] != entry) {
            m_Grid[index] = entry;
            ++m_GridRevision;
        }
    }

    void World::QueueChange(Cell& cell) {
        if (cell.changeQueued) return;
        cell.changeQueued = true;
        m_ChangedChunks.push_back(cell.coord);
    }

    std::vector<glm::ivec3> World::TakeChangedChunks() {
        std::vector<glm::ivec3> out = std::move(m_ChangedChunks);
        m_ChangedChunks.clear();
        for (const glm::ivec3& coord : out) {
            if (Cell* cell = FindCell(coord)) cell->changeQueued = false;
        }
        return out;
    }

    void World::LoadJob(const std::shared_ptr<Shared>& shared, uint32_t cellIndex, glm::ivec3 coord, uint32_t generation) {
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
        }
        if (!loaded) {
            scratch->Clear();
            const ChunkContent content = shared->generator->Generate(coord, *scratch);
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
        if (result.kind == ChunkKind::Dense) result.chunk = std::make_unique<Chunk>(*scratch);
        if (result.kind == ChunkKind::Uniform && result.uniformId == 0) result.kind = ChunkKind::Empty;

        {
            std::scoped_lock lock(shared->mutex);
            shared->results.push_back(std::move(result));
        }
        shared->inFlight.fetch_sub(1);
    }

    void World::SubmitJobs() {
        while (m_LoadCursor < m_LoadOrder.size() && m_Shared->inFlight.load() < m_MaxJobs) {
            const uint32_t index = m_LoadOrder[m_LoadCursor++];
            Cell& cell = m_Cells[index];
            if (cell.state != CellState::Unloaded) continue;
            cell.state = CellState::Loading;
            m_Shared->inFlight.fetch_add(1);
            m_Jobs.Submit([shared = m_Shared, index, coord = cell.coord, generation = cell.generation] {
                LoadJob(shared, index, coord, generation);
            });
        }
    }

    void World::IntegrateResults(uint32_t budget) {
        std::vector<Shared::Result> ready;
        {
            std::scoped_lock lock(m_Shared->mutex);
            if (m_Shared->results.empty()) return;
            if (m_Shared->results.size() <= budget) {
                ready.swap(m_Shared->results);
            } else {
                ready.assign(std::make_move_iterator(m_Shared->results.begin()),
                             std::make_move_iterator(m_Shared->results.begin() + budget));
                m_Shared->results.erase(m_Shared->results.begin(), m_Shared->results.begin() + budget);
            }
        }

        for (Shared::Result& r : ready) {
            Cell& cell = m_Cells[r.cellIndex];
            // The window may have moved on while the job ran.
            if (!cell.assigned || cell.coord != r.coord || cell.generation != r.generation || cell.state != CellState::Loading) continue;

            cell.modified = false;
            cell.uniformId = 0;
            cell.slot = ChunkStore::kInvalidSlot;
            switch (r.kind) {
                case ChunkKind::Empty:
                    cell.state = CellState::Empty;
                    break;
                case ChunkKind::Uniform:
                    cell.state = CellState::Uniform;
                    cell.uniformId = r.uniformId;
                    break;
                case ChunkKind::Dense: {
                    const uint32_t slot = m_Store.Adopt(std::move(r.chunk));
                    if (slot == ChunkStore::kInvalidSlot) {
                        static bool warned = false;
                        if (!warned) {
                            Log::Warn("ChunkStore is full ({} chunks): distant terrain will be missing. "
                                      "Raise the chunk capacity or shrink WorldConfig::windowSize.", m_Store.Capacity());
                            warned = true;
                        }
                        cell.state = CellState::Empty;
                    } else {
                        cell.state = CellState::Dense;
                        cell.slot = slot;
                    }
                    break;
                }
            }
            SetGrid(r.cellIndex, cell);
            if (cell.state == CellState::Dense || cell.state == CellState::Uniform) QueueChange(cell);
        }
    }

    void World::Save() {
        for (Cell& cell : m_Cells) {
            if (!cell.assigned || !cell.modified) continue;
            if (cell.state == CellState::Dense || cell.state == CellState::Uniform || cell.state == CellState::Empty) PersistCell(cell);
        }
        const size_t files = m_Shared->regions.Flush();
        if (files > 0) Log::Info("World saved ({} region file(s))", files);
    }

    WorldStats World::Stats() const {
        WorldStats stats;
        for (const Cell& cell : m_Cells) {
            switch (cell.state) {
                case CellState::Dense:   ++stats.denseChunks; break;
                case CellState::Uniform: ++stats.uniformChunks; break;
                case CellState::Empty:   ++stats.emptyChunks; break;
                default:                 ++stats.pendingChunks; break;
            }
        }
        stats.jobsInFlight = m_Shared->inFlight.load();
        return stats;
    }

    // --- queries ------------------------------------------------------------------------------

    CellState World::StateOf(const glm::ivec3& chunk) const {
        const Cell* cell = FindCell(chunk);
        return cell ? cell->state : CellState::Unloaded;
    }

    bool World::IsChunkLoaded(const glm::ivec3& chunk) const {
        const CellState state = StateOf(chunk);
        return state == CellState::Empty || state == CellState::Uniform || state == CellState::Dense;
    }

    uint8_t World::GetVoxel(const glm::ivec3& v) const {
        const Cell* cell = FindCell(VoxelToChunk(v));
        if (!cell) return 0;
        if (cell->state == CellState::Dense) {
            const glm::ivec3 l = VoxelToLocal(v);
            return m_Store.Get(cell->slot).Get(l.x, l.y, l.z);
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

    TraceScene World::MakeTraceScene() const {
        TraceScene scene;
        scene.store = &m_Store;
        scene.grid = m_Grid.data();
        scene.gridSize = m_Config.windowSize;
        scene.gridMinChunk = m_WindowMin;
        scene.gridOrigin = glm::vec3(0.0f);
        scene.voxelSize = m_Config.voxelSize;
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
        if (!hit.hit) return out;

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

    template <typename Fn>
    uint32_t World::EditRegion(const glm::ivec3& min, const glm::ivec3& max, Fn&& decide, std::vector<VoxelEdit>* edits) {
        if (max.x <= min.x || max.y <= min.y || max.z <= min.z) return 0;
        uint32_t changedTotal = 0;
        const glm::ivec3 cmin = VoxelToChunk(min);
        const glm::ivec3 cmax = VoxelToChunk(max - glm::ivec3(1));

        for (int cy = cmin.y; cy <= cmax.y; ++cy)
            for (int cz = cmin.z; cz <= cmax.z; ++cz)
                for (int cx = cmin.x; cx <= cmax.x; ++cx) {
                    const glm::ivec3 coord(cx, cy, cz);
                    Cell* cell = FindCell(coord);
                    if (!cell) continue;
                    if (cell->state != CellState::Dense && cell->state != CellState::Empty && cell->state != CellState::Uniform) continue;
                    const uint32_t index = CellIndex(coord);

                    const glm::ivec3 origin = ChunkToVoxel(coord);
                    const glm::ivec3 lo = glm::max(min, origin) - origin;
                    const glm::ivec3 hi = glm::min(max, origin + glm::ivec3(kChunkSize)) - origin;
                    uint32_t changed = 0;
                    bool failed = false;

                    for (int z = lo.z; z < hi.z && !failed; ++z)
                        for (int y = lo.y; y < hi.y && !failed; ++y)
                            for (int x = lo.x; x < hi.x; ++x) {
                                uint8_t oldId;
                                if (cell->state == CellState::Dense) oldId = m_Store.Get(cell->slot).Get(x, y, z);
                                else oldId = cell->state == CellState::Uniform ? cell->uniformId : uint8_t{0};

                                const glm::ivec3 voxel = origin + glm::ivec3(x, y, z);
                                const int newId = decide(voxel, oldId);
                                if (newId < 0 || newId == oldId) continue;

                                if (cell->state != CellState::Dense && !Materialize(index)) {
                                    failed = true;
                                    break;
                                }
                                m_Store.Get(cell->slot).Set(x, y, z, static_cast<uint8_t>(newId));
                                if (edits) edits->push_back({voxel, oldId, static_cast<uint8_t>(newId)});
                                ++changed;
                            }

                    if (changed > 0) {
                        m_Store.MarkDirty(cell->slot);
                        cell->modified = true;
                        QueueChange(*cell);
                        changedTotal += changed;
                    }
                }
        return changedTotal;
    }

    bool World::SetVoxel(const glm::ivec3& v, uint8_t id) {
        if (!IsChunkLoaded(VoxelToChunk(v))) return false;
        if (GetVoxel(v) == id) return true;
        return EditRegion(v, v + glm::ivec3(1), [id](const glm::ivec3&, uint8_t) { return static_cast<int>(id); }, nullptr) == 1;
    }

    uint32_t World::FillBox(const glm::ivec3& min, const glm::ivec3& max, uint8_t id, std::vector<VoxelEdit>* edits) {
        return EditRegion(min, max, [id](const glm::ivec3&, uint8_t) { return static_cast<int>(id); }, edits);
    }

    uint32_t World::FillSphere(const glm::vec3& center, float radius, uint8_t id, std::vector<VoxelEdit>* edits) {
        const glm::ivec3 mn = glm::ivec3(glm::floor(center - glm::vec3(radius)));
        const glm::ivec3 mx = glm::ivec3(glm::ceil(center + glm::vec3(radius))) + glm::ivec3(1);
        const float r2 = radius * radius;
        return EditRegion(mn, mx, [center, r2, id](const glm::ivec3& v, uint8_t) {
            const glm::vec3 d = glm::vec3(v) + glm::vec3(0.5f) - center;
            return glm::dot(d, d) <= r2 ? static_cast<int>(id) : -1;
        }, edits);
    }

    uint32_t World::CarveSphere(const glm::vec3& center, float radius, float power, std::vector<VoxelEdit>* removed) {
        const glm::ivec3 mn = glm::ivec3(glm::floor(center - glm::vec3(radius)));
        const glm::ivec3 mx = glm::ivec3(glm::ceil(center + glm::vec3(radius))) + glm::ivec3(1);
        const float r2 = radius * radius;
        const MaterialLibrary* materials = m_Materials;
        return EditRegion(mn, mx, [center, r2, power, materials](const glm::ivec3& v, uint8_t oldId) {
            if (oldId == 0) return -1;
            const glm::vec3 d = glm::vec3(v) + glm::vec3(0.5f) - center;
            if (glm::dot(d, d) > r2) return -1;
            if (materials) {
                const Material& m = materials->Get(kWorldPalette, oldId);
                if ((m.flags & MaterialFlag_Indestructible) != 0 || m.hardness > power) return -1;
            }
            return 0;
        }, removed);
    }
}
