module;

#include <entt/entt.hpp>
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
#include <span>
#include <string>
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

// Server::Impl: mechanisms and holding (docs/design/mechanisms.md "Replication", "One use key: E", "Where it runs"). [mech-1]
namespace vortex::net {
    using namespace detail;

    static_assert(sim::kUseStaleTicks < kStateHistory, "a use is judged from the state of its input's tick");

    uint32_t Server::Impl::NetIdOf(scene::Entity entity) const {
        const scene::Scene& scene = simulation->Scene();
        if (!scene.IsValid(entity)) return 0;
        const scene::NetId* id = scene.TryGet<scene::NetId>(entity);
        return id ? id->value : 0;
    }

    void Server::Impl::BuildYard() {
        const bool hollowkeep = region.templateId == sim::RegionTemplateId::Hollowkeep;
        if (!hollowkeep && (!settings.session.mechanisms || region.templateId != sim::RegionTemplateId::Playground)) return;
        // Hollowkeep's portcullis and lever are the template's own (no operations: the land generator stamps the castle).
        const sim::RegionTemplate yard =
            hollowkeep ? tmpl : sim::MechanismYardTemplate(simulation->World(), simulation->WorldMaterialIds(), tmpl.anchor);
        if (!hollowkeep)
            for (const sim::WorldOp& op : yard.ops) {
                const sim::OpResult r = sim::ApplyOp(simulation->World(), op);
                if (!CheckOp(r)) return;
                EmitOp(op, r);
            }
        mechanisms = sim::CreateTemplateMechanisms(*simulation, yard);
        scene::Scene& scene = simulation->Scene();
        std::vector<uint32_t> ids;
        for (const scene::Entity e : mechanisms) ids.push_back(sim::Replicate(*simulation, e, false));
        for (size_t i = 0; i < mechanisms.size(); ++i) {
            if (scene::Mechanism* m = scene.TryGet<scene::Mechanism>(mechanisms[i])) m->linkedNetId = NetIdOf(m->linked);
            EmitMechanismSpawn(ids[i], mechanisms[i]);
        }
        stats.mechanisms = static_cast<uint32_t>(mechanisms.size());
        Log::Info("Mechanisms: server: {} built: {} mechanisms, {} operations", hollowkeep ? "Hollowkeep" : "yard", mechanisms.size(),
                  hollowkeep ? size_t{0} : yard.ops.size());
    }

    bool Server::Impl::MakeMechanismState(scene::Entity entity, uint32_t tick, MechanismState& out) const {
        const scene::Mechanism* m = simulation->Scene().TryGet<scene::Mechanism>(entity);
        if (!m || m->track.count == 0) return false;
        out = MechanismState{};
        out.desc = m->desc;
        out.spawnVoxel = m->spawnVoxel;
        out.quarterTurns = m->quarterTurns;
        out.modelOffset = m->modelOffset;
        out.linkedNetId = NetIdOf(m->linked);
        for (uint8_t i = 0; i < m->track.count; ++i) {
            const scene::MechanismSegment& s = m->track.ring[i];
            if (i + 1 == m->track.count || s.startTick + s.durationTicks >= tick) out.segments.push_back(s);
        }
        return true;
    }

    void Server::Impl::EmitMechanismSpawn(uint32_t netId, scene::Entity entity) {
        EntitySpawn spawn;
        spawn.netId = netId;
        spawn.kind = EntityKind::Mechanism;
        if (!DescribeObject(entity, spawn.object) || !MakeMechanismState(entity, Tick(), spawn.mechanism)) return;
        spawn.object.flags = kObjectResting;
        spawn.object.linearVelocity = glm::vec3(0.0f);
        spawn.object.angularVelocity = glm::vec3(0.0f);
        BodyTrack& t = tracks[netId];
        t.resting = true;
        t.restTick = Tick();
        t.restPosition = spawn.object.position;
        t.restRotation = spawn.object.rotation;
        Emit(spawn);
        ++stats.objectSpawns;
    }

    void Server::Impl::EmitMechanismChanges(std::span<const sim::MechanismChange> changes) {
        for (const sim::MechanismChange& c : changes) {
            const uint32_t netId = NetIdOf(c.entity);
            if (netId == 0 || failed) continue;
            Emit(MechanismMove{netId, c.segment});
            ++stats.mechanismMoves;
        }
    }

    uint32_t Server::Impl::LeadOf(PeerId id, const Player& pl) const {
        const double dt = static_cast<double>(simulation->FixedTimeStep());
        const uint32_t rtt = static_cast<uint32_t>(std::ceil(std::max(0.0, transport.RoundTripSeconds(id)) / dt));
        const uint32_t margin = pl.margin == kNoInputMargin ? 0u : static_cast<uint32_t>(std::max<int16_t>(pl.margin, 0));
        return std::clamp(rtt + margin + 2u, sim::kMechanismLeadMin, sim::kMechanismLeadMax);
    }

    uint32_t Server::Impl::UseLead() {
        uint32_t lead = sim::kMechanismLeadMin;
        for (const auto& [id, p] : peers)
            if (p.joined && p.player) lead = std::max(lead, LeadOf(id, *p.player));
        stats.useLeadMin = stats.useLeadMax == 0 ? lead : std::min(stats.useLeadMin, lead);
        stats.useLeadMax = std::max(stats.useLeadMax, lead);
        return lead;
    }

    void Server::Impl::PlayerUse(Peer& p, const sim::PlayerInput& in, const sim::PlayerState* at, uint32_t now) {
        Player& pl = *p.player;
        pl.lastUseTick = in.tick;
        // The echo of an auto-drop: an edge its client made before it saw the release is consumed.
        if (pl.InEcho(in.tick)) {
            pl.useResult = sim::UseResult::None;
            ++stats.echoEdges;
            return;
        }
        // A drop is never refused (not even a stale one): the hold is the server's state now.
        if (pl.held != scene::kNoEntity) {
            pl.useResult = sim::UseResult::None;
            ReleaseHeld(p, sim::HoldEnd::Drop, in.yaw, in.pitch);
            return;
        }
        if (at == nullptr || now - in.tick > sim::kUseStaleTicks) {
            pl.useResult = sim::UseResult::Stale;
            ++stats.uses;
            ++stats.usesRefused;
            return;
        }
        const glm::dvec3 eye = glm::dvec3(at->position) + glm::dvec3(0.0, static_cast<double>(settings.player.eyeHeight), 0.0);
        const glm::vec3 dir = sim::LookDirection(in.yaw, in.pitch);
        sim::UseTargetHooks hooks;
        hooks.holdable = [this](scene::Entity e) { return sim::IsHoldable(*simulation, e, true); };
        hooks.key = [this](scene::Entity e) { return NetIdOf(e); };
        sim::UseTarget t;
        const bool found = sim::FindUseTarget(*simulation, eye, dir, in.tick, t, &hooks);
        const sim::UseTargetKind kind = found ? t.kind : sim::UseTargetKind::None;
        if (kind == sim::UseTargetKind::Holdable) {
            bool stoodOn = false;
            for (const auto& [id, other] : peers)
                if (other.player && other.player->groundEntity == t.entity && now - other.player->groundTick <= sim::kHoldGroundTicks)
                    stoodOn = true;
            sim::HoldChange c;
            pl.useResult = sim::BeginHold(*simulation, t.entity, p.slot, in.yaw, now, pl.holdChangeTick, pl.anyHoldChange, stoodOn, true, c);
            if (pl.useResult != sim::UseResult::None) {
                ++stats.holdsRefused;
                return;
            }
            EmitHold(c);
            pl.held = t.entity;
            pl.holdChangeTick = now;
            pl.anyHoldChange = true;
            ++stats.pickups;
            return;
        }
        if (kind == sim::UseTargetKind::HeldByOther) {
            pl.useResult = sim::UseResult::Busy;
            ++stats.holdsRefused;
            return;
        }
        ++stats.uses;
        if (kind != sim::UseTargetKind::Mechanism && kind != sim::UseTargetKind::Info) {
            pl.useResult = sim::UseResult::NoTarget;
            ++stats.usesRefused;
            return;
        }
        std::vector<sim::MechanismChange> changes;
        pl.useResult = sim::ApplyUse(*simulation, t, eye, now, UseLead(), changes);
        if (pl.useResult != sim::UseResult::None) ++stats.usesRefused;
        EmitMechanismChanges(changes);
    }

    bool Server::Impl::HoldEdges(Peer& p, const sim::PlayerInput& in, uint8_t pressed) {
        Player& pl = *p.player;
        if ((pressed & sim::PlayerButton_Cast) == 0) return false;
        const bool use = (pressed & sim::PlayerButton_Use) != 0;
        if (pl.InEcho(in.tick)) {
            // An auto-drop's echo: the Cast edge (and a Use edge with it) was meant for the hold that ended.
            ++stats.echoEdges;
        } else if (pl.held != scene::kNoEntity) {
            ReleaseHeld(p, sim::HoldEnd::Throw, in.yaw, in.pitch);
        } else {
            return false;
        }
        if (use) {
            pl.lastUseTick = in.tick;
            pl.useResult = sim::UseResult::None;
        }
        return true;
    }

    sim::HoldEnv Server::Impl::HoldEnvFor() {
        holdFeet.clear();
        for (const auto& [id, p] : PlayersBySlot()) {
            if (Fighting() && fighters[p->slot].dead) continue;
            holdFeet.push_back(p->player->state.position);
        }
        return sim::HoldEnv{settings.player, playArea, holdFeet};
    }

    void Server::Impl::EmitHold(const sim::HoldChange& c) {
        const uint32_t netId = NetIdOf(c.entity);
        if (netId == 0 || failed || (!c.hold && c.reason == 0)) return;
        Emit(HoldEvent(c, netId));
        BodyTrack& t = tracks[netId];
        t.held = c.hold;
        t.resting = false;
        if (!c.hold) ++stats.releases[std::min<size_t>(c.reason, stats.releases.size() - 1)];
    }

    void Server::Impl::ReleaseHeld(Peer& p, sim::HoldEnd reason, uint16_t yaw, int16_t pitch) {
        Player& pl = *p.player;
        if (pl.held == scene::kNoEntity) return;
        const uint32_t now = Tick();
        const scene::Entity held = pl.held;
        pl.held = scene::kNoEntity;
        pl.holdChangeTick = now;
        pl.anyHoldChange = true;
        if (!simulation->Scene().IsValid(held) || !simulation->Scene().TryGet<scene::Held>(held)) return;
        const sim::HoldEnv env = HoldEnvFor();
        const sim::HeldPose pose = sim::HeldPoseAt(*simulation, held, pl.state.position, yaw, sim::HoldPitch(pitch), env, now);
        const glm::dvec3 anchor = sim::HoldAnchor(pl.state.position, settings.player);
        sim::HoldChange c;
        sim::EndHold(*simulation, held, reason, pose.pose, sim::ReleaseVelocity(reason, pl.state.velocity, yaw, pitch), &anchor, env, now,
                     true, c);
        EmitHold(c);
        stats.drops += reason == sim::HoldEnd::Drop ? 1u : 0u;
        stats.throws += reason == sim::HoldEnd::Throw ? 1u : 0u;
    }

    void Server::Impl::ReleaseGone(Peer& p) {
        Player& pl = *p.player;
        if (pl.held == scene::kNoEntity) return;
        const scene::Entity held = pl.held;
        pl.held = scene::kNoEntity;
        scene::Scene& scene = simulation->Scene();
        if (!scene.IsValid(held) || !scene.TryGet<scene::Held>(held)) return;
        sim::MechanismPose pose;
        glm::vec3 velocity(0.0f);
        if (const scene::Transform* t = scene.TryGet<scene::Transform>(held)) {
            pose.position = glm::dvec3(t->current.position);
            pose.rotation = t->current.rotation;
        }
        if (const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(held)) velocity = simulation->Physics().GetState(body->body).linearVelocity;
        const sim::HoldEnv env = HoldEnvFor();
        sim::HoldChange c;
        sim::EndHold(*simulation, held, sim::HoldEnd::HolderGone, pose, velocity, nullptr, env, Tick(), true, c);
        EmitHold(c);
    }

    void Server::Impl::AutoReleased(uint8_t slot) {
        for (auto& [id, p] : peers) {
            if (!p.joined || !p.player || p.slot != slot) continue;
            Player& pl = *p.player;
            pl.held = scene::kNoEntity;
            pl.holdChangeTick = Tick();
            pl.anyHoldChange = true;
            pl.autoReleaseTick = Tick();
            pl.autoReleaseEcho = LeadOf(id, pl);
        }
    }

    void Server::Impl::EmitHoldChanges(std::span<const sim::HoldChange> changes) {
        for (const sim::HoldChange& c : changes) {
            EmitHold(c);
            if (!c.hold) AutoReleased(c.holder);
        }
    }

    void Server::Impl::ClearLostHolds() {
        const scene::Scene& scene = simulation->Scene();
        for (auto& [id, p] : peers)
            if (p.player && p.player->held != scene::kNoEntity &&
                (!scene.IsValid(p.player->held) || !scene.TryGet<scene::Held>(p.player->held)))
                p.player->held = scene::kNoEntity;
    }

    void Server::Impl::StepHolds() {
        ClearLostHolds();
        const uint32_t now = Tick();
        for (auto& [id, p] : PlayersBySlot()) {
            Player& pl = *p->player;
            if (pl.held == scene::kNoEntity) continue;
            const sim::HoldEnv env = HoldEnvFor();
            if (!sim::StepHeld(*simulation, pl.held, pl.state, pl.last.yaw, sim::HoldPitch(pl.last.pitch), env, now, true)) continue;
            ReleaseHeld(*p, sim::HoldEnd::Pinned, pl.last.yaw, pl.last.pitch);
            pl.autoReleaseTick = now;
            pl.autoReleaseEcho = LeadOf(id, pl);
        }
    }

    uint64_t Server::Impl::CheckModelHash(scene::Entity entity, uint64_t volumeHash) const {
        const scene::Scene& scene = simulation->Scene();
        if (const scene::Mechanism* m = scene.TryGet<scene::Mechanism>(entity); m && m->track.count > 0)
            return volumeHash ^ MechanismTerm(m->track.Newest(), m->track.Newest().flags, m->modelOffset);
        if (const scene::Held* h = scene.TryGet<scene::Held>(entity)) return volumeHash ^ HeldTerm(h->holder, h->relYaw, h->sinceTick);
        return volumeHash;
    }
}
