module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
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

// Client::Impl: mechanism copies and holding (docs/design/mechanisms.md "Replication", "Prediction and replication"). [mech-1]
namespace vortex::net {
    using namespace detail;

    namespace {
        constexpr uint8_t kHoldButtons = sim::PlayerButton_Use | sim::PlayerButton_Cast;
        constexpr uint32_t kNoticeTicks = 60;
    }

    uint64_t Client::Impl::CheckHash(const Copy& c) const {
        const scene::Scene& scene = simulation->Scene();
        if (c.mechanism)
            if (const scene::Mechanism* m = scene.TryGet<scene::Mechanism>(c.entity); m && m->track.count > 0)
                return c.modelHash ^ MechanismTerm(m->track.Newest(), m->track.Newest().flags, m->modelOffset);
        if (c.held && c.holdConfirmed) return c.modelHash ^ HeldTerm(c.holder, c.relYaw, c.holdSince);
        return c.modelHash;
    }

    void Client::Impl::InstallMechanism(uint32_t netId, const MechanismState& state) {
        const auto it = copies.find(netId);
        if (it == copies.end() || state.segments.empty()) return;
        Copy& c = it->second;
        scene::Scene& scene = simulation->Scene();
        scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(c.entity);
        const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(c.entity);
        if (!rigid || !voxels || !voxels->model) return;
        scene::Mechanism m;
        m.desc = state.desc;
        m.spawnVoxel = state.spawnVoxel;
        m.quarterTurns = state.quarterTurns;
        m.modelOffset = state.modelOffset;
        m.linkedNetId = state.linkedNetId;
        for (const scene::MechanismSegment& s : state.segments) sim::PushSegment(m.track, s);
        m.swept = sim::SweptBounds(m, voxels->model->Volume().Size(), simulation->World().VoxelSize());
        rigid->motion = physics::MotionType::Kinematic;
        scene.Registry().emplace_or_replace<scene::Mechanism>(c.entity, m);
        c.mechanism = true;
        c.resting = true;
        c.motion = static_cast<uint8_t>(physics::MotionType::Kinematic);
        RelinkMechanisms();
        PoseMechanismCopy(c, PoseTick());
    }

    void Client::Impl::RelinkMechanisms() {
        scene::Scene& scene = simulation->Scene();
        for (const auto& [netId, c] : copies) {
            if (!c.mechanism) continue;
            scene::Mechanism* m = scene.TryGet<scene::Mechanism>(c.entity);
            if (!m) continue;
            m->linked = scene::kNoEntity;
            if (const auto t = copies.find(m->linkedNetId); m->linkedNetId != 0 && t != copies.end() && t->second.mechanism) m->linked = t->second.entity;
        }
    }

    void Client::Impl::PoseMechanismCopy(const Copy& c, uint32_t tick) {
        scene::Scene& scene = simulation->Scene();
        const scene::Mechanism* m = scene.TryGet<scene::Mechanism>(c.entity);
        if (!m || m->track.count == 0) return;
        const sim::MechanismPose pose = sim::PoseAt(*m, sim::PositionAt(*m, tick), simulation->World().VoxelSize());
        scene.SetTransform(c.entity, glm::vec3(pose.position), pose.rotation, true);
        const physics::BodyId body = BodyOf(c.entity);
        simulation->Physics().SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
        simulation->Physics().SetLayer(body, sim::MechanismLayer(*m, tick));
    }

    void Client::Impl::PrepareStep(uint32_t tick) {
        FreshColliders();
        sim::PoseMechanisms(*simulation, tick, sim::PlayerReach(player.position, welcome.player));
        sim::PushOutOfMechanisms(*simulation, tick, player, character, welcome.player);
    }

    uint32_t Client::Impl::NetIdOfEntity(scene::Entity entity) const {
        if (entity == scene::kNoEntity) return 0;
        for (const auto& [netId, c] : copies)
            if (c.entity == entity) return netId;
        return 0;
    }

    void Client::Impl::ApplyHold(uint32_t netId, uint8_t holder, uint16_t relYaw, uint32_t tick) {
        const auto it = copies.find(netId);
        if (it == copies.end()) return;
        Copy& c = it->second;
        sim::HoldChange change;
        change.entity = c.entity;
        change.hold = true;
        change.holder = holder;
        change.relYaw = relYaw;
        change.tick = tick;
        sim::ApplyHoldChange(*simulation, c.entity, change);
        c.held = true;
        c.holdConfirmed = true;
        c.holder = holder;
        c.relYaw = relYaw;
        c.holdSince = tick;
        c.resting = false;
        bodies.Restart(netId, tick);
        pendingReleases.erase(netId);
        if (holder != welcome.slot) {
            if (heldNetId == netId) heldNetId = 0;           // another player's now: the own prediction lost it
            if (holdPending && holdPending->netId == netId) holdPending.reset();
            return;
        }
        if (holdPending && !holdPending->drop && holdPending->netId != netId) UndoPickup();
        heldNetId = netId;
        if (holdPending && !holdPending->drop) {
            holdPending.reset();
            latchedOut = latchedEdges;        // the presses made while it was pending go out now
            latchedEdges = 0;
        }
        ++stats.holdsConfirmed;
    }

    void Client::Impl::ApplyRelease(const EntityRelease& r) {
        pendingReleases.erase(r.netId);
        const auto it = copies.find(r.netId);
        if (it == copies.end()) return;
        Copy& c = it->second;
        sim::HoldChange change;
        change.entity = c.entity;
        change.hold = false;
        change.tick = r.tick;
        change.reason = r.reason;
        change.position = r.position;
        change.rotation = r.rotation;
        sim::ApplyHoldChange(*simulation, c.entity, change);
        const bool own = c.holder == welcome.slot || heldNetId == r.netId;
        c.held = false;
        c.holdConfirmed = false;
        c.resting = false;
        const physics::BodyId body = BodyOf(c.entity);
        simulation->Physics().SetLayer(body, MovingLayer(body));
        const BodyPose seed{r.position, r.rotation, r.velocity};
        bodies.Restart(r.netId, r.tick, &seed);
        if (!own) return;
        if (heldNetId == r.netId) heldNetId = 0;
        if (holdPending && holdPending->netId == r.netId) holdPending.reset();
        heldPinned = false;
        if (r.reason == static_cast<uint8_t>(sim::HoldEnd::Pinned) || r.reason == static_cast<uint8_t>(sim::HoldEnd::Damaged)) {
            holdNotice = r.reason == static_cast<uint8_t>(sim::HoldEnd::Pinned) ? "Dropped - blocked" : "Dropped - hit";
            holdNoticeTicks = kNoticeTicks;
        }
        ++stats.holdReleases;
    }

    void Client::Impl::PredictHold(const sim::PlayerStepResult& r, const sim::PlayerInput& input) {
        const uint8_t pressed = static_cast<uint8_t>(r.pressed & kHoldButtons);
        if (pressed == 0 || holdPending) return;
        if (heldNetId != 0) {
            // A drop or a throw: the copy hangs where it is (Layer::Held) until the server's EntityRelease.
            holdPending = HoldPending{heldNetId, input.tick, true};
            ++stats.dropsPredicted;
            return;
        }
        if ((pressed & sim::PlayerButton_Use) == 0) return;
        const sim::UseTargetHooks hooks = MakeUseHooks();
        const glm::dvec3 eye = glm::dvec3(player.position) + glm::dvec3(0.0, static_cast<double>(welcome.player.eyeHeight), 0.0);
        sim::UseTarget t;
        if (!sim::FindUseTarget(*simulation, eye, sim::LookDirection(input.yaw, input.pitch), input.tick, t, &hooks) ||
            t.kind != sim::UseTargetKind::Holdable)
            return;
        // What the own player stands on is not a pick-up (the server refuses it Busy): sent unpredicted.
        if (BodyOf(t.entity) == simulation->Physics().GetCharacterState(character).groundBody) {
            holdNotice = "Occupied";
            holdNoticeTicks = kNoticeTicks;
            return;
        }
        const uint32_t netId = NetIdOfEntity(t.entity);
        const auto it = copies.find(netId);
        if (it == copies.end()) return;
        Copy& c = it->second;
        const scene::Transform* transform = simulation->Scene().TryGet<scene::Transform>(c.entity);
        HoldPending p{netId, input.tick, false};
        if (transform) {
            p.position = transform->current.position;
            p.rotation = transform->current.rotation;
        }
        sim::HoldChange change;
        change.entity = c.entity;
        change.hold = true;
        change.holder = welcome.slot;
        change.relYaw = sim::RelYawAtPickup(p.rotation, input.yaw);
        change.tick = input.tick;
        sim::ApplyHoldChange(*simulation, c.entity, change);
        c.held = true;
        c.holdConfirmed = false;
        c.holder = welcome.slot;
        c.relYaw = change.relYaw;
        c.holdSince = input.tick;
        holdPending = p;
        heldNetId = netId;
        latchedEdges = 0;
        ++stats.pickupsPredicted;
    }

    sim::UseTargetHooks Client::Impl::MakeUseHooks() const {
        sim::UseTargetHooks hooks;
        hooks.holdable = [this](scene::Entity e) {
            if (!sim::IsHoldableShape(*simulation, e)) return false;
            const auto it = copies.find(NetIdOfEntity(e));
            return it != copies.end() && it->second.motion == static_cast<uint8_t>(physics::MotionType::Dynamic) && !it->second.held;
        };
        hooks.key = [this](scene::Entity e) { return NetIdOfEntity(e); };
        return hooks;
    }

    void Client::Impl::ShowLeadCue(const Copy& c) {
        const scene::Mechanism* m = simulation->Scene().TryGet<scene::Mechanism>(c.entity);
        if (!m || m->track.count == 0) return;
        const float vs = simulation->World().VoxelSize();
        const sim::MechanismPose pose = sim::PoseAt(*m, sim::PositionAt(*m, PoseTick()), vs);
        glm::vec3 local = glm::vec3(m->desc.pivotQ) / 16.0f;
        if (m->desc.kind == scene::MechanismKind::Portcullis)
            local = glm::vec3((m->desc.blockBox.min.x + m->desc.blockBox.max.x) * 0.5f, static_cast<float>(m->desc.blockBox.max.y),
                              (m->desc.blockBox.min.z + m->desc.blockBox.max.z) * 0.5f);
        local -= glm::vec3(m->modelOffset);
        const glm::dvec3 at = pose.position + glm::dvec3(pose.rotation * (local * vs));
        fx::SpawnVoxelBreak(simulation->Particles(), at, glm::vec3(0.40f, 0.29f, 0.18f), vs, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    void Client::Impl::UndoPickup() {
        if (!holdPending || holdPending->drop) return;
        const HoldPending p = *holdPending;
        holdPending.reset();
        latchedEdges = 0;
        if (heldNetId == p.netId) heldNetId = 0;
        ++stats.holdRefused;
        const auto it = copies.find(p.netId);
        if (it == copies.end()) return;
        Copy& c = it->second;
        if (c.holdConfirmed) return;
        scene::Scene& scene = simulation->Scene();
        scene.Registry().remove<scene::Held>(c.entity);
        c.held = false;
        const physics::BodyId body = BodyOf(c.entity);
        scene.SetTransform(c.entity, p.position, p.rotation, true);
        simulation->Physics().SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
        simulation->Physics().SetLayer(body, c.resting ? physics::Layer::Dynamic : MovingLayer(body));
    }

    void Client::Impl::ResolveHold(const Snapshot& snap) {
        if (holdPending && !holdPending->drop && snap.lastUseTick >= holdPending->inputTick) {
            const auto it = copies.find(holdPending->netId);
            if (it == copies.end() || !it->second.holdConfirmed) UndoPickup();
        }
    }

    void Client::Impl::PoseHeld(const sim::PlayerInput& input) {
        if (holdNoticeTicks > 0 && --holdNoticeTicks == 0) holdNotice.clear();
        bool any = heldNetId != 0 || !pendingReleases.empty();
        for (const auto& [netId, c] : copies) any = any || c.held;
        if (!any) return;
        const sim::HoldEnv env{welcome.player, playArea, {}};
        const uint32_t tick = predicting ? nextTick - 1 : PoseTick();
        heldPinned = false;
        // The own held copy follows the predicted player (not while a drop is pending: it hangs).
        if (heldNetId != 0 && predicting && !(holdPending && holdPending->drop))
            if (const auto it = copies.find(heldNetId); it != copies.end() && it->second.held) {
                const int8_t pitch = sim::HoldPitch(input.pitch);
                heldPinned = sim::HeldPoseAt(*simulation, it->second.entity, player.position, input.yaw, pitch, env, tick).pinned;
                sim::StepHeld(*simulation, it->second.entity, player, input.yaw, pitch, env, tick, false);
            }
        // Other holders: from the player as it is drawn; a release waits for the drawn holder to reach its tick.
        const std::vector<RemotePlayerView> remotesNow = remotes.Sample(serverTime.Now());
        std::vector<EntityRelease> due;
        for (auto& [netId, p] : pendingReleases) {
            ++p.waited;
            const auto c = copies.find(netId);
            // The copy's own interpolation time (MoveCopies samples it there next tick) has reached the release: from
            // here it moves on from the release pose into the server's samples without a jump.
            const double bodyAt = bodyTime + 1.0 - bodies.Delay(netId);
            const bool reached = p.waited >= static_cast<uint32_t>(RemoteBodies::kMaxDelayTicks) || c == copies.end() ||
                                 bodyAt >= static_cast<double>(p.release.tick);
            if (reached) due.push_back(p.release);
        }
        for (const EntityRelease& r : due) ApplyRelease(r);
        for (const auto& [netId, c] : copies) {
            if (!c.held || c.holder == welcome.slot) continue;
            for (const RemotePlayerView& v : remotesNow) {
                if (v.slot != c.holder) continue;
                const sim::HeldPose p = sim::HeldPoseAt(*simulation, c.entity, v.position, v.yaw, v.holdPitch, env, tick);
                simulation->Scene().SetTransform(c.entity, glm::vec3(p.pose.position), p.pose.rotation, true);
            }
        }
    }
}
