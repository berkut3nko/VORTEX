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

// Server::Impl: the cast queue, aimed and resolved through vortex.sim:rules (docs/design/rules.md "File map"). [wp-6]
namespace vortex::net {
    using namespace detail;

    void Server::Impl::CaptureCast(Peer& p, const sim::PlayerInput& input, const sim::PlayerState& at, bool late) {
        const sim::PlayerTuning& t = settings.player;
        QueuedCast q;
        q.ray = true;
        q.request.player = static_cast<uint8_t>(p.slot + 1);
        q.request.slot = sim::ActionSlot::Blast;
        q.request.inputTick = input.tick;
        q.request.eye = glm::dvec3(at.position) + glm::dvec3(0.0, static_cast<double>(t.eyeHeight), 0.0);
        q.request.dir = sim::LookDirection(input.yaw, input.pitch);
        q.power = voxel::QuantizePower(t.castPower);
        q.queuedTick = Tick();
        q.player = static_cast<uint8_t>(p.slot + 1);
        ++stats.castsFromInputs;
        stats.castsFromLateInputs += late ? 1u : 0u;
        ++stats.castsQueued;
        p.player->lastCastTick = input.tick;
        p.player->castRejected = CastRejection::None;
        queue.push_back(q);
    }

    void Server::Impl::RejectCast(Peer& p, uint32_t inputTick) {
        ++stats.castsOnCooldown;
        p.player->lastCastTick = inputTick;
        p.player->castRejected = CastRejection::Cooldown;
    }

    bool Server::Impl::Queue(const voxel::SphereQ& sphere, uint16_t power, uint8_t player) {
        ++stats.castsQueued;
        if (!sim::CastFitsRegion(region, simulation->World(), sphere)) {
            ++stats.castsRejected;
            return false;
        }
        QueuedCast q;
        q.sphere = sphere;
        q.power = power;
        q.queuedTick = Tick();
        q.player = player;
        queue.push_back(q);
        return true;
    }

    bool Server::Impl::ResolveRay(QueuedCast& q) {
        sim::ActionAim aim;
        const sim::ActionRefusal why = sim::AimAction(*simulation, q.request, aimRules, aim);
        if (aim.atBody) ++stats.castsAtBodies;        // as before: counted when a body took it, even if then Outside
        if (why == sim::ActionRefusal::Missed) ++stats.castsMissed;
        if (why == sim::ActionRefusal::Outside) ++stats.castsRejected;
        if (why == sim::ActionRefusal::None) {
            q.sphere = aim.sphere;
            return true;
        }
        if (Peer* p = PeerOfSlot(q.player); p != nullptr && p->player->lastCastTick == q.request.inputTick)
            p->player->castRejected = RejectionOf(why);
        return false;
    }

    void Server::Impl::LateCast(Peer& p, const sim::PlayerInput& in, uint32_t now) {
        Player& pl = *p.player;
        const sim::PlayerState* at = pl.StateAt(in.tick);
        const uint8_t cooldownThen = at != nullptr ? at->cooldownTicks : pl.state.cooldownTicks;
        if (cooldownThen > 0 || pl.state.cooldownTicks > 0) return RejectCast(p, in.tick);
        const uint32_t elapsed = now - 1 - in.tick;   // ticks simulated after the input's tick
        const uint32_t full = settings.player.castCooldownTicks;
        pl.state.cooldownTicks = static_cast<uint8_t>(full > elapsed ? full - elapsed : 0u);
        CaptureCast(p, in, at != nullptr ? *at : pl.state, true);
        // The kept states of the ticks since then get the cooldown they would have had, so that a
        // later late edge in the same stream is judged as its client predicted it (review of
        // stage 5: they still said 0, and a second edge a few ticks later was taken).
        // (StepPlayer counts down before it takes a cast: after tick in.tick + j the cooldown is full - j.)
        for (uint32_t k = std::max(in.tick, now > kStateHistory ? now - kStateHistory : 0u); k < now; ++k)
            if (sim::PlayerState* kept = pl.StateAt(k))
                kept->cooldownTicks = static_cast<uint8_t>(full > k - in.tick ? full - (k - in.tick) : 0u);
    }

    void Server::Impl::RunCast(const QueuedCast& q, bool overdue) {
        const uint32_t tick = Tick();
        const uint32_t firstOpSeq = LastOpSeq() + 1;
        // Wall time is logged only (shred.md R5): nothing below reads it to schedule.
        const auto start = std::chrono::steady_clock::now();
        sim::ActionAim aim;
        aim.hit = true;
        aim.sphere = q.sphere;
        aim.power = q.power;
        const sim::RulesContext context{settings.player, {}, tick};
        sim::ActionOutcome outcome = sim::ResolveAction(*simulation, context, q.request, aim);
        const sim::BlastResult& r = outcome.blasts.front();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        CastRecord rec;
        rec.player = q.player;
        rec.queuedTick = q.queuedTick;
        rec.tick = tick;
        rec.sphere = q.sphere;
        rec.power = q.power;
        rec.firstOpSeq = firstOpSeq;
        rec.overdue = overdue;
        rec.seconds = seconds;
        rec.searchErrors = r.search.islandBoxUnloaded + r.search.attachedByUnloaded + r.search.neckUnloaded;
        if (rec.searchErrors > 0) Fail("an island search met a chunk that is not loaded (the window must hold every flood box)");
        const int64_t creditBefore = eventCredit;
        // A cast the server could not run in full is not sent (the session has ended).
        for (const sim::AppliedOp& applied : r.ops) {
            if (failed || !CheckOp(applied.result)) break;
            EmitOp(applied.op, applied.result);
            ++rec.opCount;
        }
        // The objects it changed or made, and every despawn it caused (stage 6).
        EmitObjectEvents(r);
        rec.eventBytes = static_cast<uint32_t>(creditBefore - eventCredit);
        rec.work = sim::CastWorkOf(r);
        rec.islands = r.IslandCount();
        rec.crumbs = r.crumbs;
        rec.facePieces = r.search.facePieces;
        rec.neckPieces = r.search.neckPieces;
        rec.neckCrumbs = r.search.neckCrumbs;
        rec.inBoxIslands = rec.islands - std::min(rec.islands, rec.facePieces + rec.neckPieces);
        const uint64_t work = rec.work.Total();
        castCredit -= static_cast<int64_t>(work);
        ++stats.castsRun;
        stats.castsOverdue += overdue ? 1u : 0u;
        stats.castWork += work;
        stats.maxCastWork = std::max(stats.maxCastWork, work);
        stats.searchErrors += rec.searchErrors;
        stats.castSeconds += seconds;
        stats.maxCastSeconds = std::max(stats.maxCastSeconds, seconds);
        if (work >= 1000) {
            const double per10k = seconds * 1000.0 * 10000.0 / static_cast<double>(work);
            stats.maxCastMsPer10k = std::max(stats.maxCastMsPer10k, per10k);
            ++stats.castCostHistogram[std::min<size_t>(static_cast<size_t>(per10k * 10.0), stats.castCostHistogram.size() - 1)];
        }
        stats.islands += rec.islands;
        stats.crumbs += rec.crumbs;
        stats.inBoxIslands += rec.inBoxIslands;
        stats.facePieces += rec.facePieces;
        stats.neckPieces += rec.neckPieces;
        const uint32_t delay = tick - q.queuedTick;
        ++stats.castDelay[std::min<uint32_t>(delay, 7)];
        stats.maxCastDelay = std::max(stats.maxCastDelay, delay);
        if (overdue)
            Log::Warn("Server: a cast waited {} ticks and ran past the budgets (work credit {}, event credit {} bytes)", delay,
                      castCredit + static_cast<int64_t>(work), creditBefore);
        casts.push_back(std::move(rec));
        while (casts.size() > settings.castRecordLimit) casts.pop_front();
    }

    void Server::Impl::RunCasts() {
        const int64_t perTick = static_cast<int64_t>(settings.castWorkPerTick);
        const int64_t maxWait = static_cast<int64_t>(settings.maxCastWaitTicks);
        // Credit is never banked beyond one tick; debt (a cast larger than a tick) carries over,
        // bounded so that a long overload cannot stall the queue for longer than the wait limit.
        castCredit = std::clamp(castCredit + perTick, -perTick * maxWait, perTick);
        eventCredit = std::clamp(eventCredit + static_cast<int64_t>(settings.eventBytesPerTick),
                                 -static_cast<int64_t>(settings.eventBurstBytes), static_cast<int64_t>(settings.eventBurstBytes));
        // The entity budget: debt (an event larger than the credit) carries over, bounded.
        const int64_t entityBurst = static_cast<int64_t>(settings.entityEventBurstBytes);
        entityCredit = std::clamp(entityCredit + static_cast<int64_t>(settings.entityEventBytesPerTick), -8 * entityBurst, entityBurst);
        bool waitedForWork = false, waitedForBytes = false;
        while (!queue.empty() && !failed) {
            QueuedCast q = queue.front();
            const bool overdue = static_cast<int64_t>(Tick() - q.queuedTick) >= maxWait;
            if (!overdue && castCredit <= 0) {
                waitedForWork = true;
                break;
            }
            if (!overdue && eventCredit <= 0) {
                waitedForBytes = true;
                break;
            }
            queue.pop_front();
            // A player's ray is cast now, when the cast runs, on the world as the casts before it
            // left it; one that casts nothing costs nothing and the next cast is looked at.
            if (q.ray && !ResolveRay(q)) continue;
            // No new server body while the entity budget is spent: its pieces are client rubble.
            simulation->Replication().refuseNewPieces = entityCredit <= 0;
            RunCast(q, overdue && (castCredit <= 0 || eventCredit <= 0));
            simulation->Replication().refuseNewPieces = false;
        }
        stats.ticksWaitedForWork += waitedForWork ? 1u : 0u;
        stats.ticksWaitedForBytes += waitedForBytes ? 1u : 0u;
    }
}
