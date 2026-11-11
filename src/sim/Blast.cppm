module;

#include <glm/glm.hpp>

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
        /// Voxels the operation changed, in ApplyOp order: the carved voxels (particles) or the
        /// voxels of a detached piece (BuildDetachedPiece). Presentation data, never sent.
        std::vector<voxel::VoxelEdit> edits;
    };

    /**
     * @brief Everything sim::Blast did: the replicated operations in their order plus what the
     * presentation (Engine::PresentBlast) needs.
     */
    struct BlastResult {
        /// ops[0] is the CarveSphere; one Detach (netId 0) per terrain island follows, in the
        /// order ExtractFloatingIslands found them. Applying the `op`s in this order to a world
        /// that was equal before gives the same voxels (ApplyOp).
        std::vector<AppliedOp> ops;
        /// The island box (blast radius + 24 voxels) was loaded and searched. false: a chunk of it
        /// was not loaded, no island could be proven and none was emitted (a server window always
        /// has the box loaded; an offline window may not, near its edge).
        bool islandsSearched = false;
        uint32_t objectsHit = 0;       ///< Objects that lost voxels (Scene::Damage removed > 0).
        uint32_t objectVoxels = 0;     ///< Voxels removed from objects.
        /// Object pieces below SceneConfig::replicatedPieceVoxels (none while it is 0), their
        /// velocity already including the blast's push. Local rubble for whoever draws.
        std::vector<scene::DamageFragment> fragments;
        glm::dvec3 center{0.0};        ///< The blast as applied (after quantisation), world metres.
        float radius = 0.0f;           ///< Metres, after quantisation.

        /// @brief Union of the skipped / unsynced chunks of all operations.
        voxel::EditReport Report() const;
        uint32_t IslandCount() const { return ops.empty() ? 0u : static_cast<uint32_t>(ops.size() - 1); }
    };

    /// Islands up to this many voxels are detached (larger floating parts stay in place).
    inline constexpr uint32_t kMaxIslandVoxels = 60000;
    /// The island search box is the blast's bounding box grown by this many voxels per side.
    inline constexpr int32_t kIslandSearchMargin = 24;

    /**
     * @brief The push of a blast at `position`, as a velocity change (m/s): away from the centre,
     * 6 m/s x a linear falloff that reaches 0 at three radii; straight up at the centre. Bodies
     * get it as an impulse (x mass), detached pieces and fragments as their initial velocity.
     */
    glm::vec3 BlastPush(const glm::vec3& center, float radius, const glm::vec3& position);

    /**
     * @brief The simulation half of a blast (server, or an offline game): carve, detach terrain
     * islands, damage objects, push bodies. Spawns no particles and no light: that is
     * presentation (Engine::PresentBlast), made from the returned result.
     * @details In order:
     *  1. `WorldOp::Carve(sphere, power)` is applied (voxel::World::CarveSphereQ).
     *  2. ExtractFloatingIslands over the box `floor(centre) +- (radius / 16 + 24)` voxels, without
     *     removing anything; each island becomes a `WorldOp::Detach` (mask of its voxels, netId 0,
     *     velocity = BlastPush at its minimum corner) that is applied at once with ApplyOp - the
     *     same call a client makes. No entity is created for an island: rubble is local.
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
    /// @brief Quantises (voxel::QuantizeSphere with the world's voxel size, voxel::QuantizePower)
    /// and runs the blast above.
    BlastResult Blast(Simulation& simulation, const BlastDesc& desc);
}
