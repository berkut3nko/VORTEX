module;

#include <entt/entt.hpp>

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.voxel;
import vortex.physics;
import vortex.scene;

namespace vortex::sim {

    uint32_t AllocateNetId(Simulation& simulation) {
        ReplicationState& r = simulation.Replication();
        ++r.stats.netIds;
        return r.nextNetId++;
    }

    uint32_t Replicate(Simulation& simulation, scene::Entity entity, bool piece) {
        scene::Scene& scene = simulation.Scene();
        if (!scene.IsValid(entity)) return 0;
        if (const scene::NetId* existing = scene.TryGet<scene::NetId>(entity)) return existing->value;
        scene::NetId id;
        id.value = AllocateNetId(simulation);
        id.piece = piece;
        id.bornTick = simulation.Tick();
        scene.Registry().emplace<scene::NetId>(entity, id);
        return id.value;
    }

    bool IsAwake(const physics::PhysicsWorld& physics, physics::BodyId body) {
        if (!physics.IsValid(body)) return false;
        const physics::BodyState state = physics.GetState(body);
        return state.active || state.wantsToMove;
    }

    PieceCounts CountPieces(const Simulation& simulation) {
        PieceCounts c;
        const entt::registry& registry = simulation.Scene().Registry();
        const physics::PhysicsWorld& physics = const_cast<Simulation&>(simulation).Physics();
        for (const auto [entity, id] : registry.view<const scene::NetId>().each()) {
            if (registry.all_of<scene::Mechanism>(entity)) continue;     // kinematic: always awake to Jolt, never a piece slot
            if (registry.all_of<scene::Held>(entity)) continue;          // posed by its holder: not awake, never evicted
            const scene::RigidBody* body = registry.try_get<scene::RigidBody>(entity);
            const bool awake = body && IsAwake(physics, body->body);
            c.awakeBodies += awake ? 1u : 0u;
            if (!id.piece) continue;
            ++c.live;
            c.awake += awake ? 1u : 0u;
        }
        return c;
    }

    void Despawn(Simulation& simulation, scene::Entity entity, scene::DespawnReason reason) {
        scene::Scene& scene = simulation.Scene();
        if (scene::NetId* id = scene.TryGet<scene::NetId>(entity)) id->despawn = reason;
        scene.Destroy(entity);
    }

    namespace {
        /// The piece the live cap evicts: the oldest resting one (birth tick, then id), else the
        /// smallest awake one (solid voxels, then id). kNoEntity when there is no piece.
        scene::Entity EvictionCandidate(Simulation& simulation, bool& resting) {
            const entt::registry& registry = simulation.Scene().Registry();
            const physics::PhysicsWorld& physics = simulation.Physics();
            scene::Entity best = scene::kNoEntity;
            std::tuple<int, uint32_t, uint32_t> bestKey{2, 0, 0};
            for (const auto [entity, id] : registry.view<const scene::NetId>().each()) {
                if (registry.all_of<scene::Mechanism>(entity)) continue;
                if (registry.all_of<scene::Held>(entity)) continue;    // posed by its holder: never evicted
                if (!id.piece) continue;
                const scene::RigidBody* body = registry.try_get<scene::RigidBody>(entity);
                const bool awake = body && IsAwake(physics, body->body);
                uint32_t size = 0;
                if (const scene::VoxelBody* voxels = registry.try_get<scene::VoxelBody>(entity); voxels && voxels->model)
                    size = voxels->model->SolidCount();
                const std::tuple<int, uint32_t, uint32_t> key = awake ? std::make_tuple(1, size, id.value) : std::make_tuple(0, id.bornTick, id.value);
                if (best == scene::kNoEntity || key < bestKey) {
                    best = entity;
                    bestKey = key;
                }
            }
            resting = std::get<0>(bestKey) == 0;
            return best;
        }
    }

    PieceSlot TakePieceSlot(Simulation& simulation) {
        ReplicationState& r = simulation.Replication();
        const ReplicationConfig& config = simulation.Config().replication;
        if (r.budgetTick != simulation.Tick()) {
            r.budgetTick = simulation.Tick();
            r.bodiesThisTick = 0;
        }
        if (r.refuseNewPieces) {
            ++r.stats.overByteBudget;
            return PieceSlot::OverByteBudget;
        }
        if (r.bodiesThisTick >= config.bodiesPerTick) {
            ++r.stats.overTickBudget;
            return PieceSlot::OverTickBudget;
        }
        PieceCounts counts = CountPieces(simulation);
        counts.live += r.pendingPieces;
        counts.awake += r.pendingPieces;
        counts.awakeBodies += r.pendingPieces;
        r.stats.maxLive = std::max(r.stats.maxLive, counts.live);
        r.stats.maxAwake = std::max(r.stats.maxAwake, counts.awakeBodies);
        // Every awake replicated body shares the snapshots, so every one counts (review of stage 6).
        if (counts.awakeBodies >= config.maxAwakePieces) {
            ++r.stats.overAwakeCap;
            return PieceSlot::OverAwakeCap;
        }
        while (counts.live >= config.maxLivePieces) {
            bool resting = false;
            const scene::Entity victim = EvictionCandidate(simulation, resting);
            if (victim == scene::kNoEntity) break;     // every live piece is still pending in this blast
            Despawn(simulation, victim, scene::DespawnReason::ToRubble);
            ++(resting ? r.stats.evictedResting : r.stats.evictedAwake);
            --counts.live;
        }
        ++r.bodiesThisTick;
        ++r.stats.pieces;
        return PieceSlot::Replicate;
    }
}
