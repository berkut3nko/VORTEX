module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

module vortex.physics;

import vortex.core;
import vortex.math;
import vortex.voxel;

namespace vortex::physics {

    namespace {

        constexpr int kN = voxel::kChunkSize;

        // Face f lies on axis f / 2; odd faces look towards +axis.
        const std::array<glm::ivec3, 6> kFaceOffsets = {
            glm::ivec3(-1, 0, 0), glm::ivec3(1, 0, 0), glm::ivec3(0, -1, 0),
            glm::ivec3(0, 1, 0),  glm::ivec3(0, 0, -1), glm::ivec3(0, 0, 1),
        };

        struct IVec3Hash {
            size_t operator()(const glm::ivec3& v) const {
                uint64_t h = static_cast<uint32_t>(v.x) * 0x9E3779B97F4A7C15ull;
                h ^= (static_cast<uint32_t>(v.y) + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
                h ^= (static_cast<uint32_t>(v.z) + 0x165667B1ull) * 0x94D049BB133111EBull;
                h ^= h >> 29;
                return static_cast<size_t>(h);
            }
        };

        /// What a cooking job needs from the world, copied on the main thread.
        struct Snapshot {
            voxel::Chunk chunk;
            /// Collidability of the voxel layer of each face neighbour that touches this chunk,
            /// indexed by the two in-plane coordinates: l[(axis + 1) % 3] + l[(axis + 2) % 3] * 32.
            std::array<std::bitset<kN * kN>, 6> border;

            bool Outside(const glm::ivec3& local) const {
                for (int axis = 0; axis < 3; ++axis) {
                    if (local[axis] != -1 && local[axis] != kN) continue;
                    const int u = local[(axis + 1) % 3];
                    const int v = local[(axis + 2) % 3];
                    if (u < 0 || u >= kN || v < 0 || v >= kN) return false;
                    const size_t face = static_cast<size_t>(axis * 2 + (local[axis] == kN ? 1 : 0));
                    return border[face].test(static_cast<size_t>(u + v * kN));
                }
                return false;
            }
        };

        struct CookResult {
            glm::ivec3 coord{0};
            uint64_t generation = 0;
            ShapeRef shape;              // null: the chunk has no collision surface
        };

        // State shared with the cooking jobs. Jobs hold a shared_ptr to it, so they can still be
        // queued when the TerrainColliders object dies: `cancelled` then tells them to do nothing.
        struct Shared {
            explicit Shared(const PhysicsWorld& world) : physics(&world) {}

            const PhysicsWorld* physics;      // only dereferenced under `lifetime` while not cancelled
            std::shared_mutex lifetime;       // jobs: shared while cooking; destructor: exclusive
            bool cancelled = false;

            std::mutex mutex;
            std::vector<CookResult> results;
            std::atomic<uint32_t> inFlight{0};
        };

        void CookJob(const std::shared_ptr<Shared>& shared, const std::shared_ptr<const Snapshot>& snapshot,
                     const std::shared_ptr<const voxel::CollisionFilter>& filter, glm::ivec3 coord, uint64_t generation,
                     float voxelSize) {
            // Runs last, also if something below throws: Flush() relies on the counter.
            const ScopeExit done([&shared] { shared->inFlight.fetch_sub(1); });

            std::shared_lock alive(shared->lifetime);
            if (shared->cancelled) return;

            CookResult result;
            result.coord = coord;
            result.generation = generation;

            voxel::ChunkMesh mesh = voxel::BuildChunkCollisionMesh(
                snapshot->chunk, *filter, [&snapshot](const glm::ivec3& local) { return snapshot->Outside(local); });
            if (!mesh.Empty()) {
                for (glm::vec3& v : mesh.vertices) v *= voxelSize;
                result.shape = shared->physics->CreateMeshShape(mesh.vertices, mesh.indices);
            }

            std::scoped_lock lock(shared->mutex);
            shared->results.push_back(std::move(result));
        }
    }

    struct TerrainColliders::Impl {
        struct Entry {
            BodyId body;
            uint64_t generation = 0;     // identifies the newest cook request; older results are dropped
            bool queued = false;         // waiting in `dirty` for a (re)build
            bool cooking = false;        // a job for `generation` is in flight
            bool built = false;          // the collider (or the fact that none is needed) has been established once
        };

        Impl(PhysicsWorld& physicsWorld, voxel::World& voxelWorld, JobSystem& jobSystem, const voxel::MaterialLibrary& materialLibrary)
            : physics(physicsWorld), world(voxelWorld), jobs(jobSystem), materials(materialLibrary),
              shared(std::make_shared<Shared>(physicsWorld)) {
            maxJobs = std::max(2u, jobs.ThreadCount() * 2u);
        }

        PhysicsWorld& physics;
        voxel::World& world;
        JobSystem& jobs;
        const voxel::MaterialLibrary& materials;
        std::shared_ptr<Shared> shared;

        std::shared_ptr<const voxel::CollisionFilter> filter;
        uint64_t filterRevision = 0;

        std::unordered_map<glm::ivec3, Entry, IVec3Hash> chunks;
        std::vector<glm::ivec3> dirty;            // coordinates of queued entries (may hold stale / duplicate coordinates)
        uint64_t nextGeneration = 0;
        uint32_t maxJobs = 2;
        uint32_t colliderCount = 0;
        uint32_t builtTotal = 0;

        std::vector<glm::dvec3> focus;            // focus points of the last Update()
        double keepRadius = 0.0;                  // chunks within this distance of a focus point get a collider
        std::vector<glm::dvec3> scanFocus;        // what the last full range scan was made for
        double scanRadius = -1.0;

        double ChunkMetres() const { return static_cast<double>(world.VoxelSize()) * kN; }

        /// Distance from the nearest focus point to the chunk's box (0 inside).
        double Distance(const glm::ivec3& coord) const {
            const double size = ChunkMetres();
            const glm::dvec3 lo = glm::dvec3(coord) * size;
            const glm::dvec3 hi = lo + glm::dvec3(size);
            double best = std::numeric_limits<double>::infinity();
            for (const glm::dvec3& p : focus) {
                const glm::dvec3 d = glm::max(glm::max(lo - p, p - hi), glm::dvec3(0.0));
                best = std::min(best, glm::dot(d, d));
            }
            return std::sqrt(best);
        }

        math::AABB ChunkBounds(const glm::ivec3& coord) const {
            const float size = world.VoxelSize() * static_cast<float>(kN);
            const glm::vec3 lo = glm::vec3(glm::dvec3(coord) * ChunkMetres());
            return math::AABB(lo, lo + glm::vec3(size));
        }

        void RefreshFilter() {
            if (filter && materials.Revision() == filterRevision) return;
            filterRevision = materials.Revision();
            auto fresh = std::make_shared<const voxel::CollisionFilter>(
                voxel::CollisionFilter::FromMaterials(materials, voxel::kWorldPalette, voxel::MaterialFlag_Liquid));
            const bool changed = filter && filter->collidable != fresh->collidable;
            filter = std::move(fresh);
            if (!changed) return;
            // A material started or stopped colliding: every collider may be different now.
            for (auto& [coord, entry] : chunks) MarkDirty(coord, entry);
        }

        void MarkDirty(const glm::ivec3& coord, Entry& entry) {
            entry.generation = ++nextGeneration;   // whatever is being cooked for this chunk is now stale
            entry.cooking = false;
            if (entry.queued) return;
            entry.queued = true;
            dirty.push_back(coord);
        }

        bool WantsCollider(const glm::ivec3& coord) const {
            const voxel::CellState state = world.StateOf(coord);
            return (state == voxel::CellState::Dense || state == voxel::CellState::Uniform) && Distance(coord) <= keepRadius;
        }

        /// A chunk changed: rebuild it if it is tracked; start tracking it if `adopt` and it is in range.
        void Touch(const glm::ivec3& coord, bool adopt) {
            if (auto it = chunks.find(coord); it != chunks.end()) {
                MarkDirty(coord, it->second);
            } else if (adopt && WantsCollider(coord)) {
                MarkDirty(coord, chunks[coord]);
            }
        }

        bool NeedsScan(double radius) const {
            if (radius != scanRadius || focus.size() != scanFocus.size()) return true;
            // Chunks within `radius` of a focus point stay covered by the last scan (made with a
            // quarter chunk of slack) until that point has moved a quarter chunk.
            const double slack = 0.25 * ChunkMetres();
            for (size_t i = 0; i < focus.size(); ++i) {
                const glm::dvec3 d = focus[i] - scanFocus[i];
                if (!(glm::dot(d, d) <= slack * slack)) return true;
            }
            return false;
        }

        /// Starts tracking every loaded, non-empty chunk in range that is not tracked yet.
        void Scan() {
            const double size = ChunkMetres();
            const glm::ivec3 windowMin = world.WindowMinChunk();
            const glm::ivec3 windowMax = windowMin + world.WindowSize() - glm::ivec3(1);
            for (const glm::dvec3& p : focus) {
                const glm::dvec3 lo = glm::floor((p - glm::dvec3(keepRadius)) / size);
                const glm::dvec3 hi = glm::floor((p + glm::dvec3(keepRadius)) / size);
                // Clamp in floating point first: far away focus points must not overflow the integer range.
                const glm::ivec3 cmin = glm::ivec3(glm::clamp(lo, glm::dvec3(windowMin), glm::dvec3(windowMax) + glm::dvec3(1.0)));
                const glm::ivec3 cmax = glm::ivec3(glm::clamp(hi, glm::dvec3(windowMin) - glm::dvec3(1.0), glm::dvec3(windowMax)));
                for (int y = cmin.y; y <= cmax.y; ++y)
                    for (int z = cmin.z; z <= cmax.z; ++z)
                        for (int x = cmin.x; x <= cmax.x; ++x) {
                            const glm::ivec3 coord(x, y, z);
                            if (chunks.contains(coord) || !WantsCollider(coord)) continue;
                            MarkDirty(coord, chunks[coord]);
                        }
            }
        }

        void RemoveBody(Entry& entry) {
            if (!entry.body.IsValid()) return;
            physics.DestroyBody(entry.body);
            entry.body = {};
            --colliderCount;
        }

        /// Installs the outcome of a build: `shape` (may be null) replaces the chunk's collider.
        void Apply(const glm::ivec3& coord, Entry& entry, const ShapeRef& shape) {
            const bool rebuilt = entry.built;
            const bool hadBody = entry.body.IsValid();
            RemoveBody(entry);
            if (shape) {
                BodyDesc desc;
                desc.shape = shape;
                desc.position = ChunkBounds(coord).min;
                desc.motion = MotionType::Static;
                desc.layer = Layer::Static;
                entry.body = physics.CreateBody(desc);
                if (entry.body.IsValid()) {
                    ++colliderCount;
                    ++builtTotal;
                }
            }
            entry.cooking = false;
            entry.built = true;

            // The ground changed under whatever was resting here: let it react.
            if (rebuilt && (hadBody || entry.body.IsValid())) {
                math::AABB bounds = ChunkBounds(coord);
                bounds.Expand(world.VoxelSize() + 0.05f);
                physics.ActivateInBox(bounds);
            }
        }

        bool IsUniformCollidable(const glm::ivec3& coord) const {
            uint8_t uniformId = 0;
            return world.GetChunk(coord, &uniformId) == nullptr && uniformId != 0 && filter->collidable[uniformId];
        }

        /// True if the chunk cannot have a collision surface, decided without looking at voxels.
        bool NeedsNoCollider(const glm::ivec3& coord) const {
            uint8_t uniformId = 0;
            const voxel::Chunk* chunk = world.GetChunk(coord, &uniformId);
            if (chunk) return chunk->IsEmpty();
            if (uniformId == 0 || !filter->collidable[uniformId]) return true;      // air, not loaded, liquid ...
            for (const glm::ivec3& offset : kFaceOffsets) {
                if (!IsUniformCollidable(coord + offset)) return false;
            }
            return true;                                                            // solid all around: deep underground
        }

        std::shared_ptr<const Snapshot> TakeSnapshot(const glm::ivec3& coord) const {
            auto snapshot = std::make_shared<Snapshot>();
            uint8_t uniformId = 0;
            if (const voxel::Chunk* chunk = world.GetChunk(coord, &uniformId)) snapshot->chunk = *chunk;
            else snapshot->chunk.Fill(uniformId);

            for (size_t face = 0; face < kFaceOffsets.size(); ++face) {
                uint8_t neighbourId = 0;
                const voxel::Chunk* neighbour = world.GetChunk(coord + kFaceOffsets[face], &neighbourId);
                auto& bits = snapshot->border[face];
                if (!neighbour) {
                    // Uniform: closed or open as a whole. Empty or not loaded: open.
                    if (neighbourId != 0 && filter->collidable[neighbourId]) bits.set();
                    continue;
                }
                const int axis = static_cast<int>(face / 2);
                glm::ivec3 local(0);
                local[axis] = (face & 1u) ? 0 : kN - 1;      // the neighbour's layer that touches this chunk
                for (int v = 0; v < kN; ++v)
                    for (int u = 0; u < kN; ++u) {
                        local[(axis + 1) % 3] = u;
                        local[(axis + 2) % 3] = v;
                        if (filter->collidable[neighbour->Get(local.x, local.y, local.z)]) bits.set(static_cast<size_t>(u + v * kN));
                    }
            }
            return snapshot;
        }

        void InstallResults() {
            std::vector<CookResult> ready;
            {
                std::scoped_lock lock(shared->mutex);
                ready.swap(shared->results);
            }
            for (const CookResult& result : ready) {
                const auto it = chunks.find(result.coord);
                // The chunk may have been edited again, dropped or re-adopted while the job ran.
                if (it == chunks.end() || !it->second.cooking || it->second.generation != result.generation) continue;
                Apply(result.coord, it->second, result.shape);
            }
        }

        /// Starts builds for queued chunks, nearest first, until the job budget is used up.
        void ProcessDirty() {
            if (dirty.empty()) return;

            struct Candidate {
                double distance;
                glm::ivec3 coord;
            };
            std::vector<Candidate> order;
            order.reserve(dirty.size());
            for (const glm::ivec3& coord : dirty) order.push_back({Distance(coord), coord});
            std::sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) {
                if (a.distance != b.distance) return a.distance < b.distance;
                if (a.coord.x != b.coord.x) return a.coord.x < b.coord.x;
                if (a.coord.y != b.coord.y) return a.coord.y < b.coord.y;
                return a.coord.z < b.coord.z;
            });

            dirty.clear();
            const glm::ivec3* previous = nullptr;
            for (const Candidate& candidate : order) {
                if (previous && *previous == candidate.coord) continue;     // duplicates are adjacent after sorting
                previous = &candidate.coord;

                const auto it = chunks.find(candidate.coord);
                if (it == chunks.end() || !it->second.queued) continue;     // dropped since it was queued
                Entry& entry = it->second;

                if (NeedsNoCollider(candidate.coord)) {
                    entry.queued = false;
                    Apply(candidate.coord, entry, nullptr);
                    continue;
                }
                if (shared->inFlight.load() >= maxJobs) {
                    dirty.push_back(candidate.coord);                       // stays queued for a later update
                    continue;
                }

                entry.queued = false;
                entry.cooking = true;
                shared->inFlight.fetch_add(1);
                jobs.Submit([state = shared, snapshot = TakeSnapshot(candidate.coord), cookFilter = filter, coord = candidate.coord,
                             generation = entry.generation, voxelSize = world.VoxelSize()] {
                    CookJob(state, snapshot, cookFilter, coord, generation, voxelSize);
                });
            }
        }
    };

    TerrainColliders::TerrainColliders(PhysicsWorld& physics, voxel::World& world, JobSystem& jobs,
                                       const voxel::MaterialLibrary& materials)
        : m_Impl(std::make_unique<Impl>(physics, world, jobs, materials)) {
        m_Impl->RefreshFilter();
    }

    TerrainColliders::~TerrainColliders() {
        {
            // Waits for jobs that are cooking right now; jobs that start later see the flag and
            // never touch the physics world.
            std::unique_lock lock(m_Impl->shared->lifetime);
            m_Impl->shared->cancelled = true;
        }
        for (auto& [coord, entry] : m_Impl->chunks) m_Impl->RemoveBody(entry);
    }

    void TerrainColliders::Update(std::span<const glm::dvec3> focusPoints, float radius) {
        Impl& impl = *m_Impl;
        impl.RefreshFilter();

        impl.focus.clear();
        for (const glm::dvec3& p : focusPoints) {
            if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) impl.focus.push_back(p);
        }
        const double r = (std::isfinite(radius) && radius > 0.0f) ? static_cast<double>(radius) : 0.0;
        const double slack = 0.25 * impl.ChunkMetres();
        // Colliders are created a little beyond the requested radius (so the scan below does not
        // have to run every frame) and removed well beyond it (so they do not flicker at the edge).
        impl.keepRadius = r > 0.0 ? r + slack : -1.0;
        const double dropRadius = r > 0.0 ? std::max(1.5 * r, r + 2.0 * slack) : -1.0;

        // 1. Chunks whose content changed (loaded, unloaded, edited), and the neighbours whose
        //    border faces depend on them.
        for (const glm::ivec3& coord : impl.world.TakeChangedChunks()) {
            impl.Touch(coord, true);
            for (const glm::ivec3& offset : kFaceOffsets) impl.Touch(coord + offset, false);
        }

        // 2. Chunks that came into range because the focus points moved.
        if (impl.NeedsScan(r)) {
            if (impl.keepRadius > 0.0) impl.Scan();
            impl.scanFocus = impl.focus;
            impl.scanRadius = r;
        }

        // 3. Chunks that are out of range of every focus point or no longer loaded.
        for (auto it = impl.chunks.begin(); it != impl.chunks.end();) {
            if (!impl.world.IsChunkLoaded(it->first) || !(impl.Distance(it->first) <= dropRadius)) {
                impl.RemoveBody(it->second);
                it = impl.chunks.erase(it);
            } else {
                ++it;
            }
        }

        // 4. Finished builds, then new ones.
        impl.InstallResults();
        impl.ProcessDirty();
    }

    bool TerrainColliders::IsReady(const glm::dvec3& worldPos) const {
        const Impl& impl = *m_Impl;
        const glm::ivec3 coord = voxel::VoxelToChunk(impl.world.WorldToVoxel(worldPos));
        if (!impl.world.IsChunkLoaded(coord)) return false;
        if (const auto it = impl.chunks.find(coord); it != impl.chunks.end()) return it->second.built;
        return impl.NeedsNoCollider(coord);
    }

    void TerrainColliders::Flush() {
        Impl& impl = *m_Impl;
        for (;;) {
            impl.InstallResults();
            impl.ProcessDirty();

            // Read the job counter first: a job publishes its result before it counts itself out.
            const bool idle = impl.shared->inFlight.load() == 0;
            bool resultsWaiting;
            {
                std::scoped_lock lock(impl.shared->mutex);
                resultsWaiting = !impl.shared->results.empty();
            }
            if (idle && !resultsWaiting && impl.dirty.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    TerrainColliderStats TerrainColliders::Stats() const {
        TerrainColliderStats stats;
        stats.colliders = m_Impl->colliderCount;
        stats.pending = m_Impl->shared->inFlight.load();
        stats.built = m_Impl->builtTotal;
        return stats;
    }
}
