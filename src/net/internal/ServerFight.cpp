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
#include <format>
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

// Server::Impl: the fight - teams, damage, death, respawn, charges and the round (docs/design/fight.md). Every
// damage goes through DealDamage (fight.md "Damage", decision 16). A sandbox session runs none of it. [wp-7]
namespace vortex::net {
    using namespace detail;

    sim::TeamId Server::Impl::JoinTeam(uint8_t slot) {
        if (!Fighting()) return sim::kNoTeam;
        std::array<bool, 256> occupied{};
        for (const auto& [id, p] : peers)
            if (p.joined && p.player) occupied[p.slot] = true;
        occupied[slot] = false;
        return sim::PickTeam(fighters, occupied);
    }

    void Server::Impl::FightJoin(Peer& p) {
        sim::FighterState& f = fighters[p.slot];
        if (!Fighting()) {
            f = sim::FighterState{};
            f.spawned = true;
            f.charges = 0;
            return;
        }
        // The team was picked in OnHello (its spawn is the Welcome's); a spectator until its first spawn.
        f.spawned = false;
        f.dead = false;
        f.kills = f.deaths = 0;
        Log::Info("Fight: slot {} joins team {}", p.slot, f.team);
        const PeerId self = [&] {
            for (const auto& [id, other] : peers)
                if (&other == &p) return id;
            return PeerId{0};
        }();
        Send(self, Channel::Events, Encode(MakeRoundUpdate()));
        SendRoster();
        if (round.phase == sim::RoundPhase::Live || round.phase == sim::RoundPhase::Countdown) Respawn(p.slot);
    }

    void Server::Impl::FightLeave(uint8_t slot) {
        fighters[slot] = sim::FighterState{};
        // Review of [wp-7]: the slot goes to the next joiner, so nobody keeps it as its last attacker (a later fall
        // or lava death would credit the joiner, maybe of the victim's own team).
        const uint8_t player = static_cast<uint8_t>(slot + 1);
        for (sim::FighterState& f : fighters)
            if (f.lastAttacker == player) {
                f.lastAttacker = 0;
                f.lastAttackTick = 0;
            }
        if (Fighting() && !failed) SendRoster();
    }

    std::vector<sim::PlayerView> Server::Impl::PlayerViews() {
        std::vector<sim::PlayerView> views;
        for (const auto& [id, p] : PlayersBySlot()) {
            const sim::FighterState& f = fighters[p->slot];
            views.push_back({static_cast<uint8_t>(p->slot + 1), f.team, p->player->state.position, f.spawned && !f.dead});
        }
        return views;
    }

    std::array<uint32_t, 2> Server::Impl::TeamPlayers() {
        std::array<uint32_t, 2> n{};
        for (const auto& [id, p] : PlayersBySlot()) {
            const sim::TeamId t = fighters[p->slot].team;
            if (t >= 1 && t <= sim::kTeamCount) ++n[t - 1];
        }
        return n;
    }

    std::array<uint32_t, 2> Server::Impl::TeamsInSanctum() {
        std::array<uint32_t, 2> n{};
        const voxel::World& w = simulation->World();
        for (const auto& [id, p] : PlayersBySlot()) {
            const sim::FighterState& f = fighters[p->slot];
            if (!f.spawned || f.dead || f.team < 1 || f.team > sim::kTeamCount) continue;
            if (sim::InSanctum(tmpl, w.WorldToVoxel(glm::dvec3(p->player->state.position)))) ++n[f.team - 1];
        }
        return n;
    }

    void Server::Impl::LogHold(uint32_t holdBefore) {
        const uint32_t tick = Tick();
        if (round.holdSince != 0)
            Log::Info("Siege: the attackers hold the sanctum from tick {}", round.holdSince);
        else
            Log::Info("Siege: the hold broke at tick {} after {:.1f} s", tick,
                      static_cast<double>(tick - holdBefore) * static_cast<double>(simulation->FixedTimeStep()));
    }

    void Server::Impl::SendToJoined(const std::vector<uint8_t>& bytes) {
        for (const auto& [id, p] : peers)
            if (p.joined && p.player && !p.closing) Send(id, Channel::Events, bytes);
    }

    Roster Server::Impl::MakeRoster() {
        Roster r;
        for (const auto& [id, p] : PlayersBySlot()) {
            const sim::FighterState& f = fighters[p->slot];
            if (r.players.size() < kMaxRosterPlayers) r.players.push_back({p->slot, f.team, f.kills, f.deaths, RosterAttuneOf(p->slot)});
        }
        return r;
    }

    RoundUpdate Server::Impl::MakeRoundUpdate() const {
        RoundUpdate u;
        u.mode = Rules().mode;
        u.phase = round.phase;
        u.phaseEndTick = round.phaseEndTick;
        u.score = round.score;
        u.scoreLimit = Rules().scoreLimit;
        u.winner = round.winner;
        u.end = round.end;
        u.overtime = round.overtime;
        u.holdSince = round.holdSince;
        u.holdTicks = static_cast<uint16_t>(std::min<uint32_t>(Rules().holdTicks, 0xFFFF));
        return u;
    }

    void Server::Impl::SendRoster() { rosterChanged = true; }

    void Server::Impl::FlushRoster() {
        // Review of [wp-7]: one Roster per tick however many kills, joins and leaves changed it (a blast with k kills
        // sent k whole rosters to every peer, outside the event budgets).
        if (!rosterChanged) return;
        rosterChanged = false;
        if (failed) return;
        ++stats.rostersSent;
        SendToJoined(Encode(MakeRoster()));
    }

    void Server::Impl::SendRound() {
        LogRound();
        SendToJoined(Encode(MakeRoundUpdate()));
    }

    void Server::Impl::SendKill(const KillNotice& kill) { SendToJoined(Encode(kill)); }

    void Server::Impl::LogRound() {
        if (round.phase == loggedPhase) return;
        loggedPhase = round.phase;
        const uint32_t tick = Tick();
        const std::array<uint32_t, 2> n = TeamPlayers();
        Log::Info("Round: phase {} at tick {} (players {} + {})", sim::ToString(round.phase), tick, n[0], n[1]);
        if (round.phase != sim::RoundPhase::Ended) return;
        if (round.winner == sim::kDraw)
            Log::Info("Round: draw {}-{} at tick {} ({})", round.score[0], round.score[1], tick, sim::ToString(Rules().mode));
        else
            Log::Info("Round: team {} wins {}-{} by {} at tick {} ({})", round.winner, round.score[round.winner == 2 ? 1 : 0],
                      round.score[round.winner == 2 ? 0 : 1], sim::ToString(round.end), tick, sim::ToString(Rules().mode));
    }

    bool Server::Impl::NotAliveAt(uint8_t slot, uint32_t inputTick) const {
        if (!Fighting()) return false;
        const sim::FighterState& f = fighters[slot];
        // Dead now, or an input from before the current life (the dead time, or before a round start's teleport) that
        // arrives late (review of [wp-7]: it cast from the corpse with a charge of the new life).
        return f.dead || (f.spawned && inputTick <= f.lifeStartTick);
    }

    bool Server::Impl::RefuseDeadCast(Peer& p, uint32_t inputTick) {
        if (!NotAliveAt(p.slot, inputTick)) return false;
        ++stats.castsRefusedDead;
        p.player->lastCastTick = inputTick;
        p.player->castRejected = CastRejection::Dead;
        return true;
    }

    void Server::Impl::DealDamage(const sim::DamageEvent& event) {
        if (!Fighting() || round.phase != sim::RoundPhase::Live || event.target == 0) return;
        const uint32_t tick = Tick();
        const uint8_t slot = static_cast<uint8_t>(event.target - 1);
        Peer* victim = PeerOfSlot(event.target);
        if (victim == nullptr) return;
        sim::FighterState& target = fighters[slot];
        sim::FighterState* source = event.source != 0 && PeerOfSlot(event.source) != nullptr ? &fighters[event.source - 1] : nullptr;
        const sim::DamageResult r = sim::ApplyDamage(target, event.target, source, event, Rules(), tick);
        if (r.dealt <= 0) return;
        ++stats.damageEvents;
        stats.damageTotal += static_cast<uint32_t>(r.dealt);
        if (!r.killed) return;
        ++stats.deaths;
        // [mech-1] What it held falls before the death's effects (HolderDead).
        if (victim->player->held != scene::kNoEntity) ReleaseHeld(*victim, sim::HoldEnd::HolderDead, victim->player->last.yaw, victim->player->last.pitch);
        // The dead stand still where they died (not stepped until the respawn: fight.md "Rules").
        Player& pl = *victim->player;
        pl.state.velocity = glm::vec3(0.0f);
        sim::RestorePlayer(simulation->Physics(), pl.character, pl.state);
        pl.Store(tick);
        KillNotice kill;
        kill.tick = tick;
        kill.victim = slot;
        kill.cause = static_cast<sim::DamageCause>(std::clamp<uint8_t>(event.cause, 1, sim::kMaxDamageCause));
        const auto score = round.score;
        // A credit goes only to a living slot of the other team (review of [wp-7]: a backstop to FightLeave's reset).
        if (r.credit != 0 && PeerOfSlot(r.credit) != nullptr && (fighters[r.credit - 1].team != target.team || target.team == sim::kNoTeam)) {
            sim::FighterState& killer = fighters[r.credit - 1];
            killer.kills = static_cast<uint16_t>(killer.kills + 1);
            ++stats.kills;
            kill.killer = r.credit;
            kill.flags = KillNotice::kCredited;
            sim::ScoreKill(round, Rules(), killer.team, tick);
        }
        Log::Info("Kill: tick {} slot {} (team {}) by {}, credit {}", tick, slot, target.team, sim::ToString(kill.cause),
                  kill.killer != 0 ? std::format("slot {}", kill.killer - 1) : std::string("none"));
        SendKill(kill);
        SendRoster();
        if (round.score != score || round.phase != loggedPhase) SendRound();
    }

    void Server::Impl::EnvironmentDamage(uint8_t slot, const sim::PlayerState& before, const sim::PlayerState& after) {
        if (!Fighting() || round.phase != sim::RoundPhase::Live) return;
        sim::FighterState& f = fighters[slot];
        if (!f.spawned || f.dead) return;
        const uint8_t target = static_cast<uint8_t>(slot + 1);
        const uint32_t tick = Tick();
        // Void: below the region's floor.
        if (static_cast<double>(after.position.y) < static_cast<double>(region.heightRange.x) + 2.0) {
            DealDamage({target, 0, 999, static_cast<uint8_t>(sim::DamageCause::Void)});
            return;
        }
        // A landing after a step that began in the air; a lifted or leaping player takes none (magic.md "Damage and hazards").
        if (!before.OnGround() && after.OnGround() && (before.status & sim::PlayerStatus_Lifted) == 0) {
            const int16_t fall = sim::FallDamage(Rules(), before.velocity.y);
            if (fall > 0) DealDamage({target, 0, fall, static_cast<uint8_t>(sim::DamageCause::Fall)});
            if (f.dead) return;
        }
        // Lava under the feet or at the shins.
        if (tick >= f.nextLavaTick) {
            const voxel::World& w = simulation->World();
            const uint8_t lava = simulation->WorldMaterialIds().lava;
            const glm::dvec3 feet(after.position);
            const bool inLava = w.GetVoxel(w.WorldToVoxel(feet - glm::dvec3(0.0, 0.05, 0.0))) == lava ||
                                w.GetVoxel(w.WorldToVoxel(feet + glm::dvec3(0.0, 0.2, 0.0))) == lava;
            if (inLava) {
                f.nextLavaTick = tick + Rules().lavaEveryTicks;
                DealDamage({target, 0, Rules().lavaDamage, static_cast<uint8_t>(sim::DamageCause::Lava)});
            }
        }
    }

    void Server::Impl::Respawn(uint8_t slot) {
        Peer* p = PeerOfSlot(static_cast<uint8_t>(slot + 1));
        if (p == nullptr || !Fighting()) return;
        sim::FighterState& f = fighters[slot];
        if (f.team < 1 || f.team > sim::kTeamCount) return;
        const uint32_t tick = Tick();
        const uint32_t k = teamSpawnCount[f.team - 1]++;
        const glm::dvec3 feet = sim::TeamSpawnFeet(*simulation, Rules(), region, f.team, k, glm::dvec3(tmpl.spawn));
        sim::StartLife(f, Rules(), tick);
        ApplyAttune(slot);   // [mag-2] an Attune taken while dead or before this spawn
        sim::RefillMana(spellTable, castContext.casters[static_cast<uint8_t>(slot + 1)], tick);   // [mag-8] a full pool each life
        Player& pl = *p->player;
        // Decision 7: a teleport by the server, standing still, every button held (no edge until released).
        pl.state = sim::PlayerState{};
        pl.state.position = glm::vec3(feet);
        pl.state.lastButtons = sim::kPlayerButtonMask;
        sim::StartFreshLife(pl.state);   // magic.md: no root, pin or push for the guard's length
        pl.lateJump = false;
        sim::RestorePlayer(simulation->Physics(), pl.character, pl.state);
        tickFlushCooks += simulation->Terrain().FlushBox(sim::PlayerReach(pl.state.position, settings.player));
        pl.Store(tick);
        ++stats.respawns;
        Log::Info("Fight: slot {} spawns at ({:.1f}, {:.1f}, {:.1f}) life {}", slot, feet.x, feet.y, feet.z, f.lifeSeq);
    }

    void Server::Impl::StepFight() {
        if (!Fighting()) return;
        if (!sessionOver) StepRoundAndLives();
        FlushRoster();
    }

    void Server::Impl::StepRoundAndLives() {
        const uint32_t tick = Tick();
        for (const auto& [id, p] : PlayersBySlot()) {
            sim::FighterState& f = fighters[p->slot];
            if (!f.spawned) continue;
            if (!f.dead) sim::TickCharges(f, Rules(), tick);
            else if (tick >= f.respawnTick) Respawn(p->slot);
        }
        if (Rules().mode == sim::GameMode::Felling && round.phase == sim::RoundPhase::Live && tick % 30 == 0) CheckCrowns();
        const sim::RoundPhase phaseBefore = round.phase;
        const uint32_t holdBefore = round.holdSince;
        const std::array<uint32_t, 2> inSanctum = Rules().mode == sim::GameMode::Siege ? TeamsInSanctum() : std::array<uint32_t, 2>{};
        const sim::RoundStep step = sim::StepRound(round, Rules(), tick, TeamPlayers(), felled, inSanctum);
        if (round.holdSince != holdBefore) LogHold(holdBefore);
        if (step.startLives) {
            // The round begins: every player at its team's spawn, health and charges full, scores 0.
            for (const auto& [id, p] : PlayersBySlot()) {
                sim::FighterState& f = fighters[p->slot];
                f.kills = f.deaths = 0;
                Respawn(p->slot);
            }
            felled = {false, false};
            SendRoster();
        }
        // Review of [wp-7]: the towers stand whole when the round goes Live (the warmup's free casts and the
        // countdown's could cut a crown before it, and the first crown check ended the round).
        if (phaseBefore != sim::RoundPhase::Live && round.phase == sim::RoundPhase::Live) {
            BuildFellingTowers();
            felled = {false, false};
        }
        if (step.changed) SendRound();
        if (step.sessionOver) {
            sessionOver = true;
            Log::Info("Round: session over at tick {}", tick);
            Log::Info("{}", FightLineOf(stats));
            if (Rules().endSession) endNow = true;
        }
    }

    Snapshot::Fight Server::Impl::OwnerFightOf(uint8_t slot) const {
        Snapshot::Fight o;
        if (!Fighting()) return o;                           // health 100, charges 0, flags 0
        const sim::FighterState& f = fighters[slot];
        const uint32_t tick = Tick();
        o.health = static_cast<uint8_t>(std::clamp<int>(f.health, 0, 255));
        o.charges = f.charges;
        o.chargeTicks = f.nextChargeTick > tick ? static_cast<uint8_t>(std::min<uint32_t>(f.nextChargeTick - tick, 255)) : 0;
        o.flags = static_cast<uint8_t>((f.dead ? Snapshot::Fight::kDead : 0) | (f.spawned && tick < f.protectedUntil ? Snapshot::Fight::kProtected : 0) |
                                       (!f.spawned ? Snapshot::Fight::kSpectator : 0));
        o.respawnTicks = f.dead && f.respawnTick > tick ? static_cast<uint8_t>(std::min<uint32_t>(f.respawnTick - tick, 255)) : 0;
        o.lifeSeq = f.lifeSeq;
        o.hits = f.hits;
        return o;
    }

    uint8_t Server::Impl::RemoteFightOf(uint8_t slot) const {
        if (!Fighting()) return 100;
        const sim::FighterState& f = fighters[slot];
        return static_cast<uint8_t>(std::clamp<int>(f.health, 0, 127) | (f.dead ? RemotePlayer::kDeadBit : 0));
    }

    void Server::Impl::BuildFellingTowers() {
        if (Rules().mode != sim::GameMode::Felling) return;
        const voxel::World& w = simulation->World();
        const float vs = w.VoxelSize();
        const bool first = towerOps.empty();
        if (first) {
            // The towers' boxes are found once, on the generated ground (a later raycast would meet the tower).
            for (int t = 0; t < 2; ++t) {
                const glm::vec2 at = Rules().towers[t];
                const voxel::WorldRayHit hit = w.Raycast(glm::dvec3(at.x, static_cast<double>(region.heightRange.y) - 0.5, at.y),
                                                         glm::vec3(0.0f, -1.0f, 0.0f), 256.0f);
                const int32_t ground = hit.hit ? hit.voxel.y + 1 : w.WorldToVoxel(glm::dvec3(at.x, 0.0, at.y)).y;
                const sim::FellingTower tower = sim::BuildFellingTower(w, simulation->WorldMaterialIds(), at, vs, ground);
                towerOps.insert(towerOps.end(), tower.ops.begin(), tower.ops.end());
                crowns[t] = {tower.crownMin, tower.crownMax, 0};
            }
        }
        // Built again when the round goes Live: the same boxes, applied and sent as any stream operation.
        for (const sim::WorldOp& op : towerOps) {
            const sim::OpResult r = sim::ApplyOp(simulation->World(), op);
            if (!CheckOp(r)) return;
            EmitOp(op, r);
        }
        for (int t = 0; t < 2; ++t) {
            crowns[t].voxels = sim::CountSolid(w, crowns[t].min, crowns[t].max);
            if (first)
                Log::Info("Felling: tower {} at ({:.1f}, {:.1f}), crown {} voxels", t + 1, Rules().towers[t].x, Rules().towers[t].y, crowns[t].voxels);
        }
        if (!first) Log::Info("Felling: both towers built again at tick {}", Tick());
    }

    void Server::Impl::CheckCrowns() {
        const voxel::World& w = simulation->World();
        for (int t = 0; t < 2; ++t) {
            if (felled[t] || crowns[t].voxels == 0) continue;
            const uint32_t count = sim::CountSolid(w, crowns[t].min, crowns[t].max);
            if (static_cast<uint64_t>(count) * 5 < crowns[t].voxels) {
                felled[t] = true;
                Log::Info("Felling: team {}'s crown fell at tick {} ({} of {} voxels left)", t + 1, Tick(), count, crowns[t].voxels);
            }
        }
    }

    std::string FightLineOf(const ServerStats& st) {
        return std::format("Fight: kills {}, deaths {}, damage events {}, damage {} hp, refused dead {}, no charge {}, respawns {}", st.kills,
                           st.deaths, st.damageEvents, st.damageTotal, st.castsRefusedDead, st.castsRefusedNoCharge, st.respawns);
    }

    const sim::Round& Server::RoundState() const { return m_Impl->round; }
    const sim::FighterState& Server::Fighter(uint8_t slot) const { return m_Impl->fighters[slot]; }
    bool Server::SessionOver() const { return m_Impl->sessionOver; }
    std::string Server::FightLine() const { return FightLineOf(m_Impl->stats); }
}
