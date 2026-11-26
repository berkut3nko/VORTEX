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

// Server::Impl: spells - capture, the run in the cast queue, the follow-ups, zones with the lead, Attune and the owner
// block's magic state (docs/design/magic.md "The cast pipeline", "Where every effect lives"). Every damage goes through
// DealDamage; every terrain operation through EmitOp.
namespace vortex::net {
    using namespace detail;

    namespace {
        glm::ivec3 ToQ(const glm::dvec3& metres, double voxelSize) {
            const glm::dvec3 q = metres * (16.0 / voxelSize);
            return {static_cast<int32_t>(std::llround(q.x)), static_cast<int32_t>(std::llround(q.y)), static_cast<int32_t>(std::llround(q.z))};
        }

        bool ElementBuilt(const sim::SpellTable& table, uint8_t element) {
            for (sim::SpellId s = 0; s < 16; ++s)
                if (table.spells[s].built && static_cast<uint8_t>(sim::ElementOf(s)) == element) return true;
            return false;
        }
    }

    void Server::Impl::StartMagic() {
        const voxel::World& w = simulation->World();
        spellTable = sim::BuildSpellTable(w.VoxelSize());
        castContext = sim::CastContext{};
        castContext.tuning = settings.player;
        castContext.maxZones = std::min<uint32_t>(settings.maxZones, kMaxBaselineZones);
        castContext.maxZonesPerPlayer = settings.maxZonesPerPlayer;
        // The operators stop at the play bounds (whole chunks) over the window's rows: every flood box stays inside the
        // server's window (play bounds + kServerWindowMarginChunks).
        const int32_t rowMin = w.WindowMinChunk().y * voxel::kChunkSize;
        const int32_t rowMax = (w.WindowMinChunk().y + w.WindowSize().y) * voxel::kChunkSize;
        castContext.walls.min = {region.playMinChunk.x * voxel::kChunkSize, rowMin, region.playMinChunk.y * voxel::kChunkSize};
        castContext.walls.max = {region.playMaxChunk.x * voxel::kChunkSize, rowMax, region.playMaxChunk.y * voxel::kChunkSize};
        if (Fighting()) {
            std::vector<std::pair<sim::TeamId, glm::dvec3>> spawns;
            for (sim::TeamId t = 1; t <= sim::kTeamCount; ++t) {
                const glm::vec2 at = Rules().teamSpawns[t - 1];
                spawns.emplace_back(t, glm::dvec3(at.x, 0.0, at.y));
            }
            castContext.spawns = sim::SpawnAreasOf(spawns, w);
        }
    }

    bool Server::Impl::SpellAllowed(const Peer& p, const sim::PlayerInput& input, uint32_t tick) const {
        if (NotAliveAt(p.slot, tick)) return false;
        sim::SpellCast cast;
        cast.player = static_cast<uint8_t>(p.slot + 1);
        cast.spell = input.spell;
        if (sim::CheckCapture(castContext, spellTable, cast, tick) != sim::SpellRefusal::None) return false;
        if (Fighting() && round.phase != sim::RoundPhase::Warmup && Rules().maxCharges != 0 && fighters[p.slot].charges == 0) return false;
        return true;
    }

    void Server::Impl::CaptureSpell(Peer& p, const sim::PlayerInput& input, const sim::PlayerState& at) {
        Player& pl = *p.player;
        if (input.spell < 16) pl.spellElement = static_cast<uint8_t>(sim::ElementOf(input.spell));
        const auto reject = [&](CastRejection why) {
            ++stats.spellsRefused;
            pl.lastCastTick = input.tick;
            pl.castRejected = why;
        };
        // Decision 5: dead, then the capture checks, then a charge (Countdown, Live and Ended; the warmup's are free).
        if (RefuseDeadCast(p, input.tick)) {
            ++stats.spellsRefused;
            return;
        }
        sim::SpellCast cast;
        cast.player = static_cast<uint8_t>(p.slot + 1);
        cast.team = fighters[p.slot].team;
        cast.spell = input.spell;
        cast.inputTick = input.tick;
        if (const sim::SpellRefusal why = sim::CheckCapture(castContext, spellTable, cast, input.tick); why != sim::SpellRefusal::None)
            return reject(RejectionOf(why));
        if (Fighting() && round.phase != sim::RoundPhase::Warmup && !sim::SpendCharge(fighters[p.slot], Rules(), Tick())) {
            ++stats.castsRefusedNoCharge;
            return reject(CastRejection::NoCharge);
        }
        const size_t form = static_cast<size_t>(sim::FormOf(input.spell));
        castContext.casters[cast.player].formReadyTick[form] = input.tick + spellTable.forms[form].cooldownTicks;
        QueuedCast q;
        q.spell = true;
        q.team = cast.team;
        q.request.player = cast.player;
        q.request.inputTick = input.tick;
        q.request.eye = glm::dvec3(at.position) + glm::dvec3(0.0, static_cast<double>(settings.player.eyeHeight), 0.0);
        q.request.dir = sim::LookDirection(input.yaw, input.pitch);
        q.request.spell = input.spell;
        q.queuedTick = Tick();
        q.player = cast.player;
        pl.lastCastTick = input.tick;
        pl.castRejected = CastRejection::None;
        queue.push_back(q);
    }

    void Server::Impl::LateSpell(Peer& p, const sim::PlayerInput& in, uint32_t now) {
        Player& pl = *p.player;
        const bool allowed = SpellAllowed(p, in, in.tick);
        if (!LateTake(p, in, now)) return;
        const sim::PlayerState* at = pl.StateAt(in.tick);
        CaptureSpell(p, in, at != nullptr ? *at : pl.state);
        // Its client moved from the input's tick on: the move starts now, less the ticks it has run there.
        if (allowed && sim::StartSpellMove(pl.state, in.spell, sim::LookDirection(in.yaw, in.pitch))) {
            const uint32_t ran = now - in.tick;
            pl.state.dashLeft = static_cast<uint8_t>(pl.state.dashLeft > ran ? pl.state.dashLeft - ran : 0u);
        }
    }

    std::vector<sim::SpellTarget> Server::Impl::SpellTargets() {
        std::vector<sim::SpellTarget> targets;
        for (const auto& [id, p] : PlayersBySlot()) {
            const sim::FighterState& f = fighters[p->slot];
            if (!f.spawned || f.dead) continue;
            targets.push_back({static_cast<uint8_t>(p->slot + 1), f.team, p->player->state.position});
        }
        return targets;
    }

    uint32_t Server::Impl::EmitGroup(const sim::BlastResult& r) {
        if (r.search.islandBoxUnloaded + r.search.attachedByUnloaded + r.search.neckUnloaded > 0)
            Fail("an island search met a chunk that is not loaded (the window must hold every flood box)");
        uint32_t emitted = 0;
        for (const sim::AppliedOp& applied : r.ops) {
            if (failed || !CheckOp(applied.result)) break;
            EmitOp(applied.op, applied.result);
            ++emitted;
        }
        if (!failed) EmitHoldChanges(r.holdChanges);
        EmitObjectEvents(r);
        if (!failed) EmitMechanismChanges(r.mechanismChanges);
        return emitted;
    }

    void Server::Impl::RunSpell(const QueuedCast& q, bool overdue) {
        sim::SpellCast cast;
        cast.player = q.request.player;
        cast.team = q.team;
        cast.spell = q.request.spell;
        cast.inputTick = q.request.inputTick;
        cast.eye = q.request.eye;
        cast.dir = q.request.dir;
        // The caster's own move (a Dash) started at its Spell edge inside StepPlayer: never a second time here.
        cast.casterState = nullptr;
        const std::vector<sim::SpellTarget> targets = SpellTargets();
        const sim::CastSpellResult r = sim::CastSpell(*simulation, spellTable, cast, castContext, targets);
        const uint64_t work = sim::CastWorkOf(r).Total();
        castCredit -= static_cast<int64_t>(work);
        stats.castWork += work;
        stats.maxCastWork = std::max(stats.maxCastWork, work);
        stats.castsOverdue += overdue ? 1u : 0u;
        if (r.refusal != sim::SpellRefusal::None) {
            ++stats.spellsRefused;
            if (Peer* p = PeerOfSlot(q.player); p != nullptr && p->player->lastCastTick == q.request.inputTick)
                p->player->castRejected = RejectionOf(r.refusal);
            return;
        }
        ++stats.spellCasts;
        const uint64_t bytesBefore = stats.eventBytes;
        const double vs = static_cast<double>(simulation->World().VoxelSize());
        // Decision 7: CastFx, the operations applied now, the zone starts, the cancels, then the damage.
        Emit(CastFx{q.player, cast.spell, ToQ(cast.eye, vs), r.hitQ, r.face, r.runTick});
        for (const sim::BlastResult& group : r.groups) stats.spellOps += EmitGroup(group);
        for (const sim::Zone& zone : r.zonesStarted) {
            if (failed) break;
            Emit(ZoneStartEvent{zone});
            ++stats.zonesEmitted;
        }
        for (const auto& [zoneId, endTick] : r.zonesCancelled) {
            if (failed) break;
            Emit(ZoneCancelEvent{zoneId, endTick});
            ++stats.zonesCancelled;
        }
        if (!failed)
            for (const sim::SpellDamage& d : r.damage)
                DealDamage({d.player, d.source, static_cast<int16_t>(d.amount), static_cast<uint8_t>(sim::DamageCause::Blast)});
        stats.spellBytes += stats.eventBytes - bytesBefore;
        const uint32_t delay = Tick() - q.queuedTick;
        ++stats.castDelay[std::min<uint32_t>(delay, 7)];
        stats.maxCastDelay = std::max(stats.maxCastDelay, delay);
    }

    void Server::Impl::RunScheduledSpells() {
        if (failed) return;
        const uint64_t bytesBefore = stats.eventBytes;
        simulation->Replication().refuseNewPieces = entityCredit <= 0;
        const sim::ScheduledResult r = sim::RunScheduled(*simulation, spellTable, castContext, Tick(), SpellTargets());
        simulation->Replication().refuseNewPieces = false;
        for (const sim::BlastResult& group : r.groups) {
            stats.spellOps += EmitGroup(group);
            ++scheduledGroups;
        }
        sitesRun += r.sitesRun;
        if (!failed)
            for (const sim::SpellDamage& d : r.damage)
                DealDamage({d.player, d.source, static_cast<int16_t>(d.amount), static_cast<uint8_t>(sim::DamageCause::Blast)});
        stats.spellBytes += stats.eventBytes - bytesBefore;
    }

    bool Server::Impl::OnAttune(Peer& p, std::span<const uint8_t> bytes) {
        Attune a;
        if (!Decode(bytes, a)) return false;
        const sim::FighterState& f = fighters[p.slot];
        const bool open = Fighting() && (!f.spawned || f.dead);
        const bool valid = a.craftA != a.craftB && ElementBuilt(spellTable, a.craftA) && ElementBuilt(spellTable, a.craftB);
        if (!open || !valid || !p.player) {
            ++stats.attunesRefused;
            Log::Info("Server: attune refused from slot {}", p.slot);
            return true;
        }
        p.player->attunePending = true;
        p.player->attuneA = a.craftA;
        p.player->attuneB = a.craftB;
        ++stats.attunes;
        return true;
    }

    void Server::Impl::ApplyAttune(uint8_t slot) {
        Peer* p = PeerOfSlot(static_cast<uint8_t>(slot + 1));
        if (p == nullptr || !p->player->attunePending) return;
        Player& pl = *p->player;
        sim::CasterState& c = castContext.casters[static_cast<uint8_t>(slot + 1)];
        c.elementA = pl.attuneA;
        c.elementB = pl.attuneB;
        pl.attunePending = false;
        pl.spellElement = pl.attuneA;
        SendRoster();
    }

    void Server::Impl::MagicJoin(uint8_t slot) { castContext.casters[static_cast<uint8_t>(slot + 1)] = sim::CasterState{}; }

    void Server::Impl::MagicLeave(uint8_t slot) { castContext.casters.erase(static_cast<uint8_t>(slot + 1)); }

    void Server::Impl::FillMagicOwner(Snapshot& snap, uint8_t slot, const Player& pl) const {
        sim::CasterState c;
        if (const auto it = castContext.casters.find(static_cast<uint8_t>(slot + 1)); it != castContext.casters.end()) c = it->second;
        for (size_t f = 0; f < 4; ++f) {
            const uint32_t left = c.formReadyTick[f] > snap.tick ? c.formReadyTick[f] - snap.tick : 0u;
            snap.formCooldown4[f] = static_cast<uint8_t>(std::min<uint32_t>((left + 3) / 4, 255));
        }
        uint8_t attune = static_cast<uint8_t>((c.elementA & 3) | ((c.elementB & 3) << 2));
        if (pl.spellElement == c.elementB && c.elementB != c.elementA) attune = static_cast<uint8_t>(attune | Snapshot::kAttuneActiveB);
        snap.attune = attune;
    }

    uint8_t Server::Impl::RemoteStatusOf(const Player& pl) const {
        return static_cast<uint8_t>((pl.state.status & 15) | ((pl.spellElement & 3) << 4));
    }

    uint8_t Server::Impl::RosterAttuneOf(uint8_t slot) const {
        sim::CasterState c;
        if (const auto it = castContext.casters.find(static_cast<uint8_t>(slot + 1)); it != castContext.casters.end()) c = it->second;
        return static_cast<uint8_t>((c.elementA & 3) | ((c.elementB & 3) << 2));
    }

    std::vector<sim::Zone> Server::Impl::BaselineZones() const {
        std::vector<sim::Zone> zones;
        const uint32_t tick = Tick();
        for (const sim::Zone& z : simulation->Zones().All())
            if (z.EndTick() > tick && zones.size() < kMaxBaselineZones) zones.push_back(z);
        return zones;
    }

    std::string Server::MagicLine() const {
        const ServerStats& st = m_Impl->stats;
        return std::format("Magic: server: casts {}, refused {}, ops {}, bytes {}, scheduled {}, sites {}", st.spellCasts, st.spellsRefused,
                           st.spellOps, st.spellBytes, m_Impl->scheduledGroups, m_Impl->sitesRun);
    }

    std::string Server::ZonesLine() const {
        const ServerStats& st = m_Impl->stats;
        return std::format("Zones: emitted {}, late 0, cancelled {}", st.zonesEmitted, st.zonesCancelled);
    }

    sim::CasterState Server::Caster(uint8_t slot) const {
        const auto it = m_Impl->castContext.casters.find(static_cast<uint8_t>(slot + 1));
        return it != m_Impl->castContext.casters.end() ? it->second : sim::CasterState{};
    }
}
