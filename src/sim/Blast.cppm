module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

export module vortex.sim:blast;

import vortex.voxel;
import vortex.scene;
import :simulation;
import :worldop;

export namespace vortex::sim {

    /// @brief A blast as a game asks for it: world metres and material hardness (quantised by Blast).
    struct BlastDesc {
        glm::dvec3 center{0.0};   ///< World metres.
        float radius = 1.0f;      ///< Metres.
        float power = 1.0f;       ///< Terrain voxels harder than this survive (Material::hardness units).
    };

    /// @brief One operation of a blast as it was applied to the blasting simulation's world.
    struct AppliedOp {
        WorldOp op;
        OpResult result;
        /// Voxels the operation changed, in ApplyOp order: the carved voxels (particles), the
        /// voxels of a detached piece (BuildDetachedPiece) or the crumbs of the ClearMask
        /// (particles). Presentation data, never sent.
        std::vector<voxel::VoxelEdit> edits;
    };

    /// @brief Wall-clock cost of the simulation half of a blast, milliseconds (docs/design/shred.md
    /// stage 0). Logged only: nothing is ever decided from a time.
    struct BlastTimes {
        double carveMs = 0.0;      ///< The CarveSphere (or its replay in a late search).
        double searchMs = 0.0;     ///< The island box gathered and labelled (ExtractFloatingIslands).
        double floodMs = 0.0;      ///< The face flood (voxel::ProveFaceComponents).
        double opsMs = 0.0;        ///< Detach / ClearMask built and applied.
        double objectsMs = 0.0;    ///< Scene::Damage and the push.
    };

    /// @brief What the island search of a blast saw (docs/design/shred.md stage 1c: nothing
    /// silent). Counts of work and decisions; equal on every machine for equal voxels.
    struct IslandSearchStats {
        uint64_t boxVoxels = 0;          ///< Island box voxels gathered.
        uint32_t faceCandidates = 0;     ///< Face components of at most kMaxIslandVoxels in the box: flooded.
        uint32_t facePieces = 0;         ///< Proven by the flood and detached (a Detach each).
        uint32_t faceCrumbs = 0;         ///< Proven by the flood, under the piece size: in the ClearMask.
        uint32_t faceOversize = 0;       ///< Face components over kMaxIslandVoxels in the box: attached, not flooded.
        /// Floating components inside the box over kMaxIslandVoxels: they stay in the air.
        uint32_t oversizeIslands = 0;
        uint32_t merged = 0;             ///< Candidates that were part of an earlier proven island.
        uint32_t attachedByLimit = 0;    ///< voxel::FloodResult counters, summed.
        uint32_t attachedByKnown = 0;
        uint32_t attachedByBudget = 0;
        /// A flood met a chunk that is not loaded on a session server: an error (its window holds
        /// every flood box by construction), logged; the candidate stayed attached.
        uint32_t attachedByUnloaded = 0;
        uint32_t chunksDecoded = 0;
        uint32_t floodVisits = 0;
        /// A chunk of the island box was not loaded on a session server: an error (its window holds
        /// every island box by construction), logged; nothing was searched and nothing waits.
        uint32_t islandBoxUnloaded = 0;
        /// Offline: a flood met a chunk of the flood box that is not loaded. The face pieces were
        /// not emitted, the search waits for that chunk (RetryIslandSearches).
        bool floodDeferred = false;
    };

    /**
     * @brief Everything sim::Blast did: the replicated operations in their order plus what the
     * presentation (PresentBlast) needs.
     */
    struct BlastResult {
        /// ops[0] is the CarveSphere; one Detach (netId 0) per terrain island of at least
        /// scene::MinPieceVoxels(world voxel size) follows: first the islands inside the island
        /// box in the order ExtractFloatingIslands found them, then the face components the flood
        /// proved, in candidate order (in-box count ascending, ties by label); last, if there were
        /// smaller islands ("crumbs", from both), one ClearMask over the box of all of them.
        /// Applying the `op`s in this order to a world that was equal before gives the same
        /// voxels (ApplyOp).
        std::vector<AppliedOp> ops;
        /// The island box (blast radius + 24 voxels) was loaded and searched and the face flood
        /// completed. false: a chunk of the island box was not loaded (nothing emitted), or offline
        /// the flood met a chunk of the flood box that is not loaded (`search.floodDeferred`: the
        /// in-box islands were emitted, the face pieces were not). Either way the search is
        /// remembered and repeated by RetryIslandSearches. A server window always has both boxes
        /// loaded; an offline window may not, near its edge or while it streams.
        bool islandsSearched = false;
        /// Made by RetryIslandSearches: the Detach / ClearMask operations of an earlier blast whose
        /// search had to wait, preceded by the replayed CarveSphere when the blast's carve had
        /// skipped chunks that have loaded since (`carveReplayed`; a result may hold only that, while
        /// the search still waits). PresentBlast shows no explosion and no flash for it.
        bool deferred = false;
        /// Deferred only: ops[0] is the blast's carve applied again, to `carveReplayChunks` only
        /// (the chunks it had skipped that have loaded since; World::CarveSphereQInChunk). Such an
        /// op is not a self-contained replay of WorldOp::Carve: it is offline only, never sent.
        bool carveReplayed = false;
        std::vector<glm::ivec3> carveReplayChunks;
        /// This blast's search was deferred into a full list and replaced an older deferred search
        /// (the one farthest from the window's centre): that one is lost (logged).
        uint32_t deferredDropped = 0;
        /// RetryIslandSearches only: a deferred search given up because its flood box left the
        /// window (logged). Such a result has no operations.
        uint32_t deferredGivenUp = 0;
        IslandSearchStats search;
        BlastTimes times;
        uint32_t crumbs = 0;           ///< Islands under scene::MinPieceVoxels(world voxel size), all in the one ClearMask.
        uint32_t objectsHit = 0;       ///< Objects that lost voxels (Scene::Damage removed > 0).
        uint32_t objectVoxels = 0;     ///< Voxels removed from objects.
        /// Object pieces below SceneConfig::replicatedPieceVoxels (none while it is 0), their
        /// velocity already including the blast's push. Local rubble for whoever draws.
        std::vector<scene::DamageFragment> fragments;
        glm::dvec3 center{0.0};        ///< The blast as applied (after quantisation), world metres.
        float radius = 0.0f;           ///< Metres, after quantisation.

        /// @brief Union of the skipped / unsynced chunks of all operations.
        voxel::EditReport Report() const;
        /// @brief Detach operations: islands of at least scene::MinPieceVoxels(world voxel size) (crumbs not counted).
        uint32_t IslandCount() const;
        /// @brief Voxels the ClearMask removed (0 without crumbs).
        uint32_t CrumbVoxels() const;
    };

    /// Islands up to this many voxels are detached (larger floating parts stay in place, counted).
    inline constexpr uint32_t kMaxIslandVoxels = 60000;
    /// The island search box is the blast's bounding box grown by this many voxels per side.
    inline constexpr int32_t kIslandSearchMargin = 24;
    /// Work budget of one blast's face flood, in voxel visits (docs/design/shred.md, R5).
    inline constexpr uint32_t kFloodVisitBudget = 4 * kMaxIslandVoxels;
    /// Island searches waiting at once (Simulation::DeferredIslandSearches). When the list is full,
    /// a new one replaces the one whose box is farthest from the window's centre (ties: the oldest);
    /// the new search itself is always kept (it is the blast the player just made).
    inline constexpr size_t kMaxDeferredIslandSearches = 16;
    /// A session server's window is its play bounds (whole chunks) plus this many chunks on all six
    /// sides, so the flood box of every cast centred inside the play bounds is loaded
    /// (docs/design/networking.md, "Server world without a camera").
    inline constexpr int32_t kServerWindowMarginChunks = 3;
    static_assert(kServerWindowMarginChunks * voxel::kChunkSize >= voxel::kFloodHalfExtent);

    /// @brief The flood box of a blast: floor(centre / 16) +- voxel::kFloodHalfExtent voxels, [min, max).
    void FloodBox(const voxel::SphereQ& sphere, glm::ivec3& min, glm::ivec3& max);
    /// @brief The island box of a blast: floor(centre / 16) +- (radius / 16 + kIslandSearchMargin), [min, max).
    void IslandBox(const voxel::SphereQ& sphere, glm::ivec3& min, glm::ivec3& max);

    /**
     * @brief The push of a blast at `position`, as a velocity change (m/s): away from the centre,
     * 6 m/s x a linear falloff that reaches 0 at three radii; straight up at the centre. Bodies
     * get it as an impulse (x mass), detached pieces and fragments as their initial velocity.
     */
    glm::vec3 BlastPush(const glm::vec3& center, float radius, const glm::vec3& position);

    /**
     * @brief The simulation half of a blast (server, or an offline game): carve, detach terrain
     * islands, damage objects, push bodies. Spawns no particles and no light: that is
     * presentation (PresentBlast), made from the returned result.
     * @details In order:
     *  1. `WorldOp::Carve(sphere, power)` is applied (voxel::World::CarveSphereQ).
     *  2. ExtractFloatingIslands over the island box `floor(centre) +- (radius / 16 + 24)` voxels,
     *     without removing anything. Components touching the box face (attached there) with at
     *     most kMaxIslandVoxels voxels in the box are then proven by voxel::ProveFaceComponents
     *     inside the flood box (`floor(centre) +- 96` voxels, budget kFloodVisitBudget visits):
     *     a flood that closes is an island too. Each island of at least
     *     scene::MinPieceVoxels(world voxel size) becomes a `WorldOp::Detach` (mask of its voxels,
     *     netId 0, velocity = BlastPush at its minimum corner) that is applied at once with
     *     ApplyOp - the same call a client makes: in-box islands first, then proven ones. The
     *     smaller islands (crumbs) become one `WorldOp::Clear` whose mask is the union of their
     *     voxels over the smallest box holding them all, applied last. No entity is created for
     *     an island: rubble and particles are local. Offline, a search that meets a chunk that
     *     is not loaded (island box, or a flood in the flood box) asks the world to load it and
     *     is repeated by RetryIslandSearches; on a session server (SimulationConfig::sessionServer)
     *     a flood that meets one stays attached and is counted as an error.
     *  3. Scene::Damage for every body whose bounds overlap the blast's box (a snapshot taken
     *     first, so pieces made here are not hit twice); pieces below the replicated threshold
     *     come back as fragments.
     *  4. Every rigid body within three radii gets mass x BlastPush; bodies within two radii are
     *     woken (PhysicsWorld::ActivateInBox), so what rested on removed ground falls.
     * Steps 1-2 are integer-exact (equal worlds give equal operations); steps 3-4 are server
     * results (float poses, Jolt) and are never hashed.
     * Call it between the streaming and the physics phase of a tick (Application::OnFixedUpdate)
     * or from Application::OnUpdate; never inside a registry view.
     */
    BlastResult Blast(Simulation& simulation, const voxel::SphereQ& sphere, uint16_t power);

    /**
     * @brief Runs the island search (step 2 of Blast) of earlier blasts that had to wait
     * (`islandsSearched` false: an offline window streaming its cave rows, or a blast near the
     * window's edge).
     * @details Blast asked the world to load what was missing (voxel::World::RequestLoad) and
     * remembered the search (Simulation::DeferredIslandSearches). A search runs again once its
     * island box is loaded and, if a flood was waiting, the chunk it met. When the blast's own
     * carve had skipped chunks that were not loaded, the carve is applied to each of them, and only
     * to them, in the first call after it has loaded (before the search, which waits for them);
     * it is ops[0] of that call's result (`carveReplayed`, `carveReplayChunks`). The
     * operations are applied and returned as one BlastResult with `deferred` set (show it with
     * PresentBlast). A search is given up only once its flood box has left the window (a result
     * with `deferredGivenUp`, logged). Never needed on a server (its window always has the boxes
     * loaded); the operations are later operations of their own, not part of the blast's.
     * Call once per frame between the streaming and the physics phase (Engine::Run does).
     */
    std::vector<BlastResult> RetryIslandSearches(Simulation& simulation);
    /// @brief Quantises (voxel::QuantizeSphere with the world's voxel size, voxel::QuantizePower)
    /// and runs the blast above.
    BlastResult Blast(Simulation& simulation, const BlastDesc& desc);

    /**
     * @brief The presentation half of a blast, for whoever draws (Engine::PresentBlast offline;
     * a client from stage 3, given operations it applied itself). Never on a server.
     * @details Into the simulation's particle pool and scene: per CarveSphere debris particles
     * (a sample, at most 799 bursts), per Detach with netId 0 local rubble (`VoxelObjectDesc::debris`, made
     * from the voxels it removed), per ClearMask particles only - every
     * `SceneConfig::debrisParticleStride`-th crumb voxel, at most 400 bursts, pushed with
     * BlastPush at the voxel - never an entity or a body; rubble for the object fragments, the
     * explosion burst and a fading flash. Changes no voxel and nothing that is hashed.
     */
    void PresentBlast(Simulation& simulation, const BlastResult& blast);
}
