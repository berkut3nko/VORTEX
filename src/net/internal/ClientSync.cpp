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

// Client::Impl: the window, server terrain mode, applying and holding events, bulk, cast presentation (docs/design/rules.md "File map"). [wp-6]
namespace vortex::net {
    using namespace detail;

    void Client::Impl::SendWindow(const SyncBox& window, bool reset) {
        ++windowSeq;
        sync.BeginWindow(window, windowSeq, reset);
        syncBox = Intersect(window, RegionSyncBox(welcome.region));
        WindowUpdate u;
        u.windowSeq = windowSeq;
        u.flags = reset ? WindowUpdate::kReset : uint8_t{0};
        if (terrainMode) u.flags |= WindowUpdate::kTerrainMode;      // sticky: every update from the switch on
        u.minChunk = window.min;
        u.size = window.size;
        transport.Send(server, Channel::Events, Encode(u));
        ++stats.windowUpdates;
        stats.windowResets += reset ? 1u : 0u;
    }

    void Client::Impl::StartTerrainMode(TerrainModeReason why) {
        if (terrainMode) return;
        terrainMode = true;
        terrainReason = why;
        if (sessionGenerator) sessionGenerator->SetTerrainMode(true);
        sync.SetTerrainMode(true);
        stats.terrainModeTick = why == TerrainModeReason::PristineChecks ? newestSnapshot : 0u;
    }

    void Client::Impl::CheckWindow() {
        if (state != ClientState::Playing || !simulation || server == 0) return;
        voxel::World& world = simulation->World();
        const uint64_t revision = world.WindowRevision();
        const std::vector<glm::ivec3> steps = world.TakeWindowSteps();
        if (revision == windowRevision) return;
        const uint64_t moves = revision - windowRevision;
        windowRevision = revision;
        if (steps.size() == moves) {
            // Every step in order (review of stage 7a: the game streams every frame and polls on
            // ticks, so two steps can fall between two Polls): a chunk that left at one step and
            // came back at the next enters again with the second update, and the server sends it.
            for (const glm::ivec3& min : steps) {
                const SyncBox window{min, world.WindowSize()};
                if (window != sync.Window()) SendWindow(window, false);
            }
            return;
        }
        // Steps this client did not see apart (never in play: every Recenter is listed): a chunk
        // may have left and come back regenerated, so the whole window is synced again.
        Log::Info("Client: the window moved {} times between two checks ({} steps listed): every chunk of it is synced again", moves,
                  steps.size());
        SendWindow(SyncBox{world.WindowMinChunk(), world.WindowSize()}, true);
    }

    void Client::Impl::Present(const sim::WorldOp& op, sim::OpResult result, std::vector<voxel::VoxelEdit> edits) {
        sim::AppliedOp applied{op, std::move(result), std::move(edits)};
        const voxel::World& world = simulation->World();
        if (op.kind == sim::WorldOpKind::CarveSphere) {
            sim::BlastResult blast;
            // The cast's cue, as the rules seam makes it on the server (rules.md "Outcome"); a client never resolves.
            const sim::EffectCue cue = sim::BlastCueOf(op);
            blast.center = sim::CueCentreMetres(cue, world.VoxelSize());
            blast.radius = sim::CueRadiusMetres(cue, world.VoxelSize());
            castShown = true;
            castCenter = blast.center;
            castRadius = blast.radius;
            blast.islandsSearched = true;
            blast.ops.push_back(std::move(applied));
            presentations.push_back(std::move(blast));
            groupOpen = true;
        } else if (op.kind == sim::WorldOpKind::CarveCapsule) {
            // A bore opens a group of its own: its islands follow it, as a blast's do; it has no sphere to flash.
            sim::BlastResult bore;
            const double toMetres = static_cast<double>(world.VoxelSize()) / static_cast<double>(voxel::kQuantaPerVoxel);
            bore.center = (glm::dvec3(op.shape.a) + glm::dvec3(op.shape.b)) * (0.5 * toMetres);
            bore.islandsSearched = true;
            bore.ops.push_back(std::move(applied));
            presentations.push_back(std::move(bore));
            groupOpen = true;
        } else if (op.kind == sim::WorldOpKind::Detach || op.kind == sim::WorldOpKind::ClearMask) {
            if (!groupOpen) {
                sim::BlastResult late;
                late.deferred = true;
                presentations.push_back(std::move(late));
                groupOpen = true;
            }
            if (op.kind == sim::WorldOpKind::ClearMask) presentations.back().crumbs += 1;
            presentations.back().ops.push_back(std::move(applied));
        } else {
            CloseGroup();
        }
    }

    void Client::Impl::Apply(uint32_t seq, const Event& event) {
        if (const sim::WorldOp* op = std::get_if<sim::WorldOp>(&event)) {
            // Only live events are shown: what came before the join point is history.
            const bool present = settings.presentation && seq > welcome.joinOpSeq;
            // A piece with a NetId is a copy of a server body, made from the voxels it removed.
            const bool replicated = op->kind == sim::WorldOpKind::Detach && op->netId != 0;
            std::vector<voxel::VoxelEdit> edits;
            // The base rule (stage 7a): applied only to the chunks that are current with a base
            // below `seq`; an operation that skipped a chunk it touches is not complete here.
            const ChunkSync::OpOutcome out = sync.Apply(simulation->World(), seq, *op, present || replicated ? &edits : nullptr);
            const sim::OpResult& r = out.result;
            stats.skippedChunks += static_cast<uint32_t>(r.report.skipped.size());
            stats.unsyncedChunks += static_cast<uint32_t>(r.report.unsynced.size());
            ++stats.opsApplied;
            if (replicated && !tombstones.contains(op->netId) && !copies.contains(op->netId)) {
                if (out.complete) {
                    voxel::VoxelVolume piece = sim::BuildDetachedPiece(*op, edits);
                    if (!piece.IsEmpty())
                        MakeCopy(op->netId, std::make_shared<voxel::VoxelModel>(std::move(piece), voxel::kWorldPalette, "piece"),
                                 glm::vec3(simulation->World().VoxelToWorld(op->min)), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), false, seq);
                } else if (asked.insert(op->netId).second) {
                    // Some of its voxels are not here as the server had them: never a piece from a
                    // partial volume; the server sends the body (a BaselineEntity on bulk).
                    modelsToAsk.push_back(op->netId);
                }
            }
            if (present && out.complete) Present(*op, r, std::move(edits));
            else CloseGroup();
            return;
        }
        CloseGroup();
        if (std::holds_alternative<ZoneStartEvent>(event) || std::holds_alternative<ZoneCancelEvent>(event) || std::holds_alternative<CastFx>(event))
            return ApplyMagic(event);
        ApplyEntity(seq, event);
    }

    void Client::Impl::ApplyMagic(const Event& event) {
        sim::ZoneSet& zones = simulation->Zones();
        if (const ZoneStartEvent* e = std::get_if<ZoneStartEvent>(&event)) {
            sim::Zone zone = e->zone;
            if (const auto early = earlyCancels.find(zone.zoneId); early != earlyCancels.end()) {
                if (early->second >= zone.startTick) zone.duration = static_cast<uint16_t>(std::min<uint32_t>(zone.duration, early->second - zone.startTick));
                earlyCancels.erase(early);
            }
            if (zone.duration > 0 && zones.Insert(zone)) ++stats.zonesApplied;
            else ++stats.zonesIgnored;
        } else if (const ZoneCancelEvent* c = std::get_if<ZoneCancelEvent>(&event)) {
            const sim::Zone* z = zones.Find(c->zoneId);
            if (z != nullptr && c->endTick < z->startTick) {
                ++stats.zonesIgnored;
                Log::Warn("Client: a ZoneCancel of zone {} ends at tick {}, before its start {}: ignored", c->zoneId, c->endTick, z->startTick);
            } else if (z != nullptr) {
                zones.Cancel(c->zoneId, c->endTick);
                ++stats.zoneCancels;
            } else if (earlyCancels.size() < sim::kMaxZoneRecords) {
                earlyCancels[c->zoneId] = c->endTick;
            }
        } else if (const CastFx* fx = std::get_if<CastFx>(&event)) {
            ++stats.castFx;
            if (settings.presentation) {
                if (castFx.size() >= kMaxCastFx) castFx.pop_front();
                castFx.push_back(*fx);
            }
        }
    }

    void Client::Impl::Hold(uint32_t netId, uint32_t seq, const Event& event) {
        std::vector<std::pair<uint32_t, Event>>& list = held[netId];
        if (list.size() >= kMaxHeldPerId || heldCount >= kMaxHeldEvents) {
            // Too many: they are dropped and the model is asked for again, also when a request is
            // out already - an answer encoded before this event would miss it (its server dedups
            // a request whose answer is still queued). An answer older than this event is
            // discarded when it comes (review of stage 7a).
            stats.droppedHeldEvents += static_cast<uint32_t>(list.size()) + 1u;
            heldCount -= list.size();
            held.erase(netId);
            uint32_t& newest = staleBefore[netId];
            newest = std::max(newest, seq);
            asked.insert(netId);
            modelsToAsk.push_back(netId);
            return;
        }
        list.emplace_back(seq, event);
        ++heldCount;
        ++stats.heldEntityEvents;
    }

    void Client::Impl::ProcessBulk() {
        if (state != ClientState::Playing || !simulation) return;
        const auto start = std::chrono::steady_clock::now();
        uint32_t installs = 0;
        // Consecutive ChunkData are installed together: their chunks catch up with one walk of
        // the ring (recheck of stage 7a, 3). Flushed before any other message keeps its place.
        std::vector<ChunkData> chunks;
        const auto flush = [&] {
            if (chunks.empty()) return;
            sync.OnChunkData(simulation->World(), chunks, applied);
            chunks.clear();
        };
        while (!pendingBulk.empty() && state == ClientState::Playing) {
            const std::optional<MessageType> next = PeekType(pendingBulk.front());
            const bool install = next == MessageType::ChunkData || next == MessageType::BaselineEntity;
            // A budget per Poll, in arrival order (the server sends nearest first; the markers
            // keep their place after the entities before them).
            if (install && settings.maxBulkPerPoll > 0 && installs >= settings.maxBulkPerPoll) break;
            installs += install ? 1u : 0u;
            const std::vector<uint8_t> bytes = std::move(pendingBulk.front());
            pendingBulk.pop_front();
            const std::optional<MessageType> type = PeekType(bytes);
            if (type != MessageType::ChunkData) flush();
            if (type == MessageType::BaselineNear) {
                BaselineNear n;
                if (!Decode(bytes, n)) return Fail("malformed BaselineNear");
                baselineNear = true;
                entityAsOf = std::max(entityAsOf, n.asOfOpSeq);
                if (stats.baselineNearTicks == 0) stats.baselineNearTicks = TicksSinceWelcome();
            } else if (type == MessageType::ChunkData) {
                ChunkData d;
                if (!Decode(bytes, d)) return Fail("malformed ChunkData");
                ++stats.chunkData;
                {
                    // Stage 8 (O3): sent before, and sent again unchanged (the same RLE bytes).
                    uint64_t h = 0xcbf29ce484222325ull;
                    for (const uint8_t b : d.rle) h = (h ^ b) * 0x100000001b3ull;
                    const auto [seen, fresh] = chunkDataSeen.try_emplace(d.chunk, h);
                    if (!fresh) {
                        ++stats.chunkDataRepeats;
                        stats.chunkDataRepeatBytes += bytes.size();
                        if (seen->second == h) {
                            ++stats.chunkDataUnchanged;
                            stats.chunkDataUnchangedBytes += bytes.size();
                        }
                        seen->second = h;
                    }
                }
                if (terrainMode) {
                    ++stats.terrainChunkData;
                    stats.terrainChunkBytes += bytes.size();
                }
                chunks.push_back(std::move(d));
            } else if (type == MessageType::BaselineEntity) {
                BaselineEntity b;
                if (!Decode(bytes, b)) return Fail("malformed BaselineEntity");
                OnBaselineEntity(b);
            } else if (type == MessageType::BaselineEnd) {
                BaselineEnd e;
                if (!Decode(bytes, e)) return Fail("malformed BaselineEnd");
                // [mag-2] The zones live or pending at its opSeq; one the live stream already brought is kept as it is.
                for (const sim::Zone& z : e.zones)
                    if (simulation->Zones().Find(z.zoneId) == nullptr) ApplyMagic(Event{ZoneStartEvent{z}});
                baselineEnded = true;
                entityAsOf = std::max(entityAsOf, e.asOfOpSeq);
                baselineNear = true;
                const uint32_t ticks = TicksSinceWelcome();
                if (stats.baselineNearTicks == 0) stats.baselineNearTicks = ticks;
                if (stats.baselineEndTicks == 0) stats.baselineEndTicks = ticks;
                // Held events of NetIds that no baseline holds and nobody asked for: their copies
                // will not come (they died, or were never replicated to this client).
                for (auto it = held.begin(); it != held.end();) {
                    if (asked.contains(it->first)) {
                        ++it;
                        continue;
                    }
                    stats.droppedHeldEvents += static_cast<uint32_t>(it->second.size());
                    heldCount -= it->second.size();
                    it = held.erase(it);
                }
            } else {
                return Fail("an unknown message on the bulk channel");
            }
        }
        flush();
        if (installs > 0) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            stats.bulkSeconds += seconds;
            stats.maxBulkSeconds = std::max(stats.maxBulkSeconds, seconds);
            stats.maxBulkInPoll = std::max(stats.maxBulkInPoll, installs);
        }
    }

    void Client::Impl::SyncHousekeeping() {
        if (state != ClientState::Playing || !simulation || server == 0) return;
        voxel::World& world = simulation->World();
        const std::vector<glm::ivec3> loaded = world.TakeLoadedChunks();
        if (!loaded.empty()) sync.OnLoaded(world, loaded, applied);
        const uint32_t capacity = simulation->Chunks().Capacity(), used = simulation->Chunks().UsedCount();
        stats.maxStoreUsed = std::max(stats.maxStoreUsed, used);
        // A full store (networking.md "Chunk sync"): such chunks are Unsynced - not trusted, not
        // hashed, not walkable - until slots free and they are requested whole. Logged as it grows.
        if (const uint32_t unsynced = sync.Stats().unsynced; unsynced != unsyncedLogged) {
            Log::Warn("Client: {} chunk(s) found no store slot ({} of {} slots used): unsynced until slots free ({} in all)",
                      unsynced - unsyncedLogged, used, capacity, unsynced);
            unsyncedLogged = unsynced;
        }
        const glm::ivec3 near = voxel::VoxelToChunk(world.WorldToVoxel(glm::dvec3(player.position)));
        const std::vector<glm::ivec3> chunks =
            sync.TakeRequests(capacity - std::min(used, capacity), settings.requestReserveSlots, near, kMaxChunkRequest);
        if (!chunks.empty()) {
            stats.chunkRequests += static_cast<uint32_t>(chunks.size());
            transport.Send(server, Channel::Events, Encode(ChunkRequest{chunks}));
        }
        // Check entries kept while their chunks loaded (review of stage 7b), then the HashCheck
        // mismatches (stage 7b): the chunks again, with reason Repair (the server counts them).
        CheckDeferred();
        if (state != ClientState::Playing) return;
        if (const std::vector<glm::ivec3> repairs = sync.TakeRepairs(kMaxChunkRequest); !repairs.empty()) {
            stats.chunkRequests += static_cast<uint32_t>(repairs.size());
            stats.repairsRequested += static_cast<uint32_t>(repairs.size());
            transport.Send(server, Channel::Events, Encode(ChunkRequest{repairs, ChunkRequestReason::Repair}));
        }
        if (listToSend) {
            listToSend = false;
            ++stats.entityListRequests;
            transport.Send(server, Channel::Events, Encode(EntityListRequest{listCheckSeq}));
        }
        for (size_t i = 0; i < modelsToAsk.size(); i += kMaxModelRequest) {
            ModelRequest m;
            m.netIds.assign(modelsToAsk.begin() + static_cast<std::ptrdiff_t>(i),
                            modelsToAsk.begin() + static_cast<std::ptrdiff_t>(std::min(modelsToAsk.size(), i + kMaxModelRequest)));
            stats.modelRequests += static_cast<uint32_t>(m.netIds.size());
            transport.Send(server, Channel::Events, Encode(m));
        }
        modelsToAsk.clear();
        // Synced() walks the window's records (101 376 for the game's window): looked at every 16th
        // Poll only until the join has synced once.
        if (stats.joinSyncTicks == 0 && ++syncPolls % 16 == 0) {
            if (stats.joinDataTicks == 0 && baselineEnded) {
                const ChunkSyncCounts n = sync.Counts();
                if (n.noBase == 0 && n.awaiting == 0 && n.requested == 0 && n.unsynced == 0) {
                    stats.joinDataTicks = std::max(1u, newestSnapshot - welcome.tick);
                    stats.joinDataSeconds = SinceWelcome();
                }
            }
            if (Synced()) NoteSynced();
        }
    }

    void Client::Impl::NoteSynced() {
        if (stats.joinSyncTicks != 0) return;
        stats.joinSyncTicks = std::max(1u, newestSnapshot - welcome.tick);
        stats.joinSyncSeconds = SinceWelcome();
        if (stats.joinDataTicks == 0) {
            stats.joinDataTicks = stats.joinSyncTicks;
            stats.joinDataSeconds = stats.joinSyncSeconds;
        }
    }

    bool Client::Impl::Synced() const {
        return state == ClientState::Playing && simulation && baselineEnded && asked.empty() && held.empty() && modelsToAsk.empty() &&
               !listOpen && sync.Synced();
    }
}
