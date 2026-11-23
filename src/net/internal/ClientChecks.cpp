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

// Client::Impl: hash and entity checks, entity lists, model requests (docs/design/rules.md "File map"). [wp-6]
namespace vortex::net {
    using namespace detail;

    void Client::Impl::AddCheckTime(std::chrono::steady_clock::time_point start) {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stats.checkSeconds += seconds;
        stats.maxCheckSeconds = std::max(stats.maxCheckSeconds, seconds);
    }

    void Client::Impl::OnHashCheck(const HashCheck& check) {
        const auto start = std::chrono::steady_clock::now();
        const HashCheckOutcome o = sync.OnHashCheck(simulation->World(), check, applied);
        if (!o.compared) {
            ++stats.checksOutOfOrder;
            Log::Warn("Client: a HashCheck at opSeq {} came at opSeq {}: ignored", check.asOfOpSeq, applied);
            return;
        }
        ++stats.hashChecks;
        OnMismatches(o, check.asOfOpSeq);
        AddCheckTime(start);
    }

    void Client::Impl::CheckDeferred() {
        if (!sync.HasDeferredChecks()) return;
        const auto start = std::chrono::steady_clock::now();
        const uint32_t before = sync.Stats().deferredEqual + sync.Stats().deferredMismatches;
        const HashCheckOutcome o = sync.CheckDeferred(simulation->World(), applied);
        OnMismatches(o, applied);
        if (sync.Stats().deferredEqual + sync.Stats().deferredMismatches != before) AddCheckTime(start);
    }

    void Client::Impl::OnMismatches(const HashCheckOutcome& o, uint32_t opSeq) {
        for (const HashCheckOutcome::Mismatch& m : o.mismatches) {
            mismatched.insert(m.chunk);
            if (m.pristine) ++stats.mismatchesPristine;
            else ++stats.mismatchesModified;
            // A pristine chunk this client's generator made differently: the generator is suspect
            // (3 distinct ones start server terrain mode, M-b). Re-checks count once.
            if (m.pristine && m.generated) {
                pristineFailures.insert(m.chunk);
                stats.pristineFailures = static_cast<uint32_t>(pristineFailures.size());
            }
            if (m.refused) {
                Log::Error("Client: chunk ({}, {}, {}) differs from the server's again at opSeq {} soon after its repair ({}): not asked "
                           "for again - a bug to look at (its later differences are only counted)",
                           m.chunk.x, m.chunk.y, m.chunk.z, opSeq, m.pristine ? "pristine" : "modified");
                continue;
            }
            Log::Warn("Client: chunk ({}, {}, {}) differs from the server's at opSeq {} ({}{}{}{}): asked for again", m.chunk.x, m.chunk.y,
                      m.chunk.z, opSeq, m.pristine ? "pristine" : "modified", m.generated ? ", generated here" : ", from data",
                      m.repeat ? ", repaired before" : "", m.deferred ? ", compared once it had loaded" : "");
        }
        // Too many pristine chunks made differently here: this generator is not trusted any more
        // (stage 7b M-b). The whole window again, as the server holds it: a reset, with the flag.
        if (!terrainMode && settings.pristineFailuresForTerrainMode > 0 && pristineFailures.size() >= settings.pristineFailuresForTerrainMode) {
            StartTerrainMode(TerrainModeReason::PristineChecks);
            Log::Warn("Client: {} pristine chunks differed from the server's (tick {}): server terrain mode - the whole window is "
                      "asked for again",
                      pristineFailures.size(), stats.terrainModeTick);
            SendWindow(sync.Window(), true);
        }
    }

    EntityCheck Client::Impl::OwnEntityCheck(bool withAsked) const {
        EntityCheck c;
        for (const auto& [netId, copy] : copies) {
            ++c.count;
            c.idXor ^= netId;
            c.modelXor ^= EntityModelTerm(netId, copy.modelHash);
        }
        if (withAsked)
            for (const uint32_t netId : asked)
                if (!copies.contains(netId)) {
                    ++c.count;
                    c.idXor ^= netId;
                }
        return c;
    }

    void Client::Impl::OnEntityCheck(const EntityCheck& check) {
        const auto start = std::chrono::steady_clock::now();
        if (applied != check.asOfOpSeq) {
            ++stats.checksOutOfOrder;
            Log::Warn("Client: an EntityCheck at opSeq {} came at opSeq {}: ignored", check.asOfOpSeq, applied);
            return;
        }
        if (!baselineEnded || listOpen) {
            ++stats.entityChecksSkipped;
            return;
        }
        if (check.asOfOpSeq < entityAsOf) {
            ++stats.entityChecksAhead;
            return;
        }
        const bool partial = !asked.empty() || !held.empty() || !modelsToAsk.empty();
        const EntityCheck own = OwnEntityCheck(partial);
        ++(partial ? stats.entityChecksPartial : stats.entityChecks);
        const bool match = own.count == check.count && own.idXor == check.idXor && (partial || own.modelXor == check.modelXor);
        AddCheckTime(start);
        if (match) {
            listsInARow = 0;
            return;
        }
        ++stats.entityMismatches;
        // Back-off: a client that cannot make a copy (bodies full) does not ask every 2 s.
        const uint32_t now = newestSnapshot;
        if (listsInARow >= settings.entityListsBeforeBackoff) {
            const uint32_t wait = settings.entityListBackoffTicks * (listsInARow == settings.entityListsBeforeBackoff ? 1u : 3u);
            if (now - lastListTick < wait) {
                ++stats.entityListsBackedOff;
                return;
            }
        }
        Log::Warn("Client: the replicated objects differ from the server's at opSeq {} ({} here{}, {} there): asking for the list",
                  check.asOfOpSeq, own.count, partial ? " with models asked for" : "", check.count);
        ++listsInARow;
        lastListTick = now;
        listOpen = true;
        listToSend = true;
        listCheckSeq = check.asOfOpSeq;
    }

    void Client::Impl::AskModel(uint32_t netId) {
        if (asked.insert(netId).second) modelsToAsk.push_back(netId);
    }

    void Client::Impl::RemoveCopy(uint32_t netId) {
        const auto it = copies.find(netId);
        if (it == copies.end()) return;
        simulation->Scene().Destroy(it->second.entity);
        bodies.Remove(netId);
        copies.erase(it);
    }

    void Client::Impl::OnEntityList(const EntityList& list) {
        const auto start = std::chrono::steady_clock::now();
        listOpen = false;
        if (applied != list.asOfOpSeq) {
            ++stats.checksOutOfOrder;
            Log::Warn("Client: an EntityList at opSeq {} came at opSeq {}: ignored", list.asOfOpSeq, applied);
            return;
        }
        ++stats.entityLists;
        std::unordered_map<uint32_t, uint64_t> listed;
        listed.reserve(list.entries.size());
        for (const EntityListEntry& e : list.entries) listed.emplace(e.netId, e.modelHash);
        std::vector<uint32_t> drop;
        for (const auto& [netId, copy] : copies)
            if (!listed.contains(netId) && copy.base <= list.asOfOpSeq) drop.push_back(netId);
        std::sort(drop.begin(), drop.end());
        for (const uint32_t netId : drop) {
            RemoveCopy(netId);
            Tombstone(netId);
            ++stats.copiesDropped;
        }
        // A NetId asked for that the list does not have died at or before Q (every ask came from an
        // event at or below Q, and a despawn processed since would have cleared it): no answer
        // will come (the server answers nothing for a gone NetId), so it is forgotten - else
        // Synced() would wait for it for ever (review of 7b M-a).
        std::vector<uint32_t> gone;
        for (const uint32_t netId : asked)
            if (!listed.contains(netId) && !copies.contains(netId)) gone.push_back(netId);
        std::sort(gone.begin(), gone.end());
        for (const uint32_t netId : gone) {
            asked.erase(netId);
            std::erase(modelsToAsk, netId);
            if (const auto h = held.find(netId); h != held.end()) {
                heldCount -= h->second.size();
                stats.droppedHeldEvents += static_cast<uint32_t>(h->second.size());
                held.erase(h);
            }
            Tombstone(netId);
            ++stats.copiesDropped;
        }
        for (const EntityListEntry& e : list.entries) {
            if (const auto it = copies.find(e.netId); it != copies.end()) {
                if (it->second.base > list.asOfOpSeq || it->second.modelHash == e.modelHash) continue;
                RemoveCopy(e.netId);
                AskModel(e.netId);
                ++stats.copiesReplaced;
                continue;
            }
            if (asked.contains(e.netId)) continue;      // its answer is on the way
            Untombstone(e.netId);                       // alive at Q on the server: the list wins
            AskModel(e.netId);
            ++stats.copiesRestored;
        }
        Log::Info("Client: entity list at opSeq {}: {} object(s); {} dropped, {} asked for again", list.asOfOpSeq, list.entries.size(),
                  drop.size(), modelsToAsk.size());
        AddCheckTime(start);
    }
}
