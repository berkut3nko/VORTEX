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

// Server::Impl: chunk sync, window updates, server terrain mode and the bulk pacer (docs/design/rules.md "File map"). [wp-6]
namespace vortex::net {
    using namespace detail;

    bool Server::Impl::QueueChunk(Peer& p, const glm::ivec3& c) {
        if (!p.queued.insert(c).second) return false;
        p.chunks.push_back(c);
        p.sortedTail = 0;             // the back is no longer the nearest: selected again before use
        return true;
    }

    double Server::Impl::ChunkDistance(const Peer& p, const glm::ivec3& c) const {
        const double size = static_cast<double>(voxel::kChunkSize) * static_cast<double>(simulation->World().VoxelSize());
        const glm::dvec3 player = p.player ? glm::dvec3(p.player->state.position) : glm::dvec3(tmpl.spawn);
        if (p.terrainMode) {
            return glm::length((glm::dvec3(c) + glm::dvec3(0.5)) * size - (player + glm::dvec3(0.0, kEyeMetres, 0.0)));
        }
        const glm::dvec2 centre = (glm::dvec2(c.x, c.z) + glm::dvec2(0.5)) * size;
        return glm::length(centre - glm::dvec2(player.x, player.z));
    }

    void Server::Impl::SortChunks(Peer& p, bool force) {
        const glm::ivec3 at = p.player ? voxel::VoxelToChunk(simulation->World().WorldToVoxel(glm::dvec3(p.player->state.position)))
                                       : glm::ivec3(0);
        if (!force && at == p.sortedFor && (p.sortedTail > 0 || p.chunks.empty())) return;
        p.sortedFor = at;
        if (p.chunks.empty()) {
            p.sortedTail = 0;
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        const double size = static_cast<double>(voxel::kChunkSize) * static_cast<double>(simulation->World().VoxelSize());
        const glm::dvec3 player = p.player ? glm::dvec3(p.player->state.position) : glm::dvec3(tmpl.spawn);
        const glm::dvec3 eye = player + glm::dvec3(0.0, kEyeMetres, 0.0);
        const auto key = [&](const glm::ivec3& c) {
            if (p.terrainMode) {
                const glm::dvec3 d = (glm::dvec3(c) + glm::dvec3(0.5)) * size - eye;
                return glm::dot(d, d);
            }
            const glm::dvec2 d = (glm::dvec2(c.x, c.z) + glm::dvec2(0.5)) * size - glm::dvec2(player.x, player.z);
            return glm::dot(d, d);
        };
        std::vector<std::pair<double, glm::ivec3>> keyed;
        keyed.reserve(p.chunks.size());
        for (const glm::ivec3& c : p.chunks) keyed.emplace_back(key(c), c);
        const auto farther = [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return std::tie(a.second.y, a.second.z, a.second.x) > std::tie(b.second.y, b.second.z, b.second.x);
        };
        if (keyed.size() > kSortedTail) {
            const auto tail = keyed.end() - static_cast<std::ptrdiff_t>(kSortedTail);
            std::nth_element(keyed.begin(), tail, keyed.end(), farther);
            std::sort(tail, keyed.end(), farther);
            p.sortedTail = kSortedTail;
        } else {
            std::sort(keyed.begin(), keyed.end(), farther);
            p.sortedTail = keyed.size();
        }
        for (size_t i = 0; i < keyed.size(); ++i) p.chunks[i] = keyed[i].second;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stats.queueSortSeconds += seconds;
        stats.maxQueueSortSeconds = std::max(stats.maxQueueSortSeconds, seconds);
    }

    void Server::Impl::PopChunk(Peer& p) {
        p.chunks.pop_back();
        if (p.sortedTail > 0) --p.sortedTail;
    }

    void Server::Impl::PruneQueue(Peer& p, const SyncBox& box) {
        const size_t before = p.chunks.size();
        std::erase_if(p.chunks, [&](const glm::ivec3& c) {
            if (box.Contains(c)) return false;
            p.queued.erase(c);
            if (p.terrainMode) TerrainChunkGone(p, c, 0);
            return true;
        });
        stats.queuePruned += before - p.chunks.size();
        if (p.chunks.size() != before) p.sortedTail = 0;     // selected again before the next use
    }

    void Server::Impl::TerrainQueued(Peer& p, const glm::ivec3& c, uint32_t seq) {
        if (const auto it = p.terrainQueuedBy.find(c); it != p.terrainQueuedBy.end()) {
            if (it->second == seq) return;
            TerrainChunkGone(p, c, 0);
        }
        p.terrainQueuedBy[c] = seq;
        ++p.terrainSteps[seq].pending;
    }

    void Server::Impl::TerrainChunkGone(Peer& p, const glm::ivec3& c, uint64_t bytes) {
        const auto it = p.terrainQueuedBy.find(c);
        if (it == p.terrainQueuedBy.end()) return;
        const uint32_t seq = it->second;
        p.terrainQueuedBy.erase(it);
        if (seq == p.terrainFirstSeq) p.terrainJoinDataBytes += bytes;
        const auto step = p.terrainSteps.find(seq);
        if (step == p.terrainSteps.end()) return;
        step->second.bytes += bytes;
        if (step->second.pending > 0) --step->second.pending;
        if (step->second.pending == 0) FinishTerrainStep(p, seq);
    }

    void Server::Impl::FinishTerrainStep(Peer& p, uint32_t seq) {
        const auto step = p.terrainSteps.find(seq);
        if (step == p.terrainSteps.end()) return;
        if (seq != p.terrainFirstSeq) {
            ++p.terrainStepCount;
            p.terrainStepBytes += step->second.bytes;
            p.terrainMaxStepBytes = std::max(p.terrainMaxStepBytes, step->second.bytes);
        }
        p.terrainSteps.erase(step);
    }

    bool Server::Impl::OnWindowUpdate(PeerId id, Peer& p, std::span<const uint8_t> bytes) {
        WindowUpdate u;
        if (!Decode(bytes, u) || u.windowSeq != p.windowSeq + 1) return false;
        // Server terrain mode is sticky (stage 7b choice 12): a peer that asked for it once and
        // then clears the flag would disagree with the server about what the answer means.
        const bool terrain = (u.flags & WindowUpdate::kTerrainMode) != 0;
        if (p.terrainMode && !terrain) return false;
        const glm::ivec2 rows = sim::RegionRows(region);
        if (u.minChunk.y != rows.x || u.size.y != rows.y - rows.x) return false;
        ++stats.windowUpdates;
        if (!p.hasWindow) tickEvents |= kTickFirstWindow;
        const SyncBox box{u.minChunk, u.size};
        const bool all = WindowEntersWhole(!p.hasWindow, (u.flags & WindowUpdate::kReset) != 0, p.window, box);
        if (terrain) return OnTerrainWindow(id, p, u, box, all);
        if (p.hasWindow) PruneQueue(p, box);
        WindowAck ack;
        ack.windowSeq = u.windowSeq;
        ack.modified = voxel::VoxelMask(u.size);
        uint32_t marked = 0;
        const SyncBox server = RegionSyncBox(region);
        const SyncBox inside = Intersect(box, server);
        size_t fresh = 0;
        auto mark = [&](const glm::ivec3& c) {
            ack.modified.Set(c - box.min, true);
            ++marked;
            fresh += QueueChunk(p, c) ? 1u : 0u;
        };
        const size_t cells = static_cast<size_t>(inside.size.x) * static_cast<size_t>(inside.size.y) * static_cast<size_t>(inside.size.z);
        if (inside.Empty()) {
            // Nothing of the server's window: every chunk of it is pristine for good.
        } else if (modified.size() < cells) {
            // Fewer modified chunks than cells in the box (early in a session, a large window): one
            // pass over the map. The queue's order does not depend on the map's (SortChunks).
            for (const auto& [c, seq] : modified)
                if (inside.Contains(c) && (all || !p.window.Contains(c))) mark(c);
        } else {
            for (int y = inside.min.y; y < inside.Max().y; ++y)
                for (int z = inside.min.z; z < inside.Max().z; ++z)
                    for (int x = inside.min.x; x < inside.Max().x; ++x) {
                        const glm::ivec3 c(x, y, z);
                        if (!all && p.window.Contains(c)) continue;
                        if (modified.contains(c)) mark(c);
                    }
        }
        p.hasWindow = true;
        p.window = box;
        p.windowSeq = u.windowSeq;
        if (fresh > 0) SortChunks(p, true);
        p.maxQueued = std::max(p.maxQueued, static_cast<uint32_t>(p.chunks.size()));
        // The events made so far go first: a client takes the ack with them applied.
        Pump(id, p);
        ack.asOfOpSeq = LastOpSeq();
        Send(id, Channel::Events, Encode(ack));
        ++stats.windowAcks;
        stats.ackedModified += marked;
        if (u.windowSeq == 1) {
            p.join.windowChunks = static_cast<uint32_t>(box.size.x * box.size.y * box.size.z);
            p.join.modifiedChunks = marked;
        }
        return true;
    }

    bool Server::Impl::OnTerrainWindow(PeerId id, Peer& p, const WindowUpdate& u, const SyncBox& box, bool all) {
        const auto start = std::chrono::steady_clock::now();
        if (p.hasWindow) PruneQueue(p, box);
        const bool step = p.terrainMode && !all;
        const size_t direction = box.min.x != p.window.min.x ? (box.min.z != p.window.min.z ? 2u : 0u) : 1u;
        if (!p.terrainMode) {
            p.terrainMode = true;
            p.terrainFirstSeq = u.windowSeq;
            ++stats.terrainModePeers;
            if (settings.terrainBulkCeilingBytes > settings.bulkPacer.ceilingBytes) {
                // Stage 8, O2: a larger bulk window for a peer that receives its whole window.
                BulkPacerSettings ts = settings.bulkPacer;
                ts.ceilingBytes = settings.terrainBulkCeilingBytes;
                p.pacer = std::make_unique<BulkPacer>(ts);
            }
            Log::Info("Server: client {} asks for server terrain mode at window update {}: the whole window comes from here", id,
                      u.windowSeq);
        }
        const voxel::World& world = simulation->World();
        const SyncBox server = RegionSyncBox(region);
        TerrainAck ack;
        ack.windowSeq = u.windowSeq;
        ack.size = u.size;
        uint32_t queuedNow = 0, uniform = 0;
        const auto add = [&ack](TerrainCode code, uint8_t id) {
            if (!ack.runs.empty() && ack.runs.back().code == code && ack.runs.back().id == id) {
                ++ack.runs.back().count;
                return;
            }
            ack.runs.push_back({1, code, id});
        };
        // One code per entering cell in ForEachEnteringChunk order (protocol 10, review of stage 7b):
        // a step visits and codes its entering slab only, never the whole box.
        ForEachEnteringChunk(box, p.window, all, [&](const glm::ivec3& c) {
            if (!server.Contains(c)) {
                add(TerrainCode::NotInAck, 0);
                return;
            }
            switch (world.StateOf(c)) {
                case voxel::CellState::Empty:
                    add(TerrainCode::Uniform, 0);
                    ++uniform;
                    return;
                case voxel::CellState::Uniform: {
                    uint8_t id = 0;
                    (void)world.GetChunk(c, &id);
                    add(TerrainCode::Uniform, id);
                    ++uniform;
                    return;
                }
                case voxel::CellState::Packed:
                    add(TerrainCode::HiddenData, 0);
                    ++stats.terrainHidden;
                    break;
                default:          // Dense (the server's window is always loaded)
                    add(TerrainCode::Data, 0);
                    ++stats.terrainData;
                    break;
            }
            if (QueueChunk(p, c)) ++queuedNow;
            TerrainQueued(p, c, u.windowSeq);
        });
        stats.terrainUniform += uniform;
        p.hasWindow = true;
        p.window = box;
        p.windowSeq = u.windowSeq;
        if (queuedNow > 0) SortChunks(p, true);
        p.maxQueued = std::max(p.maxQueued, static_cast<uint32_t>(p.chunks.size()));
        Pump(id, p);
        ack.asOfOpSeq = LastOpSeq();
        const std::vector<uint8_t> bytes = Encode(ack);
        Send(id, Channel::Events, bytes);
        ++stats.windowAcks;
        ++stats.terrainAcks;
        stats.terrainAckBytes += bytes.size();
        p.terrainSteps[u.windowSeq].bytes += bytes.size();
        if (step) {
            ++p.ackSteps[direction];
            p.ackStepBytes[direction] += bytes.size();
            p.maxAckStepBytes[direction] = std::max<uint64_t>(p.maxAckStepBytes[direction], bytes.size());
        }
        if (p.terrainSteps[u.windowSeq].pending == 0) FinishTerrainStep(p, u.windowSeq);
        if (u.windowSeq == p.terrainFirstSeq) {
            p.terrainJoinAckBytes = bytes.size();
            p.terrainJoinChunks = queuedNow;
            p.terrainJoinUniform = uniform;
        }
        if (u.windowSeq == 1) {
            p.join.windowChunks = static_cast<uint32_t>(box.size.x * box.size.y * box.size.z);
            p.join.modifiedChunks = queuedNow;
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stats.terrainAckSeconds += seconds;
        stats.maxTerrainAckSeconds = std::max(stats.maxTerrainAckSeconds, seconds);
        if (step) {
            ++stats.terrainStepAcks;
            stats.terrainStepAckSeconds += seconds;
            stats.maxTerrainStepAckSeconds = std::max(stats.maxTerrainStepAckSeconds, seconds);
        }
        return true;
    }

    bool Server::Impl::OnChunkRequest(Peer& p, std::span<const uint8_t> bytes) {
        ChunkRequest r;
        if (!Decode(bytes, r)) return false;
        const SyncBox server = RegionSyncBox(region);
        const bool repair = r.reason == ChunkRequestReason::Repair;
        for (const glm::ivec3& c : r.chunks) {
            ++stats.chunkRequests;
            stats.repairsRequested += repair ? 1u : 0u;
            ++p.join.requests;
            if (settings.countChunkRequests) ++requestCounts[c];
            if (!p.hasWindow || !p.window.Contains(c) || !server.Contains(c)) {
                ++stats.requestsIgnored;
                continue;
            }
            // Answers go first; one queued further back moves to the front.
            if (p.queued.contains(c)) {
                const auto it = std::find(p.chunks.begin(), p.chunks.end(), c);
                if (it == p.chunks.end()) continue;      // already in the front
                if (static_cast<size_t>(p.chunks.end() - it) <= p.sortedTail) --p.sortedTail;
                p.chunks.erase(it);
            }
            p.queued.insert(c);
            p.front.push_back({BulkItem::Kind::Chunk, c, 0, repair});
            if (p.front.size() > settings.maxQueuedAnswers) return false;
        }
        return true;
    }

    std::vector<uint8_t> Server::Impl::NextBulk(Peer& p, bool& any) {
        any = true;
        BulkItem item;
        bool end = false;
        SortChunks(p, false);         // the sorted tail may have run out
        if (!p.front.empty()) {
            item = p.front.front();
            p.front.pop_front();
            if (item.kind == BulkItem::Kind::Entity) p.modelsQueued.erase(item.netId);
        } else if (!p.chunks.empty() && ChunkDistance(p, p.chunks.back()) <= static_cast<double>(settings.bulkNearMetres)) {
            item = {BulkItem::Kind::Chunk, p.chunks.back(), 0};
            PopChunk(p);
        } else if (p.baselineOpen && !p.nearSent && p.nearLeft == 0) {
            // The entities near the spawn are out: the joiner's start gate may open (the far
            // ones follow; their events are held meanwhile).
            p.nearSent = true;
            ++stats.baselineNear;
            return Encode(BaselineNear{LastOpSeq(), p.baselineSent});
        } else if (p.baselineOpen && !p.baseline.empty()) {
            item = {BulkItem::Kind::Entity, glm::ivec3(0), p.baseline.front()};
            p.baseline.pop_front();
            ++p.baselineSent;
            if (p.nearLeft > 0) --p.nearLeft;
        } else if (p.baselineOpen) {
            end = true;
        } else if (!p.chunks.empty()) {
            item = {BulkItem::Kind::Chunk, p.chunks.back(), 0};
            PopChunk(p);
        } else {
            any = false;
            return {};
        }
        if (end) {
            p.baselineOpen = false;
            return Encode(BaselineEnd{LastOpSeq(), p.baselineSent});
        }
        if (item.kind == BulkItem::Kind::Chunk) {
            p.queued.erase(item.chunk);
            ChunkData data;
            data.chunk = item.chunk;
            data.asOfOpSeq = LastOpSeq();
            if (!p.window.Contains(item.chunk) || !simulation->World().EncodeChunkAt(item.chunk, data.rle)) {
                ++stats.chunksDropped;
                if (p.terrainMode) TerrainChunkGone(p, item.chunk, 0);
                return {};
            }
            std::vector<uint8_t> bytes = Encode(data);
            if (p.terrainMode) {
                ++stats.terrainChunkData;
                stats.terrainChunkBytes += bytes.size();
                TerrainChunkGone(p, item.chunk, bytes.size());
            }
            ++stats.chunkData;
            stats.repairsSent += item.repair ? 1u : 0u;
            stats.chunkDataBytes += bytes.size();
            ++p.join.chunkData;
            p.join.chunkBytes += bytes.size();
            return bytes;
        }
        BaselineEntity e;
        if (!MakeBaselineEntity(item.netId, e)) {
            ++stats.baselineDropped;
            if (p.baselineOpen && p.baselineSent > 0) --p.baselineSent;
            return {};
        }
        std::vector<uint8_t> bytes = Encode(e);
        ++stats.baselineEntities;
        stats.baselineBytes += bytes.size();
        ++p.join.baseline;
        p.join.baselineBytes += bytes.size();
        return bytes;
    }

    void Server::Impl::PumpBulk(PeerId id, Peer& p) {
        if (!p.pacer) return;
        const double dt = static_cast<double>(simulation->FixedTimeStep());
        size_t inFlight = transport.ReliableBytesInFlight(id);
        size_t bulkInFlight = transport.ChannelBytesInFlight(id, Channel::Bulk);
        // The live events handed over since the last tick count in what drained (the window is the
        // peer's whole reliable traffic; review of stage 7a).
        p.pacer->Sent(p.eventBytesSince);
        p.eventBytesSince = 0;
        p.pacer->Update(inFlight, transport.RoundTripSeconds(id), dt);
        const size_t window = p.pacer->Window();
        stats.minBulkWindow = stats.minBulkWindow == 0 ? window : std::min(stats.minBulkWindow, window);
        stats.maxBulkWindow = std::max(stats.maxBulkWindow, window);
        p.join.minWindow = p.join.minWindow == 0 ? window : std::min(p.join.minWindow, window);
        p.join.maxWindow = std::max(p.join.maxWindow, window);
        const double cap = settings.maxBulkBytesPerSecond;
        if (cap > 0.0) p.bulkCredit = std::min(p.bulkCredit + cap * dt, std::max(cap * dt, 4096.0));
        SortChunks(p, false);
        const auto encodeStart = std::chrono::steady_clock::now();
        bool any = true;
        while (any) {
            const bool pending = !p.heldBulk.empty() || !p.front.empty() || !p.chunks.empty() || p.baselineOpen;
            if (!pending) break;
            // Under the window, always (recheck of stage 7a, 1): a few events in flight are under it,
            // so they never keep the bulk from moving; events that fill it by themselves do.
            if (!p.pacer->MayHandOver(inFlight) || (cap > 0.0 && p.bulkCredit <= 0.0)) {
                ++stats.bulkHeld;
                break;
            }
            std::vector<uint8_t> bytes = std::move(p.heldBulk);
            p.heldBulk.clear();
            if (bytes.empty()) bytes = NextBulk(p, any);
            if (bytes.empty()) continue;
            // A message that does not fit the window waits until the bulk in flight has drained,
            // then goes alone (the bytes in flight are under the window here): the window is never
            // overshot by more than one message (plus the live events, which are never held).
            const bool fits = inFlight + bytes.size() <= window;
            if (!fits && bulkInFlight > 0) {
                p.heldBulk = std::move(bytes);
                ++stats.bulkHeld;
                break;
            }
            stats.bulkAlone += fits ? 0u : 1u;
            stats.bulkOverWindow += inFlight >= window ? 1u : 0u;
            Send(id, Channel::Bulk, bytes);
            inFlight += bytes.size();
            bulkInFlight += bytes.size();
            p.pacer->Sent(bytes.size());
            if (cap > 0.0) p.bulkCredit -= static_cast<double>(bytes.size());
            stats.bulkBytes += bytes.size();
            stats.bulkOversize += bytes.size() > 16384 ? 1u : 0u;
            stats.maxBulkMessage = std::max(stats.maxBulkMessage, bytes.size());
            if (Tick() - p.join.secondStart >= 60) {
                p.join.secondStart = Tick();
                p.join.secondBytes = 0;
            }
            p.join.secondBytes += bytes.size();
            p.join.peakRate = std::max(p.join.peakRate, static_cast<double>(p.join.secondBytes));
        }
        const double encoded = std::chrono::duration<double>(std::chrono::steady_clock::now() - encodeStart).count();
        encodeTickSeconds += encoded;
        stats.encodeSeconds += encoded;
        p.join.encodeSeconds += encoded;
        const bool empty = p.heldBulk.empty() && p.front.empty() && p.chunks.empty() && !p.baselineOpen;
        // Stage 8: the join ends when its data is acknowledged, not when it was handed over (on a
        // slow link the last window of bulk is still in flight then).
        const size_t bulkNow = transport.ChannelBytesInFlight(id, Channel::Bulk);
        stats.maxBulkInFlight = std::max(stats.maxBulkInFlight, bulkNow);
        if (empty && p.hasWindow && !p.join.done && bulkNow == 0) p.join.done = true;
        if (empty && p.hasWindow && !p.join.logged) {
            p.join.logged = true;
            const double seconds = static_cast<double>(Tick() - p.join.joinTick) * dt;
            Log::Info("Join: client {} (slot {}): window {} chunks, {} modified; {} ChunkData / {:.1f} KiB, {} baseline entities / {:.1f} "
                      "KiB, {} requests; queue empty after {:.2f} s; bulk window {:.1f}..{:.1f} KiB, peak {:.1f} KiB/s; bulk work {:.1f} ms "
                      "(the server's busiest tick so far {:.2f} ms)",
                      id, p.slot, p.join.windowChunks, p.join.modifiedChunks, p.join.chunkData,
                      static_cast<double>(p.join.chunkBytes) / 1024.0, p.join.baseline, static_cast<double>(p.join.baselineBytes) / 1024.0,
                      p.join.requests, seconds, static_cast<double>(p.join.minWindow) / 1024.0, static_cast<double>(p.join.maxWindow) / 1024.0,
                      p.join.peakRate / 1024.0, p.join.encodeSeconds * 1000.0, stats.maxEncodeSecondsPerTick * 1000.0);
        }
    }
}
