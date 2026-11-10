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

        // --- body guard tuning ------------------------------------------------------------------
        /// A body is frozen if the terrain it reaches within this time is not ready (seconds; a
        /// few fixed steps, the guard runs before each of them).
        constexpr float kFreezeLookahead = 0.05f;
        /// A moving body asks for the colliders of the chunks it reaches within this time (seconds),
        /// so that they are cooked before it arrives.
        constexpr float kRequestLookahead = 0.75f;
        /// Update() calls a collider requested by a body is kept after the last request.
        constexpr uint64_t kBodyKeepUpdates = 120;

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
            uint64_t generation = 0;     // identifies the newest cook request
            /// Generation of the result that is installed (for a new entry: the newest generation
            /// that existed before it). Results that are not newer are dropped; one that is newer
            /// is installed even if the chunk has been edited again since - a chunk edited every
            /// frame would otherwise never get a collider.
            uint64_t installed = 0;
            /// Generation of the last change of a face neighbour that uncovered faces of this
            /// chunk. Until a result at least this new is installed the collider has a hole there
            /// (border faces covered by a solid neighbour are not meshed), so the chunk is not ready.
            uint64_t exposed = 0;
            bool queued = false;         // waiting in `dirty` for a (re)build
            bool built = false;          // the collider (or the fact that none is needed) has been established once
            bool hasBorder = false;      // `border` is valid
            uint64_t keepUntil = 0;      // a body needs this chunk: not dropped before this Update() index
            /// Neighbour collidability the newest mesh job of this chunk was given (Snapshot::border),
            /// kept up to date as neighbours change: a neighbour change that leaves it as it is
            /// does not need a new mesh.
            std::array<std::bitset<kN * kN>, 6> border;
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
        uint64_t updateIndex = 1;                 // number of the current Update() call
        std::vector<DynamicBodyInfo> bodies;      // scratch of GuardBodies()

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
            for (auto& [coord, entry] : chunks) {
                entry.hasBorder = false;
                MarkDirty(coord, entry);
            }
        }

        void MarkDirty(const glm::ivec3& coord, Entry& entry) {
            entry.generation = ++nextGeneration;   // whatever is being cooked for this chunk is now out of date
            if (entry.queued) return;
            entry.queued = true;
            dirty.push_back(coord);
        }

        /// Starts tracking a chunk and queues its first build.
        Entry& Track(const glm::ivec3& coord) {
            Entry& entry = chunks[coord];
            // Jobs of an earlier entry for this chunk (dropped since) may still deliver: not wanted.
            entry.installed = nextGeneration;
            MarkDirty(coord, entry);
            return entry;
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
                Track(coord);
            }
        }

        /// The chunk beyond face `face` of chunk `coord` changed. The mesh of `coord` depends on
        /// it only through the collidability of the touching voxel layer.
        void NeighbourChanged(const glm::ivec3& coord, size_t face) {
            const auto it = chunks.find(coord);
            if (it == chunks.end()) return;
            Entry& entry = it->second;
            if (!entry.hasBorder) {
                MarkDirty(coord, entry);
                // Established without a mesh: air, which has nothing to uncover, or rock that was
                // enclosed by rock and may now have an open side.
                if (entry.built && IsUniformCollidable(coord)) entry.exposed = entry.generation;
                return;
            }
            const std::bitset<kN * kN> bits = BorderBits(coord, face);
            if (bits == entry.border[face]) return;
            const bool uncovered = (entry.border[face] & ~bits).any();
            entry.border[face] = bits;
            MarkDirty(coord, entry);
            if (uncovered) entry.exposed = entry.generation;
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
                            Track(coord);
                        }
            }
        }

        /// True if a body may be simulated in this chunk: it is loaded and its collider exists
        /// or it needs none. An edited chunk keeps the old collider until the new one is cooked,
        /// which is safe (the old surface is still there) - except where carving a *neighbour*
        /// uncovered faces this chunk's collider never had: that chunk is not ready until re-cooked.
        bool ChunkReady(const glm::ivec3& coord) const {
            if (!world.IsChunkLoaded(coord)) return false;
            if (const auto it = chunks.find(coord); it != chunks.end()) return it->second.built && it->second.installed >= it->second.exposed;
            return NeedsNoCollider(coord);
        }

        bool BoxReady(const math::AABB& bounds, const glm::vec3& travel) const {
            glm::ivec3 lo, hi;
            if (!ChunkRange(bounds, travel, world.VoxelSize(), lo, hi)) return false;
            for (int y = lo.y; y <= hi.y; ++y)
                for (int z = lo.z; z <= hi.z; ++z)
                    for (int x = lo.x; x <= hi.x; ++x) {
                        if (!ChunkReady(glm::ivec3(x, y, z))) return false;
                    }
            return true;
        }

        /// A body needs collision in this chunk now, wherever the focus points are.
        void Require(const glm::ivec3& coord) {
            auto it = chunks.find(coord);
            if (it == chunks.end()) {
                const voxel::CellState state = world.StateOf(coord);
                if (state != voxel::CellState::Dense && state != voxel::CellState::Uniform) return;   // nothing to collide with, or not loaded
                Track(coord);
                it = chunks.find(coord);
            }
            it->second.keepUntil = updateIndex + kBodyKeepUpdates;
        }

        /// Chunk coordinates touched by a world-space box grown by `margin` and swept along `travel`.
        /// False if the box is unusable.
        bool ChunkRange(const math::AABB& bounds, const glm::vec3& travel, float margin, glm::ivec3& lo, glm::ivec3& hi) const {
            if (bounds.IsEmpty()) return false;
            const glm::vec3 bmin = glm::min(bounds.min, bounds.min + travel) - glm::vec3(margin);
            const glm::vec3 bmax = glm::max(bounds.max, bounds.max + travel) + glm::vec3(margin);
            const glm::dvec3 a = glm::floor(glm::dvec3(bmin) / ChunkMetres());
            const glm::dvec3 b = glm::floor(glm::dvec3(bmax) / ChunkMetres());
            for (int i = 0; i < 3; ++i) {
                // Also false for NaN. Bodies that far out are not in any world window.
                if (!(std::abs(a[i]) < 1.0e9 && std::abs(b[i]) < 1.0e9)) return false;
            }
            lo = glm::ivec3(a);
            hi = glm::ivec3(b);
            return true;
        }

        void GuardBodies() {
            physics.GetDynamicBodies(bodies);
            const float voxel = world.VoxelSize();
            const float chunk = voxel * static_cast<float>(kN);
            for (const DynamicBodyInfo& info : bodies) {
                glm::vec3 velocity = info.linearVelocity;
                if (!(std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z))) velocity = glm::vec3(0.0f);
                glm::ivec3 lo, hi;

                // 1. Bodies that move, or want to, get their colliders: where they are and where
                //    they are going (at most two chunks ahead, however fast they are).
                if (info.active || info.wantsToMove) {
                    const glm::vec3 ahead = glm::clamp(velocity * kRequestLookahead, glm::vec3(-2.0f * chunk), glm::vec3(2.0f * chunk));
                    if (ChunkRange(info.bounds, ahead, voxel, lo, hi)) {
                        for (int y = lo.y; y <= hi.y; ++y)
                            for (int z = lo.z; z <= hi.z; ++z)
                                for (int x = lo.x; x <= hi.x; ++x) Require(glm::ivec3(x, y, z));
                    }
                }

                // 2. No body is simulated where the terrain cannot be collided with.
                const glm::vec3 next = glm::clamp(velocity * kFreezeLookahead, glm::vec3(-chunk), glm::vec3(chunk));
                const bool ready = BoxReady(info.bounds, next);
                if (ready == info.frozen) physics.SetFrozen(info.body, !ready);
            }
        }

        void RemoveBody(Entry& entry) {
            if (!entry.body.IsValid()) return;
            physics.DestroyBody(entry.body);
            entry.body = {};
            --colliderCount;
        }

        /// Installs the outcome of a build: `shape` (may be null) replaces the chunk's collider.
        void Apply(const glm::ivec3& coord, Entry& entry, const ShapeRef& shape, uint64_t generation) {
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
            entry.installed = generation;
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

        /// Collidability of the voxel layer beyond face `face` of chunk `coord` (Snapshot::border).
        std::bitset<kN * kN> BorderBits(const glm::ivec3& coord, size_t face) const {
            std::bitset<kN * kN> bits;
            uint8_t neighbourId = 0;
            const voxel::Chunk* neighbour = world.GetChunk(coord + kFaceOffsets[face], &neighbourId);
            if (!neighbour) {
                // Uniform: closed or open as a whole. Empty or not loaded: open.
                if (neighbourId != 0 && filter->collidable[neighbourId]) bits.set();
                return bits;
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
            return bits;
        }

        std::shared_ptr<const Snapshot> TakeSnapshot(const glm::ivec3& coord) const {
            auto snapshot = std::make_shared<Snapshot>();
            uint8_t uniformId = 0;
            if (const voxel::Chunk* chunk = world.GetChunk(coord, &uniformId)) snapshot->chunk = *chunk;
            else snapshot->chunk.Fill(uniformId);
            for (size_t face = 0; face < kFaceOffsets.size(); ++face) snapshot->border[face] = BorderBits(coord, face);
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
                // The chunk may have been dropped or re-adopted while the job ran, or a newer result
                // overtook this one. If it was merely edited again, this result is still better
                // than what is installed; the newest state is queued or cooking and follows.
                if (it == chunks.end() || result.generation <= it->second.installed) continue;
                Apply(result.coord, it->second, result.shape, result.generation);
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
            for (const glm::ivec3& coord : dirty) {
                // Chunks a body is waiting for come first: that body is frozen until they exist.
                const auto it = chunks.find(coord);
                const bool urgent = it != chunks.end() && it->second.keepUntil >= updateIndex;
                order.push_back({urgent ? -1.0 : Distance(coord), coord});
            }
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
                    entry.hasBorder = false;
                    Apply(candidate.coord, entry, nullptr, entry.generation);
                    continue;
                }
                if (shared->inFlight.load() >= maxJobs) {
                    dirty.push_back(candidate.coord);                       // stays queued for a later update
                    continue;
                }

                entry.queued = false;
                std::shared_ptr<const Snapshot> taken = TakeSnapshot(candidate.coord);
                entry.border = taken->border;
                entry.hasBorder = true;
                shared->inFlight.fetch_add(1);
                jobs.Submit([state = shared, snapshot = std::move(taken), cookFilter = filter, coord = candidate.coord,
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
        const bool anyFrozen = impl.physics.FrozenBodyCount() > 0;
        for (const glm::ivec3& coord : impl.world.TakeChangedChunks()) {
            impl.Touch(coord, true);
            // Face f of the neighbour beyond face f ^ 1 looks back at this chunk.
            for (size_t face = 0; face < kFaceOffsets.size(); ++face) impl.NeighbourChanged(coord + kFaceOffsets[face], face ^ 1u);
            // A tracked chunk wakes what rests on it when its new collider is installed. Where
            // nothing is tracked, bodies are parked (frozen asleep): tell them their ground
            // changed, so they come back to life, ask for colliders and fall if it is gone.
            if (anyFrozen && !impl.chunks.contains(coord) && impl.world.IsChunkLoaded(coord)) {
                math::AABB bounds = impl.ChunkBounds(coord);
                bounds.Expand(impl.world.VoxelSize() + 0.05f);
                impl.physics.ActivateInBox(bounds);
            }
        }

        // 2. Chunks that came into range because the focus points moved.
        if (impl.NeedsScan(r)) {
            if (impl.keepRadius > 0.0) impl.Scan();
            impl.scanFocus = impl.focus;
            impl.scanRadius = r;
        }

        // 3. Chunks that are no longer loaded, or out of range of every focus point and not
        //    needed by a body lately.
        ++impl.updateIndex;
        for (auto it = impl.chunks.begin(); it != impl.chunks.end();) {
            const bool wanted = impl.Distance(it->first) <= dropRadius || it->second.keepUntil >= impl.updateIndex;
            if (!impl.world.IsChunkLoaded(it->first) || !wanted) {
                impl.RemoveBody(it->second);
                it = impl.chunks.erase(it);
            } else {
                ++it;
            }
        }

        // 4. Finished builds; then the bodies (which may resume on what was just installed, be
        //    frozen where step 3 took the ground away, and ask for more); then new builds.
        impl.InstallResults();
        impl.GuardBodies();
        impl.ProcessDirty();
    }

    void TerrainColliders::GuardBodies() {
        m_Impl->GuardBodies();
        m_Impl->ProcessDirty();      // start cooking what the bodies asked for right away
    }

    bool TerrainColliders::IsReady(const glm::dvec3& worldPos) const {
        const Impl& impl = *m_Impl;
        return impl.ChunkReady(voxel::VoxelToChunk(impl.world.WorldToVoxel(worldPos)));
    }

    bool TerrainColliders::IsReady(const math::AABB& worldBounds) const { return m_Impl->BoxReady(worldBounds, glm::vec3(0.0f)); }

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
            if (idle && !resultsWaiting && impl.dirty.empty()) {
                // Bodies resume on the new colliders, and may ask for more.
                impl.GuardBodies();
                if (impl.dirty.empty()) break;
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    TerrainColliderStats TerrainColliders::Stats() const {
        TerrainColliderStats stats;
        stats.colliders = m_Impl->colliderCount;
        stats.pending = m_Impl->shared->inFlight.load();
        stats.built = m_Impl->builtTotal;
        stats.frozenBodies = m_Impl->physics.FrozenBodyCount();
        return stats;
    }
}
