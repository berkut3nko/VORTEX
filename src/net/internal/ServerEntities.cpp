module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

module vortex.net;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;
import :server_impl;

// Server::Impl: replicated bodies, their events and rest states, crates (docs/design/rules.md "File map"). [wp-6]
namespace vortex::net {
    using namespace detail;

    void Server::Impl::OnNetIdDestroyed(entt::registry& registry, entt::entity entity) {
        const scene::NetId& id = registry.get<scene::NetId>(entity);
        despawned.push_back({id.value, id.despawn});
    }

    void Server::Impl::DrainDespawns() {
        std::vector<EntityDespawn> queue = std::move(despawned);
        despawned.clear();
        for (const EntityDespawn& d : queue) {
            tracks.erase(d.netId);
            for (auto& [pid, p] : peers)
                if (p.player) std::erase(p.player->crates, d.netId);
            if (failed) continue;
            Emit(d);
            ++stats.despawns[std::min<size_t>(static_cast<size_t>(d.reason), 3)];
        }
    }

    bool Server::Impl::DescribeObject(scene::Entity entity, ObjectSpawn& out) {
        scene::Scene& scene = simulation->Scene();
        const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
        const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(entity);
        if (!voxels || !voxels->model || !body) return false;
        const physics::PhysicsWorld& physics = simulation->Physics();
        const physics::BodyState state = physics.GetState(body->body);
        out.position = state.position;
        out.rotation = state.rotation;
        out.linearVelocity = state.frozen ? state.keptLinearVelocity : state.linearVelocity;
        out.angularVelocity = state.angularVelocity;
        out.motion = static_cast<uint8_t>(body->motion);
        out.flags = sim::IsAwake(physics, body->body) ? 0 : kObjectResting;
        out.volume = voxels->model->Volume();
        return true;
    }

    void Server::Impl::EmitObjectSpawn(uint32_t netId, scene::Entity entity) {
        EntitySpawn spawn;
        spawn.netId = netId;
        spawn.kind = EntityKind::Object;
        if (!DescribeObject(entity, spawn.object)) return;
        BodyTrack& t = tracks[netId];
        t.resting = (spawn.object.flags & kObjectResting) != 0;
        if (t.resting) {
            // Created at rest: its rest is the spawn (no EntityRest follows until it moved).
            t.restTick = Tick();
            t.restPosition = spawn.object.position;
            t.restRotation = spawn.object.rotation;
        }
        Emit(spawn);
        ++stats.objectSpawns;
    }

    void Server::Impl::EmitObjectEvents(const sim::BlastResult& r) {
        for (const sim::AppliedOp& applied : r.ops)
            if (applied.op.kind == sim::WorldOpKind::Detach && applied.op.netId != 0) ++stats.detachBodies;
        for (const sim::ObjectEvent& e : r.objectEvents) {
            if (failed || !e.model) continue;
            if (e.kind == sim::ObjectEvent::Kind::Model) {
                // Over the entity byte budget the survivor is let go as client rubble (6 bytes)
                // instead of resending its whole model (review of stage 6).
                if (entityCredit <= 0 && simulation->Scene().IsValid(e.entity)) {
                    sim::Despawn(*simulation, e.entity, scene::DespawnReason::ToRubble);
                    ++stats.modelsToRubble;
                    continue;
                }
                ObjectModel m;
                m.netId = e.netId;
                m.offset = e.offset;
                m.motion = static_cast<uint8_t>(e.motion);
                m.volume = e.model->Volume();
                Emit(m);
                ++stats.objectModels;
                continue;
            }
            EntitySpawn spawn;
            spawn.netId = e.netId;
            spawn.kind = EntityKind::Object;
            spawn.object.position = e.pose.position;
            spawn.object.rotation = e.pose.rotation;
            spawn.object.linearVelocity = e.linearVelocity;
            spawn.object.angularVelocity = e.angularVelocity;
            spawn.object.motion = static_cast<uint8_t>(e.motion);
            spawn.object.volume = e.model->Volume();
            tracks[e.netId].resting = false;
            Emit(spawn);
            ++stats.objectSpawns;
        }
        for (const scene::DamageFragment& f : r.fragments) {
            if (failed || !f.model) continue;
            EntitySpawn spawn;
            spawn.kind = EntityKind::Object;
            spawn.object.position = f.pose.position;
            spawn.object.rotation = f.pose.rotation;
            spawn.object.linearVelocity = f.linearVelocity;
            spawn.object.angularVelocity = f.angularVelocity;
            spawn.object.volume = f.model->Volume();
            Emit(spawn);
            ++stats.fragmentSpawns;
        }
        for (const uint32_t lost : r.lostNetIds)
            if (!failed) {
                Emit(EntityDespawn{lost, scene::DespawnReason::Destroyed});
                ++stats.despawns[1];
            }
        DrainDespawns();
    }

    void Server::Impl::TrackBodies(uint32_t tick) {
        physics::PhysicsWorld& physics = simulation->Physics();
        std::vector<physics::BodyId> grounds;
        for (const auto& [pid, p] : peers)
            if (p.player)
                if (const physics::BodyId g = physics.GetCharacterState(p.player->character).groundBody; g.IsValid()) grounds.push_back(g);
        uint32_t awake = 0;
        for (const auto [entity, id, body] : simulation->Scene().Registry().view<const scene::NetId, const scene::RigidBody>().each()) {
            const bool moving = sim::IsAwake(physics, body.body);
            BodyTrack& t = tracks[id.value];
            awake += moving ? 1u : 0u;
            if (moving) {
                if (t.resting) {
                    t.resting = false;
                    Emit(EntityWake{id.value, tick});
                    ++stats.wakes;
                }
                const bool carries = std::find(grounds.begin(), grounds.end(), body.body) != grounds.end();
                if (body.motion != physics::MotionType::Static)
                    physics.SetLayer(body.body, carries ? physics::Layer::Dynamic : physics::Layer::Rubble);
            } else if (!t.resting) {
                t.resting = true;
                if (body.motion != physics::MotionType::Static) physics.SetLayer(body.body, physics::Layer::Dynamic);
                const physics::BodyState state = physics.GetState(body.body);
                t.restTick = tick;
                t.restPosition = state.position;
                t.restRotation = state.rotation;
                Emit(EntityRest{id.value, tick, state.position, state.rotation});
                ++stats.rests;
            }
        }
        stats.maxAwakeBodies = std::max(stats.maxAwakeBodies, awake);
    }

    scene::Entity Server::Impl::EntityOf(uint32_t netId) {
        for (const auto [entity, id] : simulation->Scene().Registry().view<const scene::NetId>().each())
            if (id.value == netId) return entity;
        return scene::kNoEntity;
    }

    uint32_t Server::Impl::SpawnCrateAt(const glm::dvec3& corner, Peer* owner) {
        if (!crateModel) crateModel = sim::CrateModel(simulation->WorldMaterialIds(), sim::PlaygroundScale(simulation->World().VoxelSize()));
        const uint32_t netId = SpawnObjectAt(crateModel, corner, owner);
        if (netId != 0) ++stats.crates;
        return netId;
    }

    uint32_t Server::Impl::SpawnObjectAt(const std::shared_ptr<voxel::VoxelModel>& model, const glm::dvec3& corner, Peer* owner) {
        voxel::World& world = simulation->World();
        const double vs = static_cast<double>(world.VoxelSize());
        const glm::dvec3 size = glm::dvec3(model->Volume().Size()) * vs;
        const double chunk = static_cast<double>(voxel::kChunkSize) * vs;
        const glm::dvec2 lo = glm::dvec2(region.playMinChunk) * chunk, hi = glm::dvec2(region.playMaxChunk) * chunk;
        const glm::ivec2 rows = sim::RegionRows(region);
        const bool inside = std::isfinite(corner.x) && std::isfinite(corner.y) && std::isfinite(corner.z) && corner.x >= lo.x &&
                            corner.z >= lo.y && corner.x + size.x <= hi.x && corner.z + size.z <= hi.y &&
                            corner.y >= static_cast<double>(rows.x) * chunk && corner.y + size.y <= static_cast<double>(rows.y) * chunk;
        // A player's crate shares the snapshots with every awake body: none at the awake cap.
        const bool room = !owner || sim::CountPieces(*simulation).awakeBodies < simulation->Config().replication.maxAwakePieces;
        const scene::Entity entity =
            inside && room ? simulation->Scene().CreateVoxelObject({.model = model, .position = glm::vec3(corner)}) : scene::kNoEntity;
        if (entity == scene::kNoEntity) {
            ++stats.cratesRefused;
            return 0;
        }
        const uint32_t netId = sim::Replicate(*simulation, entity, false);
        // Moving: no player collides with it until it rests (TrackBodies), here as on every client.
        if (const scene::RigidBody* body = simulation->Scene().TryGet<scene::RigidBody>(entity))
            simulation->Physics().SetLayer(body->body, physics::Layer::Rubble);
        EmitObjectSpawn(netId, entity);
        if (owner && owner->player) {
            owner->player->crates.push_back(netId);
            while (owner->player->crates.size() > settings.maxCratesPerPlayer) {
                const uint32_t oldest = owner->player->crates.front();
                owner->player->crates.pop_front();
                sim::Despawn(*simulation, EntityOf(oldest), scene::DespawnReason::Destroyed);
            }
        }
        DrainDespawns();
        return netId;
    }

    void Server::Impl::PlayerCrate(Peer& p, const sim::PlayerInput& input, const sim::PlayerState& at) {
        Player& pl = *p.player;
        const uint32_t now = Tick();
        if (pl.anyCrate && now - pl.lastCrateTick < settings.crateCooldownTicks) {
            ++stats.cratesRefused;
            return;
        }
        if (!crateModel) crateModel = sim::CrateModel(simulation->WorldMaterialIds(), sim::PlaygroundScale(simulation->World().VoxelSize()));
        const glm::dvec3 eye = glm::dvec3(at.position) + glm::dvec3(0.0, static_cast<double>(settings.player.eyeHeight), 0.0);
        const glm::dvec3 centre = eye + glm::dvec3(sim::LookDirection(input.yaw, input.pitch)) * static_cast<double>(settings.crateDistance);
        const glm::dvec3 half = glm::dvec3(crateModel->Volume().Size()) * (0.5 * static_cast<double>(simulation->World().VoxelSize()));
        if (SpawnCrateAt(centre - half, &p) != 0) {
            pl.anyCrate = true;
            pl.lastCrateTick = now;
        }
    }

    std::vector<BodySample> Server::Impl::AwakeSamples() {
        std::vector<BodySample> out;
        const physics::PhysicsWorld& physics = simulation->Physics();
        for (const auto [entity, id, body] : simulation->Scene().Registry().view<const scene::NetId, const scene::RigidBody>().each()) {
            const auto it = tracks.find(id.value);
            if (it == tracks.end() || it->second.resting) continue;
            const physics::BodyState state = physics.GetState(body.body);
            BodySample b;
            b.netId = id.value;
            b.position = state.position;
            b.rotation = state.rotation;
            b.velocity = state.frozen ? glm::vec3(0.0f) : state.linearVelocity;
            out.push_back(b);
        }
        std::sort(out.begin(), out.end(), [](const BodySample& a, const BodySample& b) { return a.netId < b.netId; });
        return out;
    }
}
