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

    /**
     * @brief Everything sim::Blast did: the replicated operations in their order plus what the
     * presentation (PresentBlast) needs.
     */
    struct BlastResult {
        /// ops[0] is the CarveSphere; one Detach (netId 0) per terrain island of at least
        /// scene::kMinPieceVoxels follows, in the order ExtractFloatingIslands found them; last,
        /// if there were smaller islands ("crumbs"), one ClearMask over the box of all of them.
        /// Applying the `op`s in this order to a world that was equal before gives the same
        /// voxels (ApplyOp).
        std::vector<AppliedOp> ops;
        /// The island box (blast radius + 24 voxels) was loaded and searched. false: a chunk of it
        /// was not loaded, no island could be proven and none was emitted (a server window always
        /// has the box loaded; an offline window may not, near its edge).
        bool islandsSearched = false;
        /// Made by RetryIslandSearches: only the Detach / ClearMask operations of an earlier
        /// blast whose island box was not loaded then (no CarveSphere; PresentBlast shows no
        /// explosion and no flash for it).
        bool deferred = false;
        uint32_t crumbs = 0;           ///< Islands under scene::kMinPieceVoxels, all in the one ClearMask.
        uint32_t objectsHit = 0;       ///< Objects that lost voxels (Scene::Damage removed > 0).
        uint32_t objectVoxels = 0;     ///< Voxels removed from objects.
        /// Object pieces below SceneConfig::replicatedPieceVoxels (none while it is 0), their
        /// velocity already including the blast's push. Local rubble for whoever draws.
        std::vector<scene::DamageFragment> fragments;
        glm::dvec3 center{0.0};        ///< The blast as applied (after quantisation), world metres.
        float radius = 0.0f;           ///< Metres, after quantisation.

        /// @brief Union of the skipped / unsynced chunks of all operations.
        voxel::EditReport Report() const;
        /// @brief Detach operations: islands of at least scene::kMinPieceVoxels (crumbs not counted).
        uint32_t IslandCount() const;
        /// @brief Voxels the ClearMask removed (0 without crumbs).
        uint32_t CrumbVoxels() const;
    };

    /// Islands up to this many voxels are detached (larger floating parts stay in place).
    inline constexpr uint32_t kMaxIslandVoxels = 60000;
    /// The island search box is the blast's bounding box grown by this many voxels per side.
    inline constexpr int32_t kIslandSearchMargin = 24;
    /// Island searches waiting for their box at once (Simulation::DeferredIslandSearches); the oldest goes.
    inline constexpr size_t kMaxDeferredIslandSearches = 16;
    /// RetryIslandSearches calls after which a search whose box never loaded is given up.
    inline constexpr uint32_t kIslandRetryLimit = 600;

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
     *  2. ExtractFloatingIslands over the box `floor(centre) +- (radius / 16 + 24)` voxels, without
     *     removing anything; each island of at least scene::kMinPieceVoxels becomes a
     *     `WorldOp::Detach` (mask of its voxels, netId 0, velocity = BlastPush at its minimum
     *     corner) that is applied at once with ApplyOp - the same call a client makes. The
     *     smaller islands (crumbs) become one `WorldOp::Clear` whose mask is the union of their
     *     voxels over the smallest box holding them all (inside the island box), applied last.
     *     No entity is created for an island: rubble and particles are local.
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
     * @brief Runs the island search (step 2 of Blast) of earlier blasts whose box was not loaded
     * then (`islandsSearched` false; an offline window streaming its cave rows, or a blast near
     * the window's edge).
     * @details Blast asked the world to load such a box first (voxel::World::RequestLoad) and
     * remembered the search (Simulation::DeferredIslandSearches). Each one whose box is loaded
     * now runs: its Detach / ClearMask operations are applied and returned as one BlastResult
     * with `deferred` set and no CarveSphere (show it with PresentBlast). A search is given up
     * after kIslandRetryLimit calls. Never needed on a server (its window always has the box
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
