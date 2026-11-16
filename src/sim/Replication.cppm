module;

#include <cstdint>
#include <vector>

export module vortex.sim:replication;

import vortex.physics;
import vortex.scene;
import :simulation;

export namespace vortex::sim {

    /// @brief A NetId for a new server entity: counts up from 1, never reused (Simulation::Replication).
    uint32_t AllocateNetId(Simulation& simulation);

    /**
     * @brief Makes `entity` a replicated server entity: a scene::NetId with a new id, `piece` as
     * given and the current tick as its birth. Returns the id (0 for an invalid entity or one that
     * has a NetId already: its id is returned unchanged).
     */
    uint32_t Replicate(Simulation& simulation, scene::Entity entity, bool piece);

    /// @brief Awake as the replication sees it: Jolt-awake, or frozen by the terrain guard while it
    /// wanted to move (it moves on as soon as its ground has a collider).
    bool IsAwake(const physics::PhysicsWorld& physics, physics::BodyId body);

    /// @brief Replicated pieces alive and awake now (placed objects not counted), and every awake
    /// replicated body (pieces and placed objects: what the awake cap and the snapshots see).
    struct PieceCounts {
        uint32_t live = 0;
        uint32_t awake = 0;
        uint32_t awakeBodies = 0;
    };
    PieceCounts CountPieces(const Simulation& simulation);

    /// @brief What TakePieceSlot decided for a new piece.
    enum class PieceSlot : uint8_t {
        Replicate,        ///< a server body with a NetId
        OverTickBudget,   ///< ReplicationConfig::bodiesPerTick reached this tick: client rubble
        OverAwakeCap,     ///< ReplicationConfig::maxAwakePieces replicated bodies awake: client rubble
        OverByteBudget,   ///< the server's entity byte budget is spent (ReplicationState::refuseNewPieces): client rubble
    };

    /**
     * @brief Asks for a server body for a new piece (sim::Blast, the server's only source of pieces
     * in stage 6). Refused while the entity byte budget is spent, at `bodiesPerTick` this tick, or
     * with `maxAwakePieces` replicated bodies awake (pieces and placed objects). Charges this tick's
     * body budget; with `maxLivePieces` live pieces it first evicts
     * the oldest resting piece, else the smallest awake one (scene::DespawnReason::ToRubble: every
     * client keeps its copy as local rubble). `pending` slots (taken, body made later in the same
     * blast: Detach pieces) count as live and awake. A decision of the server (Jolt's sleep state is
     * an input); clients only see its events. Only with ReplicationConfig::enabled.
     */
    PieceSlot TakePieceSlot(Simulation& simulation);

    /// @brief Destroys a replicated entity, saying why (what its EntityDespawn carries).
    void Despawn(Simulation& simulation, scene::Entity entity, scene::DespawnReason reason);
}
