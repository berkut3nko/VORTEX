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

// Server::Impl: hash and entity checks, entity lists, model requests (docs/design/rules.md "File map"). [wp-6]
namespace vortex::net {
    using namespace detail;

    bool Server::Impl::OnEntityListRequest(Peer& p, std::span<const uint8_t> bytes) {
        EntityListRequest r;
        if (!Decode(bytes, r)) return false;
        if (p.listOwed) ++stats.entityListsMerged;
        else ++stats.entityListRequests;
        p.listOwed = true;
        return true;
    }

    void Server::Impl::EntityValues() {
        if (entityValuesValid && entityValuesAt == LastOpSeq()) return;
        entityValuesValid = true;
        entityValuesAt = LastOpSeq();
        entityValues = EntityCheck{};
        entityList.clear();
        const entt::registry& registry = simulation->Scene().Registry();
        std::unordered_map<uint32_t, ModelHash> seen;
        seen.reserve(modelHashes.size());
        for (const auto [entity, id, voxels, body] : registry.view<const scene::NetId, const scene::VoxelBody, const scene::RigidBody>().each()) {
            if (!voxels.model) continue;
            ModelHash cached;
            if (const auto it = modelHashes.find(id.value); it != modelHashes.end() && it->second.model.lock() == voxels.model) {
                cached = it->second;
            } else {
                cached.model = voxels.model;
                cached.hash = VolumeHash(voxels.model->Volume());
            }
            entityList.push_back({id.value, CheckModelHash(entity, cached.hash)});
            seen.emplace(id.value, std::move(cached));
        }
        modelHashes = std::move(seen);      // gone objects are forgotten
        std::sort(entityList.begin(), entityList.end(), [](const EntityListEntry& a, const EntityListEntry& b) { return a.netId < b.netId; });
        for (const EntityListEntry& e : entityList) {
            ++entityValues.count;
            entityValues.idXor ^= e.netId;
            entityValues.modelXor ^= EntityModelTerm(e.netId, e.modelHash);
        }
    }

    void Server::Impl::SendHashCheck(PeerId id, Peer& p) {
        voxel::World& world = simulation->World();
        HashCheckInput in;
        in.box = Intersect(p.window, RegionSyncBox(region));
        in.modified = &modified;
        in.recent = recentChanged;
        in.dense = [&world](const glm::ivec3& c) {
            const voxel::CellState state = world.StateOf(c);
            return state == voxel::CellState::Dense || state == voxel::CellState::Packed;
        };
        in.skip = [&p](const glm::ivec3& c) { return p.queued.contains(c); };
        in.feet = p.player ? glm::dvec3(p.player->state.position) : glm::dvec3(tmpl.spawn);
        in.velocity = p.player ? glm::dvec3(p.player->state.velocity) : glm::dvec3(0.0);
        in.chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(world.VoxelSize());
        in.tick = steppedTick;
        HashCheck check;
        check.asOfOpSeq = LastOpSeq();
        for (const HashCheckPick& pick : p.planner.Plan(in)) {
            HashCheckEntry e;
            e.chunk = pick.chunk;
            if (!voxel::ChunkHashAt(world, pick.chunk, e.hash)) continue;    // never in the server's window
            e.pristine = !modified.contains(pick.chunk);
            check.entries.push_back(e);
            ++stats.hashCheckEntries[static_cast<size_t>(pick.kind)];
            if (check.entries.size() >= kMaxHashCheckEntries) break;
        }
        if (check.entries.empty()) return;
        Send(id, Channel::Events, Encode(check));
        ++stats.hashChecksSent;
    }

    void Server::Impl::SendChecks(PeerId id, Peer& p) {
        if (p.sentUpTo != LastOpSeq()) return;     // events held (a test's maxEventBytesInFlight): later
        const auto start = std::chrono::steady_clock::now();
        // At most one list per peer per half check period (review of stage 7b): a client that asks
        // every tick gets one now and the rest merged into the next.
        const uint32_t listGap = settings.entityCheckTicks / 2;
        const bool listDue = !p.listSent || steppedTick - p.lastListTick >= listGap;
        if (p.listOwed && !listDue && !p.listDelayed) {
            p.listDelayed = true;     // once per delayed list, not per tick it waits (recheck of stage 7b)
            ++stats.entityListsDelayed;
        }
        if (p.listOwed && listDue) {
            p.listOwed = false;
            p.listDelayed = false;
            p.listSent = true;
            p.lastListTick = steppedTick;
            EntityValues();
            EntityList list;
            list.asOfOpSeq = LastOpSeq();
            list.entries = entityList;
            const std::vector<uint8_t> bytes = Encode(list);
            Send(id, Channel::Events, bytes);
            ++stats.entityListsSent;
            stats.entityListBytes += bytes.size();
        }
        const uint32_t tick = steppedTick;
        const uint32_t stagger = 17u * p.slot;
        if (const uint32_t n = settings.hashCheckTicks; n > 0 && p.hasWindow && tick % n == stagger % n) SendHashCheck(id, p);
        if (const uint32_t n = settings.entityCheckTicks; n > 0 && p.hasWindow && !p.baselineOpen && tick % n == (stagger + n / 2) % n) {
            EntityValues();
            EntityCheck check = entityValues;
            check.asOfOpSeq = LastOpSeq();
            Send(id, Channel::Events, Encode(check));
            ++stats.entityChecksSent;
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stats.checkSeconds += seconds;
        stats.maxCheckSeconds = std::max(stats.maxCheckSeconds, seconds);
    }

    bool Server::Impl::OnModelRequest(Peer& p, std::span<const uint8_t> bytes) {
        ModelRequest r;
        if (!Decode(bytes, r)) return false;
        for (const uint32_t netId : r.netIds) {
            ++stats.modelRequests;
            if (!lights.contains(netId) && EntityOf(netId) == scene::kNoEntity) {
                ++stats.requestsIgnored;      // gone: its EntityDespawn is in the stream
                continue;
            }
            // An answer still queued is encoded at its dequeue, after this request arrived: it
            // answers this one too (review of stage 7a).
            if (!p.modelsQueued.insert(netId).second) {
                ++stats.modelRequestRepeats;
                continue;
            }
            p.front.push_back({BulkItem::Kind::Entity, glm::ivec3(0), netId});
            if (p.front.size() > settings.maxQueuedAnswers) return false;
        }
        return true;
    }
}
