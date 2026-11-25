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

// Server::Impl: join, refusals, inputs, player steps, snapshots, the join's baseline (docs/design/rules.md "File map"). [wp-6]
namespace vortex::net {
    using namespace detail;

    void Server::Impl::DropPlayer(Peer& p) {
        if (!p.player) return;
        ReleaseGone(p);   // [mech-1] what it held falls where it is (HolderGone)
        // Its crates go with it (review of stage 6: a reconnect loop left 8 bodies per connection).
        const std::deque<uint32_t> crates = p.player->crates;
        for (const uint32_t netId : crates)
            if (const scene::Entity e = EntityOf(netId); e != scene::kNoEntity) sim::Despawn(*simulation, e, scene::DespawnReason::Destroyed);
        simulation->Physics().DestroyCharacter(p.player->character);
        p.player.reset();
        if (p.joined) {
            // Review of [wp-7]: its queued casts still carve, as anyone's, but hurt nobody and credit nobody (the slot
            // goes to the next joiner, whose team would decide who they hurt).
            const uint8_t player = static_cast<uint8_t>(p.slot + 1);
            for (QueuedCast& q : queue)
                if (q.player == player) {
                    q.player = 0;
                    q.request.player = 0;
                }
            FightLeave(p.slot);   // [wp-7] its place on its team is free (never rebalanced mid-round)
        }
    }

    std::vector<std::pair<PeerId, Peer*>> Server::Impl::PlayersBySlot() {
        std::vector<std::pair<PeerId, Peer*>> out;
        for (auto& [id, p] : peers)
            if (p.joined && p.player) out.emplace_back(id, &p);
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second->slot < b.second->slot; });
        return out;
    }

    std::vector<glm::dvec3> Server::Impl::PlayerFocus() const {
        std::vector<glm::dvec3> focus;
        for (const auto& [id, p] : peers)
            if (p.player) focus.push_back(glm::dvec3(p.player->state.position));
        return focus;
    }

    Peer* Server::Impl::PeerOfSlot(uint8_t player) {
        if (player == 0) return nullptr;
        for (auto& [id, p] : peers)
            if (p.joined && p.player && p.slot + 1u == player) return &p;
        return nullptr;
    }

    void Server::Impl::OnInputs(Peer& p, std::span<const uint8_t> bytes) {
        InputBatch batch;
        if (!p.player || !Decode(bytes, batch)) {
            ++stats.malformedInputs;
            return;
        }
        ++stats.inputPackets;
        Player& pl = *p.player;
        const uint32_t now = Tick();   // the tick simulated next
        uint32_t newest = 0;
        bool any = false;
        for (const sim::PlayerInput& in : batch.inputs) {
            const uint32_t t = in.tick;
            if (pl.anyInput && t <= pl.lastRealTick) {
                ++stats.inputsDuplicate;
            } else if (t < pl.joinTick) {
                // No client makes an input for a tick before its player existed (it predicts from
                // its first snapshot on): a forged or broken one is ignored.
                ++stats.inputsBeforeJoin;
                continue;
            } else if (t < now) {
                // Too late for its tick: it only refreshes the last input. Its edges still count
                // (a cast in it is captured once), derived against the levels the repeats kept.
                ++stats.inputsLate;
                const bool dead = Fighting() && fighters[p.slot].dead;
                if (!dead && NotAliveAt(p.slot, t)) {
                    // [wp-7] An input of the dead time (or from before the round start's teleport) that arrives after
                    // the spawn (review of [wp-7]): the new life's levels stay the spawn's (decision 7: every button
                    // held until released) and a cast in it is refused Dead; nothing else of it acts.
                    if ((in.buttons & sim::PlayerButton_Cast) != 0) RefuseDeadCast(p, t);
                } else {
                    const uint8_t pressed = sim::PressButtons(pl.state, in.buttons);
                    // [mech-1] A throw (or an auto-drop's echo) replaces the late cast and its use.
                    const bool consumed = !dead && HoldEdges(p, in, pressed);
                    // [wp-7] A dead player's late edges do nothing but refuse its cast (fight.md "Rules").
                    if (!consumed && (pressed & sim::PlayerButton_Cast) != 0 && !RefuseDeadCast(p, in.tick)) LateCast(p, in, now);
                    // [mech-1] A late use is judged from the state of its own tick.
                    if (!consumed && (pressed & sim::PlayerButton_Use) != 0 && !dead) PlayerUse(p, in, pl.StateAt(in.tick), now);
                    if ((pressed & sim::PlayerButton_Crate) != 0 && !dead) PlayerCrate(p, in, pl.state);
                    // A jump edge is not spent here without a jump: the next step makes it.
                    if ((pressed & sim::PlayerButton_Jump) != 0 && !pl.lateJump && !dead) {
                        pl.lateJump = true;
                        ++stats.jumpsFromLateInputs;
                    }
                }
                pl.last = in;
                pl.lastRealTick = t;
                pl.anyInput = true;
                pl.queue.erase(pl.queue.begin(), pl.queue.upper_bound(t));
            } else if (t - now >= settings.inputQueueTicks) {
                ++stats.inputsTooEarly;
                continue;
            } else if (!pl.queue.emplace(t, in).second) {
                ++stats.inputsDuplicate;
            } else {
                ++stats.inputsQueued;
            }
            newest = any ? std::max(newest, t) : t;
            any = true;
        }
        if (batch.inputs.empty()) return;
        if (any) pl.ackTick = std::max(pl.ackTick, newest);
        // The margin of the newest input of the packet, dropped ones too (the client is too far
        // ahead: exactly what its clock must hear), unless a packet with newer inputs already
        // arrived (reordered or duplicated: it would pair an older, smaller margin with the tick).
        const uint32_t last = batch.inputs.back().tick;
        if (pl.margin != kNoInputMargin && last <= pl.marginTick) return;
        const int64_t margin = static_cast<int64_t>(last) - static_cast<int64_t>(now);
        // No client is that far off: a forged tick must not become the newest for good.
        if (margin > kMaxReportedMargin || margin < -kMaxReportedMargin) return;
        pl.margin = static_cast<int16_t>(std::clamp<int64_t>(margin, -32767, 32767));
        pl.marginTick = last;
    }

    void Server::Impl::StepPlayers() {
        const uint32_t now = Tick();
        const float dt = simulation->FixedTimeStep();
        ClearLostHolds();
        for (auto& [id, peer] : PlayersBySlot()) {
            Player& pl = *peer->player;
            sim::PlayerInput input;
            const auto it = pl.queue.find(now);
            if (it != pl.queue.end()) {
                input = it->second;
                pl.last = input;
                pl.lastRealTick = now;
                pl.anyInput = true;
            } else {
                // Missing: the last movement for repeatInputTicks, then none. The levels stay those of
                // the last input, so a repeat never makes an edge.
                input = pl.last;
                input.buttons = pl.state.lastButtons;
                ++stats.ticksRepeated;
                if (!pl.anyInput || now - pl.lastRealTick > settings.repeatInputTicks) {
                    input.moveX = input.moveZ = 0;
                    ++stats.ticksStood;
                }
            }
            pl.queue.erase(pl.queue.begin(), pl.queue.upper_bound(now));
            input.tick = now;
            if (Fighting() && fighters[peer->slot].dead) {
                // [wp-7] A dead player is not stepped (fight.md "Rules", decision 7: no change to StepPlayer): it
                // stays where it died; its input is consumed, a cast edge in it refused Dead.
                const uint8_t levels = static_cast<uint8_t>(input.buttons & sim::kPlayerButtonMask);
                if ((levels & sim::PlayerButton_Cast) != 0 && (pl.state.lastButtons & sim::PlayerButton_Cast) == 0) RefuseDeadCast(*peer, now);
                pl.state.lastButtons = levels;
                pl.lateJump = false;
                pl.Store(now);
                continue;
            }
            const sim::PlayerState before = pl.state;
            // The colliders this step can touch hold every edit made so far (never a worker's timing).
            tickFlushCooks += simulation->Terrain().FlushBox(sim::PlayerReach(pl.state.position, settings.player));
            // [mech-1] Mechanisms take their pose at this tick before the player meets them, as on its client.
            if (!mechanisms.empty()) {
                sim::PoseMechanisms(*simulation, now, sim::PlayerReach(pl.state.position, settings.player));
                sim::PushOutOfMechanisms(*simulation, now, pl.state, pl.character, settings.player);
            }
            const uint8_t levels = input.buttons;
            const bool lateJump = pl.lateJump;
            if (lateJump) {
                // The edge of a late input's jump is made now; the levels stay this input's.
                pl.state.lastButtons = static_cast<uint8_t>(pl.state.lastButtons & ~sim::PlayerButton_Jump);
                input.buttons = static_cast<uint8_t>(input.buttons | sim::PlayerButton_Jump);
                pl.lateJump = false;
            }
            const sim::PlayerStepResult r = sim::StepPlayer(simulation->Physics(), simulation->Terrain(), pl.character, pl.state, input,
                                                            settings.player, playArea, dt);
            if (lateJump) pl.state.lastButtons = static_cast<uint8_t>(levels & sim::kPlayerButtonMask);
            stats.heldTicks += r.held ? 1u : 0u;
            EnvironmentDamage(peer->slot, before, pl.state);   // [wp-7] falls, lava, the void (Live only)
            pl.Store(now);
            // [mech-1] The replicated body it stands on (a body stood on recently is not a pick-up).
            if (const physics::BodyId g = simulation->Physics().GetCharacterState(pl.character).groundBody; g.IsValid()) {
                const scene::Entity e = simulation->Scene().EntityFromUserData(simulation->Physics().GetUserData(g));
                if (NetIdOf(e) != 0) {
                    pl.groundEntity = e;
                    pl.groundTick = now;
                }
            }
            // [mech-1] The throw (and an auto-drop's echo) comes first and consumes the cast and the use.
            const bool consumed = HoldEdges(*peer, input, r.pressed);
            if (!consumed && r.cast) CaptureCast(*peer, input, pl.state, false);
            else if (!consumed && (r.pressed & sim::PlayerButton_Cast) != 0) RejectCast(*peer, now);
            if (!consumed && (r.pressed & sim::PlayerButton_Use) != 0) PlayerUse(*peer, input, &pl.state, now);
            if ((r.pressed & sim::PlayerButton_Crate) != 0) PlayerCrate(*peer, input, pl.state);
        }
    }

    void Server::Impl::SendSnapshots(uint32_t tick) {
        const std::vector<std::pair<PeerId, Peer*>> players = PlayersBySlot();
        const std::vector<BodySample> awake = AwakeSamples();
        for (const auto& [id, peer] : players) {
            Snapshot snap;
            snap.tick = tick;
            snap.lastOpSeq = LastOpSeq();
            snap.ackInputTick = peer->player->ackTick;
            snap.marginTick = peer->player->marginTick;
            snap.inputMargin = peer->player->margin;
            snap.hasOwner = true;
            snap.owner = peer->player->state;
            snap.lastCastTick = peer->player->lastCastTick;
            snap.castRejected = peer->player->castRejected;
            snap.lastUseTick = peer->player->lastUseTick;
            snap.useResult = peer->player->useResult;
            snap.fight = OwnerFightOf(peer->slot);
            for (const auto& [otherId, other] : players) {
                if (otherId == id) continue;
                snap.others.push_back({other->slot, other->player->state.position, other->player->last.yaw, other->player->state.flags,
                                       RemoteFightOf(other->slot), sim::HoldPitch(other->player->last.pitch)});
            }
            // Bodies by priority: what was left out longest, nearest first, as many as fit.
            const size_t fixed = kSnapshotHeaderBytes + kSnapshotOwnerBytes + snap.others.size() * kSnapshotPlayerBytes;
            const size_t room = settings.snapshotBytes > fixed ? std::min<size_t>((settings.snapshotBytes - fixed) / kSnapshotBodyBytes, 255) : 0;
            std::unordered_map<uint32_t, float> priority;
            std::vector<std::pair<float, size_t>> order;
            order.reserve(awake.size());
            const float scale = std::max(settings.bodyPriorityMetres, 0.001f);
            for (size_t i = 0; i < awake.size(); ++i) {
                const auto it = peer->priority.find(awake[i].netId);
                const float waited = (it != peer->priority.end() ? it->second : 0.0f) + 1.0f;
                priority[awake[i].netId] = waited;
                const float distance = glm::distance(awake[i].position, peer->player->state.position);
                order.emplace_back(waited / (1.0f + distance / scale), i);
            }
            std::sort(order.begin(), order.end(), [&awake](const auto& a, const auto& b) {
                return a.first != b.first ? a.first > b.first : awake[a.second].netId < awake[b.second].netId;
            });
            for (size_t k = 0; k < order.size() && k < room; ++k) {
                const BodySample& b = awake[order[k].second];
                snap.bodies.push_back(b);
                priority[b.netId] = 0.0f;
            }
            peer->priority = std::move(priority);
            stats.bodySamples += snap.bodies.size();
            stats.bodiesLeftOut += awake.size() - snap.bodies.size();
            stats.maxSnapshotBodies = std::max(stats.maxSnapshotBodies, static_cast<uint32_t>(snap.bodies.size()));
            const std::vector<uint8_t> bytes = Encode(snap);
            stats.maxSnapshotBytes = std::max(stats.maxSnapshotBytes, bytes.size());
            ++stats.snapshotHistogram[std::min<size_t>(bytes.size() / 64, stats.snapshotHistogram.size() - 1)];
            ++stats.snapshots;
            stats.snapshotBytes += bytes.size();
            Send(id, Channel::State, bytes);
        }
    }

    uint32_t Server::Impl::JoinedCount() const {
        uint32_t n = 0;
        for (const auto& [id, p] : peers) n += p.joined ? 1u : 0u;
        return n;
    }

    void Server::Impl::Refuse(PeerId peer, Peer& p, RefuseReason reason) {
        Send(peer, Channel::Events, Encode(net::Refuse{reason}));
        p.closing = true;
        p.closingTick = Tick();
        ++stats.clientsRefused;
        Log::Info("Server: refused a client: {}", ToString(reason));
    }

    void Server::Impl::OnHello(PeerId peer, Peer& p, std::span<const uint8_t> bytes) {
        Hello hello;
        if (!Decode(bytes, hello)) return Refuse(peer, p, RefuseReason::Malformed);
        if (hello.protocolVersion != kProtocolVersion) return Refuse(peer, p, RefuseReason::Protocol);
        if (hello.buildHash != buildHash) return Refuse(peer, p, RefuseReason::Build);
        if (hello.token != settings.session.token) return Refuse(peer, p, RefuseReason::Token);
        uint32_t joined = 0;
        std::vector<bool> used(256, false);
        for (const auto& [id, other] : peers)
            if (other.joined) {
                ++joined;
                used[other.slot] = true;
            }
        if (joined >= settings.session.maxClients) return Refuse(peer, p, RefuseReason::Full);
        uint8_t slot = 0;
        while (used[slot]) ++slot;
        Welcome welcome;
        welcome.slot = slot;
        welcome.tick = Tick();
        welcome.region = region;
        welcome.spawn = tmpl.spawn;
        // [wp-7] In a fight a joiner starts at its team's spawn (fight.md "Rules", decision 13).
        fighters[slot] = sim::FighterState{};
        if (Fighting()) {
            fighters[slot].team = JoinTeam(slot);
            welcome.spawn = sim::TeamSpawnFeet(*simulation, Rules(), region, fighters[slot].team, teamSpawnCount[fighters[slot].team - 1]++,
                                               glm::dvec3(tmpl.spawn));
            welcome.fight.mode = Rules().mode;
            welcome.fight.maxHealth = static_cast<uint8_t>(std::clamp<int>(Rules().maxHealth, 1, 255));
            welcome.fight.maxCharges = Rules().maxCharges;
            welcome.fight.chargeTicks = Rules().chargeTicks;
            welcome.fight.respawnTicks = Rules().respawnTicks;
        }
        welcome.player = settings.player;
        welcome.tickSeconds = simulation->FixedTimeStep();
        welcome.gravity = simulation->Config().physics.gravity;
        welcome.joinOpSeq = LastOpSeq();
        welcome.probes = probes;
        Send(peer, Channel::Events, Encode(welcome));
        // Join by chunk sync (stage 7a): the live stream from the join point on; what was made
        // before comes as ChunkData (its WindowUpdate says where) and the entity baseline.
        p.joined = true;
        p.slot = slot;
        p.sentUpTo = LastOpSeq();
        p.pacer = std::make_unique<BulkPacer>(settings.bulkPacer);
        p.planner = HashCheckPlanner(settings.hashCheck);
        p.join = JoinRecord{};
        p.join.joinTick = Tick();
        p.join.joinOpSeq = welcome.joinOpSeq;
        QueueBaseline(p, welcome.spawn);
        // Its player stands at the spawn point; the ground under every player gets its collider
        // before the next step (a gate that held the player here would hold it for another
        // number of ticks than its client's): the spawn is a permanent collider focus (built at
        // Start, kept current by the background cooks) and this tick's StepPlayers cooks whatever
        // of the player's reach is still stale (FlushBox). Stage 8: no global Terrain().Flush()
        // here any more - it cooked every pending collider of the window in the join tick (126 ms
        // in the owner's run, the first join after the spawn's colliders were dropped).
        p.player = std::make_unique<Player>();
        p.player->joinTick = Tick();          // Receive runs at the start of the tick that steps it first
        p.player->state.position = glm::vec3(welcome.spawn);
        p.player->character = simulation->Physics().CreateCharacter(sim::PlayerCharacterDesc(p.player->state.position, settings.player));
        tickEvents |= kTickHello;
        FightJoin(p);
        ++stats.clientsJoined;
        Log::Info("Server: client {} joined at tick {} (slot {}, join point opSeq {}, {} entities in its baseline, {} modified chunks)",
                  peer, Tick(), slot, LastOpSeq(), p.baseline.size(), modified.size());
    }

    void Server::Impl::QueueBaseline(Peer& p, const glm::dvec3& at) {
        // Nearest first by the distance to the nearest point of the model's bounding sphere (the
        // body's origin is a corner of its model): the near set is a prefix of the order.
        std::vector<std::pair<double, uint32_t>> ids;
        for (const auto& [netId, light] : lights) ids.emplace_back(glm::distance(glm::dvec3(light.position), at), netId);
        const physics::PhysicsWorld& physics = simulation->Physics();
        const double voxelSize = static_cast<double>(simulation->World().VoxelSize());
        const scene::Scene& scene = simulation->Scene();
        for (const auto [entity, id, body] : scene.Registry().view<const scene::NetId, const scene::RigidBody>().each()) {
            double extent = 0.0;
            if (const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity); voxels && voxels->model)
                extent = glm::length(glm::dvec3(voxels->model->Volume().Size())) * voxelSize;
            ids.emplace_back(std::max(0.0, glm::distance(glm::dvec3(physics.GetState(body.body).position), at) - extent), id.value);
        }
        std::sort(ids.begin(), ids.end());
        p.baseline.clear();
        p.nearLeft = 0;
        p.nearSent = false;
        for (const auto& [d, netId] : ids) {
            p.baseline.push_back(netId);
            p.nearLeft += d <= static_cast<double>(settings.baselineNearMetres) ? 1u : 0u;
        }
        p.baselineOpen = true;
        p.baselineSent = 0;
    }

    bool Server::Impl::MakeBaselineEntity(uint32_t netId, BaselineEntity& out) {
        out = BaselineEntity{};
        out.asOfOpSeq = LastOpSeq();
        // The tick whose physics made the state (Flush runs after StepPhysics, which already
        // counted the next tick): snapshots, EntityRest and the rest tick label it the same
        // (review of stage 7a).
        out.tick = steppedTick;
        out.spawn.netId = netId;
        if (const auto light = lights.find(netId); light != lights.end()) {
            out.spawn.kind = EntityKind::Light;
            out.spawn.light = light->second;
            return true;
        }
        const scene::Entity entity = EntityOf(netId);
        if (entity == scene::kNoEntity) return false;
        out.spawn.kind = EntityKind::Object;
        if (!DescribeObject(entity, out.spawn.object)) return false;
        if (MakeMechanismState(entity, steppedTick, out.spawn.mechanism)) out.spawn.kind = EntityKind::Mechanism;
        if (out.spawn.object.flags & kObjectHeld) return true;   // [mech-1] its pose now, never resting
        const auto t = tracks.find(netId);
        const bool resting = t != tracks.end() && t->second.resting;
        if (resting) {
            out.restTick = t->second.restTick;
            out.spawn.object.position = t->second.restPosition;
            out.spawn.object.rotation = t->second.restRotation;
            out.spawn.object.linearVelocity = glm::vec3(0.0f);
            out.spawn.object.angularVelocity = glm::vec3(0.0f);
            out.spawn.object.flags = kObjectResting;
        } else {
            out.spawn.object.flags = 0;
        }
        return true;
    }
}
