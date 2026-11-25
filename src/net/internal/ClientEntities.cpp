module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

module vortex.net;

import vortex.core;
import vortex.math;
import vortex.fx;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;
import :client_impl;

// Client::Impl: replicated bodies (copies, rubble, tombstones, samples, the baseline) (docs/design/rules.md "File map"). [wp-6]
namespace vortex::net {
    using namespace detail;

    physics::BodyId Client::Impl::BodyOf(scene::Entity entity) const {
        const scene::RigidBody* body = simulation->Scene().TryGet<scene::RigidBody>(entity);
        return body ? body->body : physics::BodyId{};
    }

    physics::Layer Client::Impl::MovingLayer(physics::BodyId body) const {
        if (!predicting || !body.IsValid()) return physics::Layer::Rubble;
        return simulation->Physics().GetCharacterState(character).groundBody == body ? physics::Layer::Dynamic : physics::Layer::Rubble;
    }

    void Client::Impl::Tombstone(uint32_t netId) {
        if (!tombstones.insert(netId).second) return;
        tombstoneOrder.push_back(netId);
        while (tombstoneOrder.size() > std::max<size_t>(1, settings.maxTombstones)) {
            tombstones.erase(tombstoneOrder.front());
            tombstoneOrder.pop_front();
            ++stats.tombstonesEvicted;
        }
    }

    void Client::Impl::Untombstone(uint32_t netId) {
        if (tombstones.erase(netId) == 0) return;
        if (const auto it = std::find(tombstoneOrder.begin(), tombstoneOrder.end(), netId); it != tombstoneOrder.end()) tombstoneOrder.erase(it);
    }

    bool Client::Impl::MakeCopy(uint32_t netId, std::shared_ptr<voxel::VoxelModel> model, const glm::vec3& position, const glm::quat& rotation,
                  bool resting, uint32_t base, uint8_t motion) {
        if (copies.contains(netId) || tombstones.contains(netId)) {
            ++stats.ignoredEntityEvents;
            return false;
        }
        const uint64_t hash = VolumeHash(model->Volume());
        scene::Scene& scene = simulation->Scene();
        const scene::Entity entity = scene.CreateVoxelObject(
            {.model = std::move(model), .position = position, .rotation = rotation, .motion = physics::MotionType::Kinematic});
        if (entity == scene::kNoEntity) {
            ++stats.copiesFailed;
            Log::Error("Client: no copy of server object {} could be made (store or bodies full)", netId);
            return false;
        }
        simulation->Physics().SetLayer(BodyOf(entity), resting ? physics::Layer::Dynamic : physics::Layer::Rubble);
        Copy c;
        c.entity = entity;
        c.resting = resting;
        c.modelHash = hash;
        c.base = base;
        c.motion = motion;
        copies[netId] = c;
        ++stats.copiesMade;
        return true;
    }

    void Client::Impl::MakeRubble(std::shared_ptr<voxel::VoxelModel> model, const glm::vec3& position, const glm::quat& rotation,
                    const glm::vec3& linear, const glm::vec3& angular) {
        simulation->Scene().CreateVoxelObject({.model = std::move(model),
                                               .position = position,
                                               .rotation = rotation,
                                               .linearVelocity = linear,
                                               .angularVelocity = angular,
                                               .debris = true});
    }

    void Client::Impl::ApplyEntity(uint32_t seq, const Event& event) {
        const bool live = settings.presentation && seq > welcome.joinOpSeq;
        scene::Scene& scene = simulation->Scene();
        physics::PhysicsWorld& physics = simulation->Physics();
        if (const EntitySpawn* spawn = std::get_if<EntitySpawn>(&event)) {
            if (spawn->kind == EntityKind::Light) {
                if (!entities.contains(spawn->netId)) {
                    const sim::TemplateLight& l = spawn->light;
                    entities[spawn->netId] =
                        scene.CreateLight({.position = l.position, .color = l.color, .intensity = l.intensity, .radius = l.radius});
                    ++stats.lightsSpawned;
                }
                return;
            }
            const ObjectSpawn& o = spawn->object;
            auto model = std::make_shared<voxel::VoxelModel>(o.volume, voxel::kWorldPalette, spawn->netId != 0 ? "object" : "debris");
            if (spawn->netId == 0) {
                ++stats.fragments;
                if (live) MakeRubble(std::move(model), o.position, o.rotation, o.linearVelocity, o.angularVelocity);
                return;
            }
            if (!MakeCopy(spawn->netId, std::move(model), o.position, o.rotation, (o.flags & kObjectResting) != 0, seq, o.motion)) return;
            if (spawn->kind == EntityKind::Mechanism) InstallMechanism(spawn->netId, spawn->mechanism);
            if ((o.flags & kObjectHeld) != 0) ApplyHold(spawn->netId, o.holder, o.relYaw, o.holdSince);
            return;
        }
        // Rest, wake and model of a NetId: dropped when its copy is already as of this event (the
        // baseline or a model answer was encoded after it), held while its copy may still come.
        if (!std::holds_alternative<EntityDespawn>(event)) {
            const uint32_t netId = EntityEventNetId(event);
            const auto copy = copies.find(netId);
            if (copy != copies.end() && seq <= copy->second.base) {
                ++stats.staleEntityEvents;
                return;
            }
            if (copy == copies.end() && !tombstones.contains(netId) && (!baselineEnded || asked.contains(netId))) {
                Hold(netId, seq, event);
                return;
            }
        }
        if (const EntityDespawn* d = std::get_if<EntityDespawn>(&event)) {
            Tombstone(d->netId);
            bodies.Remove(d->netId);
            asked.erase(d->netId);
            staleBefore.erase(d->netId);
            if (const auto h = held.find(d->netId); h != held.end()) {
                stats.droppedHeldEvents += static_cast<uint32_t>(h->second.size());
                heldCount -= h->second.size();
                held.erase(h);
            }
            const auto it = copies.find(d->netId);
            if (it == copies.end()) {
                ++stats.ignoredEntityEvents;
                return;
            }
            const scene::Entity entity = it->second.entity;
            const bool wasMechanism = it->second.mechanism;
            copies.erase(it);
            ++stats.despawns;
            pendingReleases.erase(d->netId);
            if (heldNetId == d->netId) heldNetId = 0;
            if (holdPending && holdPending->netId == d->netId) holdPending.reset();
            if (wasMechanism) RelinkMechanisms();
            if (d->reason == scene::DespawnReason::Broken && live && castShown) {
                // Broken by a blast: what the blast took of it bursts (its fragments, the rest,
                // came as netId-0 spawns), as Scene::Damage shows offline.
                const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
                const scene::Transform* transform = scene.TryGet<scene::Transform>(entity);
                if (voxels && voxels->model && transform)
                    ShowVoxelsInSphere(voxels->model->Volume(), transform->current.position, transform->current.rotation);
            }
            if (d->reason == scene::DespawnReason::ToRubble && live) {
                // The server let go of it (piece caps): it stays here as this client's rubble.
                const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
                const scene::Transform* transform = scene.TryGet<scene::Transform>(entity);
                if (voxels && voxels->model && transform) {
                    const physics::BodyState state = physics.GetState(BodyOf(entity));
                    MakeRubble(voxels->model, transform->current.position, transform->current.rotation, state.linearVelocity,
                               state.angularVelocity);
                    ++stats.toRubble;
                }
            }
            scene.Destroy(entity);
            return;
        }
        if (const EntityRest* r = std::get_if<EntityRest>(&event)) {
            const bool rests = bodies.Rest(r->netId, r->tick);
            const auto it = copies.find(r->netId);
            if (it == copies.end()) {
                ++stats.ignoredEntityEvents;
                return;
            }
            if (it->second.held || it->second.mechanism) {
                ++stats.ignoredEntityEvents;
                return;
            }
            if (!rests) {
                // A sample newer than the rest was taken already (it cannot be, as samples wait for
                // their snapshot's events; kept as a guard): the copy keeps moving.
                ++stats.staleRests;
                return;
            }
            Copy& c = it->second;
            c.resting = true;
            const physics::BodyId body = BodyOf(c.entity);
            scene.SetTransform(c.entity, r->position, r->rotation, false);
            physics.SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
            physics.SetLayer(body, physics::Layer::Dynamic);
            ++stats.rests;
            return;
        }
        if (const EntityWake* k = std::get_if<EntityWake>(&event)) {
            const auto it = copies.find(k->netId);
            if (it == copies.end()) {
                ++stats.ignoredEntityEvents;
                return;
            }
            if (it->second.held || it->second.mechanism) {
                ++stats.ignoredEntityEvents;
                return;
            }
            // Moving from here on: not solid for a predicted player (unless it carries this
            // one), still where it rested until its samples come (RemoteBodies keeps the rest's
            // tick, so older samples stay refused).
            Copy& c = it->second;
            c.resting = false;
            const physics::BodyId body = BodyOf(c.entity);
            physics.SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
            physics.SetLayer(body, MovingLayer(body));
            ++stats.wakes;
            return;
        }
        if (const MechanismMove* mv = std::get_if<MechanismMove>(&event)) {
            const auto it = copies.find(mv->netId);
            scene::Mechanism* mech = it != copies.end() && it->second.mechanism ? scene.TryGet<scene::Mechanism>(it->second.entity) : nullptr;
            if (!mech) {
                ++stats.ignoredEntityEvents;
                return;
            }
            Copy& c = it->second;
            stats.mechanismLateEvents += predicting && nextTick > mv->segment.startTick ? 1u : 0u;
            sim::PushSegment(mech->track, mv->segment);
            ++stats.mechanismMoves;
            if (live && (mv->segment.flags & scene::MechanismFlag_Freed) == 0) ShowLeadCue(c);
            if ((mv->segment.flags & scene::MechanismFlag_Freed) != 0) {
                // Off its hinge: an ordinary moving copy from here on (the server's wake, samples and rest drive it).
                scene.Registry().remove<scene::Mechanism>(c.entity);
                c.mechanism = false;
                c.resting = false;
                c.motion = static_cast<uint8_t>(physics::MotionType::Dynamic);
                const physics::BodyId body = BodyOf(c.entity);
                physics.SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
                physics.SetLayer(body, physics::Layer::Rubble);
                RelinkMechanisms();
            }
            return;
        }
        if (const EntityHold* h = std::get_if<EntityHold>(&event)) {
            if (!copies.contains(h->netId)) {
                ++stats.ignoredEntityEvents;
                return;
            }
            ApplyHold(h->netId, h->holder, h->relYaw, h->tick);
            return;
        }
        if (const EntityRelease* r = std::get_if<EntityRelease>(&event)) {
            const auto it = copies.find(r->netId);
            if (it == copies.end()) {
                ++stats.ignoredEntityEvents;
                return;
            }
            // Another holder's release waits until that holder is drawn at its tick (no jump on screen).
            const bool other = it->second.held && it->second.holder != welcome.slot && heldNetId != r->netId;
            if (other) pendingReleases[r->netId] = {*r, 0};
            else ApplyRelease(*r);
            return;
        }
        const ObjectModel& m = std::get<ObjectModel>(event);
        const auto it = copies.find(m.netId);
        if (it == copies.end()) {
            ++stats.ignoredEntityEvents;
            return;
        }
        // The survivor of a damage: a new model whose origin moved by `offset` voxels of the old
        // one (in its frame). A new copy there; its samples move with it.
        const Copy old = it->second;
        std::optional<scene::Mechanism> mechanism;
        if (old.mechanism)
            if (const scene::Mechanism* mm = scene.TryGet<scene::Mechanism>(old.entity)) mechanism = *mm;
        copies.erase(it);
        const scene::Transform* transform = scene.TryGet<scene::Transform>(old.entity);
        const glm::vec3 shift = glm::vec3(m.offset) * simulation->World().VoxelSize();
        glm::vec3 position = transform ? transform->current.position : glm::vec3(0.0f);
        const glm::quat rotation = transform ? transform->current.rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        if (live) {
            // What the damage removed, as particles (Scene::Damage spawns them offline; a client
            // never runs it): the old model's voxels the new one does not have.
            const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(old.entity);
            if (voxels && voxels->model && transform) ShowRemovedVoxels(voxels->model->Volume(), m, position, rotation);
        }
        position += rotation * shift;
        scene.Destroy(old.entity);
        // Only samples taken before this event are kept (later ones wait for it): all in the old
        // frame, so all are shifted.
        bodies.Shift(m.netId, shift);
        // The server's survivor is a new body, awake (Scene::Damage), unless it is static: a
        // resting copy is re-made moving, and the server's EntityWake / EntityRest follow.
        const bool staticModel = m.motion == static_cast<uint8_t>(physics::MotionType::Static);
        const bool made = MakeCopy(m.netId, std::make_shared<voxel::VoxelModel>(m.volume, voxel::kWorldPalette, "object"), position, rotation,
                                   staticModel && old.resting, seq, m.motion);
        if (made && mechanism) {
            // [mech-1] A dented mechanism keeps its state; its model origin moved by the offset.
            Copy& c = copies[m.netId];
            mechanism->modelOffset += m.offset;
            if (scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(c.entity)) rigid->motion = physics::MotionType::Kinematic;
            scene.Registry().emplace_or_replace<scene::Mechanism>(c.entity, *mechanism);
            c.mechanism = true;
            c.resting = true;
            RelinkMechanisms();
            PoseMechanismCopy(c, PoseTick());
        }
        ++stats.models;
    }

    void Client::Impl::ShowRemovedVoxels(const voxel::VoxelVolume& before, const ObjectModel& after, const glm::vec3& position, const glm::quat& rotation) {
        constexpr size_t kMaxDamageBursts = 64;
        const float vs = simulation->World().VoxelSize();
        std::vector<glm::ivec3> removed;
        const glm::ivec3 size = before.Size();
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x) {
                    const glm::ivec3 v(x, y, z);
                    if (before.Get(x, y, z) == 0) continue;
                    const glm::ivec3 n = v - after.offset;
                    const glm::ivec3 ns = after.volume.Size();
                    const bool kept = n.x >= 0 && n.y >= 0 && n.z >= 0 && n.x < ns.x && n.y < ns.y && n.z < ns.z && after.volume.Get(n.x, n.y, n.z) != 0;
                    if (!kept) removed.push_back(v);
                }
        const size_t stride = std::max<size_t>(1, (removed.size() + kMaxDamageBursts - 1) / kMaxDamageBursts);
        for (size_t i = 0; i < removed.size(); i += stride) {
            const glm::ivec3& v = removed[i];
            const glm::vec3 local = (glm::vec3(v) + glm::vec3(0.5f)) * vs;
            const glm::dvec3 at = glm::dvec3(position + rotation * local);
            fx::SpawnVoxelBreak(simulation->Particles(), at, simulation->Materials().Get(voxel::kWorldPalette, before.Get(v.x, v.y, v.z)).color, vs);
        }
    }

    void Client::Impl::ShowVoxelsInSphere(const voxel::VoxelVolume& volume, const glm::vec3& position, const glm::quat& rotation) {
        constexpr size_t kMaxDamageBursts = 64;
        const float vs = simulation->World().VoxelSize();
        const double reach = static_cast<double>(castRadius) + 0.5 * static_cast<double>(vs);
        std::vector<std::pair<glm::dvec3, uint8_t>> inside;
        const glm::ivec3 size = volume.Size();
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x) {
                    const uint8_t id = volume.Get(x, y, z);
                    if (id == 0) continue;
                    const glm::dvec3 at = glm::dvec3(position + rotation * ((glm::vec3(x, y, z) + glm::vec3(0.5f)) * vs));
                    if (glm::length(at - castCenter) <= reach) inside.emplace_back(at, id);
                }
        const size_t stride = std::max<size_t>(1, (inside.size() + kMaxDamageBursts - 1) / kMaxDamageBursts);
        for (size_t i = 0; i < inside.size(); i += stride)
            fx::SpawnVoxelBreak(simulation->Particles(), inside[i].first, simulation->Materials().Get(voxel::kWorldPalette, inside[i].second).color, vs);
    }

    void Client::Impl::TakeBodySamples(uint32_t tick, const std::vector<BodySample>& samples) {
        for (const BodySample& b : samples) {
            const auto it = copies.find(b.netId);
            if (it == copies.end() || it->second.resting || it->second.held || it->second.mechanism ||
                !bodies.Add(b.netId, tick, BodyPose{b.position, b.rotation, b.velocity})) {
                ++stats.bodySamplesIgnored;
                continue;
            }
            ++stats.bodySamples;
        }
    }

    void Client::Impl::TakeReadySamples() {
        if (state != ClientState::Playing || !simulation) return;
        while (!waitingSamples.empty() && waitingSamples.front().lastOpSeq <= applied) {
            TakeBodySamples(waitingSamples.front().tick, waitingSamples.front().bodies);
            waitingSamples.pop_front();
        }
    }

    void Client::Impl::MoveCopies() {
        const double now = serverTime.Now();
        if (!bodyTimeStarted) {
            bodyTime = now;
            bodyTimeStarted = serverTime.Started();
        } else {
            // One tick per call, pulled gently towards the server time (it advances per frame).
            const double error = now - bodyTime;
            bodyTime += std::abs(error) > 30.0 ? error : 1.0 + std::clamp(error * 0.1, -0.5, 0.5);
        }
        physics::PhysicsWorld& physics = simulation->Physics();
        const float dt = Dt();
        const physics::BodyId ground = predicting ? physics.GetCharacterState(character).groundBody : physics::BodyId{};
        // A copy only the server may remove; if this scene removed one by another path (it fell
        // below the window first, drawn in the past), it is lost here: counted, and the entity
        // check sees it missing until the server's despawn comes.
        for (auto it = copies.begin(); it != copies.end();) {
            if (simulation->Scene().IsValid(it->second.entity)) {
                ++it;
                continue;
            }
            ++stats.copiesLost;
            bodies.Remove(it->first);
            it = copies.erase(it);
        }
        for (const auto& [netId, c] : copies) {
            if (c.resting || c.held || c.mechanism) continue;
            const physics::BodyId body = BodyOf(c.entity);
            // Solid only while it carries the own player (MovingLayer; the server's rule).
            const physics::Layer layer = ground.IsValid() && ground == body ? physics::Layer::Dynamic : physics::Layer::Rubble;
            if (physics.GetLayer(body) != layer) physics.SetLayer(body, layer);
            stats.heldUnderPlayer += layer == physics::Layer::Dynamic ? 1u : 0u;
            BodyPose pose;
            if (bodies.Sample(netId, bodyTime + 1.0, static_cast<double>(dt), pose)) physics.MoveKinematic(body, pose.position, pose.rotation, dt);
            else physics.SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
        }
    }

    void Client::Impl::OnBaselineEntity(const BaselineEntity& b) {
        ++stats.baselineEntities;
        const uint32_t netId = b.spawn.netId;
        if (const auto stale = staleBefore.find(netId); stale != staleBefore.end()) {
            if (b.asOfOpSeq < stale->second && !tombstones.contains(netId)) {
                // Made before events of it this client dropped: the re-asked answer follows.
                ++stats.staleAnswers;
                return;
            }
            staleBefore.erase(stale);
        }
        entityAsOf = std::max(entityAsOf, b.asOfOpSeq);
        asked.erase(netId);
        if (b.spawn.kind == EntityKind::Light) {
            if (!entities.contains(netId) && !tombstones.contains(netId)) {
                const sim::TemplateLight& l = b.spawn.light;
                entities[netId] = simulation->Scene().CreateLight({.position = l.position, .color = l.color, .intensity = l.intensity, .radius = l.radius});
                ++stats.lightsSpawned;
            } else {
                ++stats.baselineDiscarded;
            }
            return;
        }
        const ObjectSpawn& o = b.spawn.object;
        const bool resting = b.restTick != BaselineEntity::kAwake;
        if (!MakeCopy(netId, std::make_shared<voxel::VoxelModel>(o.volume, voxel::kWorldPalette, "object"), o.position, o.rotation, resting,
                      b.asOfOpSeq, o.motion)) {
            ++stats.baselineDiscarded;
        } else if (b.spawn.kind == EntityKind::Mechanism) {
            InstallMechanism(netId, b.spawn.mechanism);
        } else if ((o.flags & kObjectHeld) != 0) {
            ApplyHold(netId, o.holder, o.relYaw, o.holdSince);
        } else if (resting) {
            bodies.Rest(netId, b.restTick);      // samples of its rest's tick or older stay refused
        } else {
            bodies.Add(netId, b.tick, BodyPose{o.position, o.rotation, o.linearVelocity});
        }
        // The events held for it: newer than the copy's state applied, the others dropped.
        const auto h = held.find(netId);
        if (h == held.end()) return;
        std::vector<std::pair<uint32_t, Event>> list = std::move(h->second);
        heldCount -= list.size();
        held.erase(h);
        for (const auto& [seq, event] : list) {
            if (copies.contains(netId) && seq > copies[netId].base) ApplyEntity(seq, event);
            else ++stats.droppedHeldEvents;
        }
    }
}
