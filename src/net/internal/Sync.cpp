module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <cmath>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

module vortex.net;

import vortex.voxel;
import vortex.sim;

namespace vortex::net {

    namespace {
        int FloorModInt(int a, int b) {
            const int m = a % b;
            return m < 0 ? m + b : m;
        }

        /// Calls `fn` for every chunk of [lo, hi] (inclusive) in (y, z, x) order, World's edit order.
        template <typename Fn>
        void ForEachChunk(const glm::ivec3& lo, const glm::ivec3& hi, Fn&& fn) {
            for (int y = lo.y; y <= hi.y; ++y)
                for (int z = lo.z; z <= hi.z; ++z)
                    for (int x = lo.x; x <= hi.x; ++x) fn(glm::ivec3(x, y, z));
        }

        bool InRange(const glm::ivec3& c, const glm::ivec3& lo, const glm::ivec3& hi) {
            return c.x >= lo.x && c.y >= lo.y && c.z >= lo.z && c.x <= hi.x && c.y <= hi.y && c.z <= hi.z;
        }

        void Merge(sim::OpResult& into, const sim::OpResult& r) {
            into.changed += r.changed;
            into.report.skipped.insert(into.report.skipped.end(), r.report.skipped.begin(), r.report.skipped.end());
            into.report.unsynced.insert(into.report.unsynced.end(), r.report.unsynced.begin(), r.report.unsynced.end());
            into.report.changed.insert(into.report.changed.end(), r.report.changed.begin(), r.report.changed.end());
            into.report.outOfBounds = into.report.outOfBounds || r.report.outOfBounds;
        }
    }

    SyncBox Intersect(const SyncBox& a, const SyncBox& b) {
        SyncBox out;
        out.min = glm::max(a.min, b.min);
        const glm::ivec3 max = glm::min(a.Max(), b.Max());
        out.size = glm::max(max - out.min, glm::ivec3(0));
        return out;
    }

    SyncBox RegionSyncBox(const sim::RegionDesc& region) {
        const glm::ivec2 rows = sim::RegionRows(region);
        SyncBox box;
        box.min = sim::RegionWindowMinChunk(region);
        box.min.y = rows.x;
        const glm::ivec3 size = sim::RegionWindowSize(region);
        box.size = glm::ivec3(size.x, rows.y - rows.x, size.z);
        return box;
    }

    // --- OpRing ----------------------------------------------------------------------------------

    void OpRing::Reset(uint32_t seq) {
        m_Ops.clear();
        m_Bytes = 0;
        m_Floor = seq;
    }

    void OpRing::Push(uint32_t seq, const sim::WorldOp& op) {
        Entry e;
        e.seq = seq;
        if (!sim::OpChunkRange(op, e.lo, e.hi)) {
            e.lo = glm::ivec3(0);
            e.hi = glm::ivec3(-1);
        }
        // What a replay needs: the voxels' operation. A Detach's NetId and push are presentation.
        ByteWriter w;
        if (op.kind == sim::WorldOpKind::Detach && (op.netId != 0 || op.velocity != glm::vec3(0.0f))) {
            sim::WorldOp bare = sim::WorldOp::Detach(op.min, op.mask, glm::vec3(0.0f), 0);
            WriteEvent(w, Event{std::move(bare)});
        } else {
            WriteEvent(w, Event{op});
        }
        e.bytes = w.Take();
        m_Bytes += e.bytes.size();
        m_Ops.push_back(std::move(e));
        while (!m_Ops.empty() && (m_Ops.size() > m_MaxOps || m_Bytes > m_MaxBytes)) {
            m_Floor = m_Ops.front().seq;
            m_Bytes -= m_Ops.front().bytes.size();
            m_Ops.pop_front();
        }
    }

    void OpRing::ForEachAfter(uint32_t base, const glm::ivec3& chunk, const std::function<void(uint32_t, const sim::WorldOp&)>& fn) const {
        Walk(
            base,
            [&](uint32_t, const glm::ivec3& lo, const glm::ivec3& hi) { return InRange(chunk, lo, hi); },
            [&](uint32_t seq, const glm::ivec3&, const glm::ivec3&, const sim::WorldOp& op) { fn(seq, op); });
    }

    void OpRing::Walk(uint32_t base, const std::function<bool(uint32_t, const glm::ivec3&, const glm::ivec3&)>& wants,
                      const std::function<void(uint32_t, const glm::ivec3&, const glm::ivec3&, const sim::WorldOp&)>& fn) const {
        // Entries are in stream order: start at the first one after `base` (a chunk that loads just
        // after its ack scans only the few operations since, not the whole ring; stage 7a M-b).
        const auto first = std::partition_point(m_Ops.begin(), m_Ops.end(), [base](const Entry& e) { return e.seq <= base; });
        for (auto it = first; it != m_Ops.end(); ++it) {
            const Entry& e = *it;
            if (!wants(e.seq, e.lo, e.hi)) continue;
            ByteReader in(e.bytes);
            Event event;
            if (!ReadEvent(in, event)) continue;     // the bytes were written here: cannot happen
            const sim::WorldOp* op = std::get_if<sim::WorldOp>(&event);
            if (!op) continue;
            ++m_Decodes;
            fn(e.seq, e.lo, e.hi, *op);
        }
    }

    bool OpRing::TouchesNear(uint32_t after, uint32_t upTo, const glm::ivec3& chunk, uint32_t newest) const {
        if (upTo > newest || !Covers(after)) return true;
        const auto first = std::partition_point(m_Ops.begin(), m_Ops.end(), [after](const Entry& e) { return e.seq <= after; });
        for (auto it = first; it != m_Ops.end() && it->seq <= upTo; ++it) {
            if (it->hi.x < it->lo.x) return true;     // no chunk range known: assume it does
            if (InRange(chunk, it->lo - glm::ivec3(1), it->hi + glm::ivec3(1))) return true;
        }
        return false;
    }

    // --- ChunkSync -------------------------------------------------------------------------------

    ChunkSync::Record* ChunkSync::Find(const glm::ivec3& c) {
        if (!m_Window.Contains(c) || m_Records.empty()) return nullptr;
        const glm::ivec3 s = m_Window.size;
        Record& r = m_Records[static_cast<size_t>((FloorModInt(c.x, s.x) + FloorModInt(c.z, s.z) * s.x) * s.y + FloorModInt(c.y, s.y))];
        return r.assigned && r.coord == c ? &r : nullptr;
    }

    const ChunkSync::Record* ChunkSync::Find(const glm::ivec3& c) const { return const_cast<ChunkSync*>(this)->Find(c); }

    void ChunkSync::SetState(Record& r, ChunkSyncState state) {
        if (r.state == ChunkSyncState::Unsynced) --m_Unsynced;
        if (state == ChunkSyncState::Unsynced) ++m_Unsynced;
        r.state = state;
        Recount(r);
    }

    void ChunkSync::Recount(Record& r) {
        const bool incoming = r.assigned && (r.state == ChunkSyncState::NoBase || r.state == ChunkSyncState::Requested ||
                                             (r.state == ChunkSyncState::Awaiting && !(r.hidden && m_PacksHidden)));
        if (incoming == r.incoming) return;
        r.incoming = incoming;
        if (incoming) ++m_Incoming;
        else --m_Incoming;
    }

    template <typename Fn>
    void ChunkSync::ForEachEntering(const SyncBox& box, const SyncBox& prev, bool all, Fn&& fn) const {
        // Rows fixed: whole columns enter (one or two slabs for a step, not the whole box).
        ForEachEnteringChunk(box, prev, all, std::forward<Fn>(fn));
    }

    void ChunkSync::Start(uint32_t joinOpSeq, const SyncBox& serverWindow) {
        m_Ring.Reset(joinOpSeq);
        m_Applied = joinOpSeq;
        m_ServerWindow = serverWindow;
    }

    void ChunkSync::BeginWindow(const SyncBox& window, uint32_t seq, bool reset) {
        bool all = WindowEntersWhole(m_Records.empty(), reset, m_Window, window);
        if (window.size != m_Window.size || m_Records.empty()) {
            m_Records.assign(static_cast<size_t>(window.size.x) * static_cast<size_t>(window.size.y) * static_cast<size_t>(window.size.z), Record{});
            m_Unsynced = 0;
            m_Incoming = 0;
            all = true;
        }
        const SyncBox prev = m_Window;
        m_Window = window;
        const glm::ivec3 s = window.size;
        // Only the cells that entered (review of stage 7a: 2112 records per chunk of a step of the game's
        // window - a session client steps 2 chunks, 4224 - not 101 376); every cell of the previous
        // window still in this one keeps its record.
        ForEachEntering(window, prev, all, [&](const glm::ivec3& c) {
            ++m_Stats.recordsVisited;
            Record& r = m_Records[static_cast<size_t>((FloorModInt(c.x, s.x) + FloorModInt(c.z, s.z) * s.x) * s.y + FloorModInt(c.y, s.y))];
            if (r.assigned && r.coord == c && !reset) return;
            if (r.assigned && r.state == ChunkSyncState::Unsynced) --m_Unsynced;
            if (r.assigned && r.incoming) --m_Incoming;
            r = Record{};
            r.coord = c;
            r.assigned = true;
            r.entered = seq;
            r.neverEdited = !m_ServerWindow.Contains(c);
            Recount(r);                   // a new record is NoBase
        });
        // A chunk owed a request that left the window is not owed any more; after a reset every
        // chunk waits for this update's ack (which marks the modified ones), none is owed.
        if (reset) {
            m_Owed.clear();
            m_Repairs.clear();
            m_Deferred.clear();
        } else {
            std::erase_if(m_Owed, [this](const glm::ivec3& c) { return Find(c) == nullptr; });
            std::erase_if(m_Repairs, [this](const glm::ivec3& c) { return Find(c) == nullptr; });
        }
        m_Sent.push_back({seq, window, prev, all});
    }

    void ChunkSync::CatchUp(voxel::World& world, std::vector<Record*>& records, uint32_t applied) {
        // (y, z, x) order, each record once (a chunk may load or be installed twice in one batch).
        std::sort(records.begin(), records.end(), [](const Record* a, const Record* b) {
            return std::tie(a->coord.y, a->coord.z, a->coord.x) < std::tie(b->coord.y, b->coord.z, b->coord.x);
        });
        records.erase(std::unique(records.begin(), records.end()), records.end());
        struct Replay {
            Record* r = nullptr;
            bool ok = true;
        };
        std::vector<Replay> replays;
        uint32_t from = applied;
        for (Record* r : records) {
            r->ready = false;
            if (r->state != ChunkSyncState::Based) continue;     // a later install of the batch found no slot
            if (r->neverEdited || r->base >= applied) {
                r->ready = true;
                continue;
            }
            if (!m_Ring.Covers(r->base)) {
                // Never a partial replay: the whole chunk again.
                SetState(*r, ChunkSyncState::Requested);
                m_Owed.push_back(r->coord);
                ++m_Stats.notCovered;
                continue;
            }
            replays.push_back({r, true});
            from = std::min(from, r->base);
        }
        if (replays.empty()) return;
        const auto needs = [](const Replay& p, uint32_t seq, const glm::ivec3& lo, const glm::ivec3& hi) {
            return p.ok && seq > p.r->base && InRange(p.r->coord, lo, hi);
        };
        m_Ring.Walk(
            from,
            [&](uint32_t seq, const glm::ivec3& lo, const glm::ivec3& hi) {
                return std::any_of(replays.begin(), replays.end(), [&](const Replay& p) { return needs(p, seq, lo, hi); });
            },
            [&](uint32_t seq, const glm::ivec3& lo, const glm::ivec3& hi, const sim::WorldOp& op) {
                for (Replay& p : replays) {
                    if (!needs(p, seq, lo, hi)) continue;
                    const voxel::ChunkClip clip = voxel::ChunkClip::One(p.r->coord);
                    const sim::OpResult res = sim::ApplyOp(world, op, nullptr, &clip);
                    ++m_Stats.replays;
                    if (!res.report.unsynced.empty()) {
                        SetState(*p.r, ChunkSyncState::Unsynced);
                        ++m_Stats.unsynced;
                        p.ok = false;
                    } else if (!res.report.skipped.empty()) {
                        p.ok = false;      // not loaded after all: its load report replays it
                    }
                }
            });
        for (const Replay& p : replays) p.r->ready = p.ok;
        m_Stats.opsDecoded = m_Ring.Decodes();
    }

    bool ChunkSync::OnAck(voxel::World& world, const WindowAck& ack, uint32_t applied) {
        const auto sent = std::find_if(m_Sent.begin(), m_Sent.end(), [&](const SentWindow& w) { return w.seq == ack.windowSeq; });
        if (sent == m_Sent.end() || ack.modified.Size() != sent->box.size) return false;
        const SentWindow w = *sent;
        m_Sent.erase(m_Sent.begin(), sent + 1);
        ++m_Stats.acks;
        std::vector<Record*> loaded;      // caught up together (recheck of stage 7a, 3)
        // The records this ack can base entered with its window: its entering cells only.
        ForEachEntering(w.box, w.prev, w.all, [&](const glm::ivec3& c) {
            ++m_Stats.recordsVisited;
            Record* r = Find(c);
            if (!r || r->state != ChunkSyncState::NoBase || r->entered != ack.windowSeq) return;
            if (ack.modified.Get(c - w.box.min) && !r->neverEdited) {
                SetState(*r, ChunkSyncState::Awaiting);
                ++m_Stats.ackedModified;
                return;
            }
            SetState(*r, ChunkSyncState::Based);
            r->base = ack.asOfOpSeq;
            r->ready = false;
            if (world.IsChunkLoaded(c)) loaded.push_back(r);
        });
        CatchUp(world, loaded, applied);
        return true;
    }

    bool ChunkSync::OnTerrainAck(voxel::World& world, const TerrainAck& ack, uint32_t applied) {
        const auto sent = std::find_if(m_Sent.begin(), m_Sent.end(), [&](const SentWindow& w) { return w.seq == ack.windowSeq; });
        if (sent == m_Sent.end() || ack.size != sent->box.size) return false;
        const SentWindow w = *sent;
        m_Sent.erase(m_Sent.begin(), sent + 1);
        ++m_Stats.acks;
        ++m_Stats.terrainAcks;
        m_PacksHidden = world.PacksHidden();
        // One code per entering cell, in ForEachEnteringChunk order (protocol 10): the runs are read
        // with a cursor as the walk goes; they must cover the entering cells exactly.
        size_t run = 0;
        uint32_t used = 0;                // cells of ack.runs[run] already taken
        uint64_t cells = 0;
        for (const TerrainRun& tr : ack.runs) cells += tr.count;
        uint64_t entering = 0;
        ForEachEntering(w.box, w.prev, w.all, [&](const glm::ivec3&) { ++entering; });
        if (cells != entering) return false;     // an entering cell undescribed, or one too many
        bool ok = true;
        std::vector<Record*> based;       // caught up together (applied == A: nothing to replay, normally)
        ForEachEntering(w.box, w.prev, w.all, [&](const glm::ivec3& c) {
            ++m_Stats.recordsVisited;
            const TerrainRun code{1, ack.runs[run].code, ack.runs[run].id};
            if (++used == ack.runs[run].count) {
                ++run;
                used = 0;
            }
            if (!ok) return;
            Record* r = Find(c);
            if (!r || r->state != ChunkSyncState::NoBase || r->entered != ack.windowSeq) return;
            if (r->neverEdited) {
                // Outside the server's window: this client's generator makes it, nothing edits it (7a).
                if (code.code != TerrainCode::NotInAck) ok = false;
                SetState(*r, ChunkSyncState::Based);
                r->base = ack.asOfOpSeq;
                r->ready = false;
                if (world.IsChunkLoaded(c)) based.push_back(r);
                return;
            }
            switch (code.code) {
                case TerrainCode::NotInAck: ok = false; return;     // an entering chunk the server did not describe
                case TerrainCode::Data:
                case TerrainCode::HiddenData:
                    r->hidden = code.code == TerrainCode::HiddenData;
                    SetState(*r, ChunkSyncState::Awaiting);
                    r->ackedAt = ack.asOfOpSeq;
                    ++m_Stats.awaitingData;
                    m_Stats.awaitingHidden += r->hidden ? 1u : 0u;
                    return;
                case TerrainCode::Uniform: {
                    const voxel::CellState state = world.StateOf(c);
                    uint8_t id = 0;
                    if (state == voxel::CellState::Uniform) (void)world.GetChunk(c, &id);
                    const bool same = (state == voxel::CellState::Empty && code.id == 0) || (state == voxel::CellState::Uniform && id == code.id);
                    if (same) {
                        ++m_Stats.uniformBased;
                    } else {
                        const std::vector<uint8_t> rle = code.id == 0 ? voxel::EncodeChunk(voxel::ChunkKind::Empty, 0, nullptr)
                                                                      : voxel::EncodeChunk(voxel::ChunkKind::Uniform, code.id, nullptr);
                        if (world.SetChunkEncoded(c, rle, voxel::InstallMode::Content) != voxel::SetChunkResult::Installed) {
                            ok = false;     // a uniform chunk needs no slot, and the cell is in the window
                            return;
                        }
                        ++m_Stats.uniformInstalled;
                    }
                    r->fromData = true;
                    SetState(*r, ChunkSyncState::Based);
                    r->base = ack.asOfOpSeq;
                    r->ready = false;
                    based.push_back(r);
                    return;
                }
            }
        });
        CatchUp(world, based, applied);
        return ok;
    }

    ChunkSync::OpOutcome ChunkSync::Apply(voxel::World& world, uint32_t seq, const sim::WorldOp& op, std::vector<voxel::VoxelEdit>* edits) {
        OpOutcome out;
        m_Ring.Push(seq, op);
        m_Applied = seq;
        glm::ivec3 lo, hi;
        if (!sim::OpChunkRange(op, lo, hi)) {
            m_Stats.deferredDropped += static_cast<uint32_t>(m_Deferred.size());
            m_Deferred.clear();
            out.result = sim::ApplyOp(world, op, edits);
            return out;
        }
        // A kept check entry is the server's chunk only while no operation may have changed it.
        if (!m_Deferred.empty())
            m_Stats.deferredDropped += static_cast<uint32_t>(std::erase_if(m_Deferred, [&](const DeferredCheck& d) { return InRange(d.chunk, lo, hi); }));
        const auto passes = [&](const glm::ivec3& c) {
            const Record* r = Find(c);
            return r && Current(*r) && seq > r->base;
        };
        bool all = true;
        ForEachChunk(lo, hi, [&](const glm::ivec3& c) { all = all && passes(c); });
        std::vector<glm::ivec3> blocked;      // chunks the op touches that do not pass
        if (!all)
            ForEachChunk(lo, hi, [&](const glm::ivec3& c) {
                if (!passes(c) && sim::OpTouchesChunk(op, c)) blocked.push_back(c);
            });
        if (blocked.empty()) {
            // Every chunk it can change passes: the whole operation (the fast path).
            out.result = sim::ApplyOp(world, op, edits);
            ++m_Stats.opsFast;
            ForEachChunk(lo, hi, [&](const glm::ivec3& c) {
                if (Record* r = Find(c); r && passes(c)) ++r->appliedOps;
            });
        } else {
            ++m_Stats.opsClipped;
            ++m_Stats.opsIncomplete;
            out.complete = false;
            ForEachChunk(lo, hi, [&](const glm::ivec3& c) {
                if (!passes(c)) return;
                const voxel::ChunkClip clip = voxel::ChunkClip::One(c);
                Merge(out.result, sim::ApplyOp(world, op, edits, &clip));
                ++Find(c)->appliedOps;
            });
            for (const glm::ivec3& c : blocked) {
                ++m_Stats.chunkSkips;
                if (Record* r = Find(c); r && r->state == ChunkSyncState::Based && seq <= r->base) ++r->skippedOps;
                // Terrain mode (review of stage 7b): the server may have changed this chunk, which
                // shows its face neighbours there; neither its later data (a Content install, not an
                // edit) nor anything else would show them here.
                if (m_TerrainMode) world.ShowServerHiddenNeighbours(c);
            }
        }
        // What the world could not apply to a chunk that passed: not loaded after all (its load
        // report replays it), or no store slot (Unsynced: requested whole once slots free).
        for (const glm::ivec3& c : out.result.report.skipped)
            if (Record* r = Find(c)) r->ready = false;
        for (const glm::ivec3& c : out.result.report.unsynced)
            if (Record* r = Find(c)) {
                SetState(*r, ChunkSyncState::Unsynced);
                r->ready = false;
                ++m_Stats.unsynced;
            }
        if (!out.result.report.skipped.empty() || !out.result.report.unsynced.empty()) {
            if (out.complete) ++m_Stats.opsIncomplete;
            out.complete = false;
        }
        return out;
    }

    ChunkSync::DataOutcome ChunkSync::Install(voxel::World& world, const ChunkData& data, uint32_t applied, std::vector<Record*>& installed) {
        Record* r = Find(data.chunk);
        if (!r || r->neverEdited || (r->state == ChunkSyncState::Based && r->base >= data.asOfOpSeq)) {
            ++m_Stats.staleChunkData;
            return DataOutcome::Stale;
        }
        // Terrain mode (stage 7b): the server's content, packed as the server packs it. A chunk the
        // ack called hidden is installed packed only if no operation over it or a face neighbour came
        // between the ack and the stream's position (an edit there may have shown it on the server;
        // World::SetChunkEncoded also shows it when a face neighbour is an edit here); every other
        // install - repairs, re-requests, data the server holds Dense - is shown content, never an edit.
        voxel::InstallMode mode = voxel::InstallMode::Edit;
        if (m_TerrainMode) {
            mode = voxel::InstallMode::Content;
            // The whole interval up to the stream's position (review of stage 7b): an operation after
            // the data was encoded may have shown it on the server too, and the edit of a neighbour that
            // was not current here leaves nothing on this world that would show it later.
            if (r->hidden && r->state == ChunkSyncState::Awaiting &&
                !m_Ring.TouchesNear(r->ackedAt, std::max(data.asOfOpSeq, applied), data.chunk, applied))
                mode = voxel::InstallMode::HiddenContent;
        }
        const voxel::SetChunkResult res = world.SetChunkEncoded(data.chunk, data.rle, mode);
        if (res == voxel::SetChunkResult::StoreFull) {
            if (r->state != ChunkSyncState::Unsynced) ++m_Stats.unsynced;
            SetState(*r, ChunkSyncState::Unsynced);
            r->ready = false;
            return DataOutcome::StoreFull;
        }
        if (res != voxel::SetChunkResult::Installed) {
            ++m_Stats.staleChunkData;
            return DataOutcome::Stale;
        }
        ++m_Stats.chunksInstalled;
        if (m_TerrainMode) ++(world.StateOf(data.chunk) == voxel::CellState::Packed ? m_Stats.installedHidden : m_Stats.installedShown);
        r->hidden = false;
        std::erase(m_Owed, data.chunk);
        std::erase(m_Repairs, data.chunk);
        if (r->repair) {
            ++m_Stats.repairsInstalled;
            r->repair = false;
            r->repaired = true;
            r->repairs = static_cast<uint8_t>(std::min<int>(r->repairs + 1, 255));
            r->repairedAtCheck = m_Stats.hashChecks;
        }
        r->fromData = true;
        SetState(*r, ChunkSyncState::Based);
        r->base = data.asOfOpSeq;
        r->ready = false;
        installed.push_back(r);
        return DataOutcome::Installed;
    }

    ChunkSync::DataOutcome ChunkSync::OnChunkData(voxel::World& world, const ChunkData& data, uint32_t applied) {
        std::vector<Record*> installed;
        const DataOutcome out = Install(world, data, applied, installed);
        CatchUp(world, installed, applied);
        return out;
    }

    void ChunkSync::OnChunkData(voxel::World& world, std::span<const ChunkData> data, uint32_t applied) {
        std::vector<Record*> installed;
        for (const ChunkData& d : data) (void)Install(world, d, applied, installed);
        CatchUp(world, installed, applied);
    }

    void ChunkSync::OnLoaded(voxel::World& world, std::span<const glm::ivec3> chunks, uint32_t applied) {
        std::vector<Record*> loaded;
        for (const glm::ivec3& c : chunks) {
            Record* r = Find(c);
            if (!r || r->state != ChunkSyncState::Based || r->ready || !world.IsChunkLoaded(c)) continue;
            loaded.push_back(r);
        }
        CatchUp(world, loaded, applied);
    }

    HashCheckOutcome ChunkSync::OnHashCheck(const voxel::World& world, const HashCheck& check, uint32_t applied) {
        HashCheckOutcome out;
        if (applied != check.asOfOpSeq) {
            ++m_Stats.checksOutOfOrder;
            return out;
        }
        out.compared = true;
        ++m_Stats.hashChecks;
        for (const HashCheckEntry& e : check.entries) {
            Record* r = Find(e.chunk);
            if (!r || !m_ServerWindow.Contains(e.chunk)) {
                ++out.outside;
                continue;
            }
            if (!Current(*r)) {
                ++out.notCurrent;
                // Still loading here (or its ack still to come): the server's planner counts the pick
                // as checked, so keep the entry and compare once the chunk is current (review of 7b).
                if (r->state == ChunkSyncState::NoBase || r->state == ChunkSyncState::Based) {
                    std::erase_if(m_Deferred, [&](const DeferredCheck& d) { return d.chunk == e.chunk; });
                    if (m_Deferred.size() >= kMaxDeferredChecks) {
                        m_Deferred.erase(m_Deferred.begin());
                        ++m_Stats.deferredDropped;
                    }
                    m_Deferred.push_back({e.chunk, e.hash, r->entered, e.pristine});
                    ++m_Stats.checkDeferred;
                }
                continue;
            }
            if (r->base > check.asOfOpSeq) {
                // Its data holds events the check does not (bulk ahead of the stream): not comparable.
                ++out.ahead;
                continue;
            }
            uint64_t hash = 0;
            if (!voxel::ChunkHashAt(world, e.chunk, hash)) {
                ++out.notCurrent;          // current means loaded: kept as a guard
                continue;
            }
            if (hash == e.hash) {
                ++out.equal;
                continue;
            }
            Mismatch(*r, e.chunk, e.pristine, false, out);
        }
        m_Stats.checkEqual += out.equal;
        m_Stats.checkAhead += out.ahead;
        m_Stats.checkNotCurrent += out.notCurrent;
        m_Stats.checkOutside += out.outside;
        return out;
    }

    void ChunkSync::Mismatch(Record& r, const glm::ivec3& chunk, bool pristine, bool deferred, HashCheckOutcome& out) {
        if (r.stuck) {
            ++m_Stats.stuckMismatches;     // logged once when it was refused
            return;
        }
        ++m_Stats.mismatches;
        m_Stats.deferredMismatches += deferred ? 1u : 0u;
        HashCheckOutcome::Mismatch m{chunk, pristine, !r.fromData, r.repaired, false, deferred};
        if (r.repaired) {
            ++m_Stats.repeatRepairs;
            // The plan's choice 17 (review of stage 7b): logged, never looped on. A repeat soon after a
            // repair (or the fourth repair) would hold the player over it every 2 s for the rest of the
            // session; the chunk stays current and the difference is counted (a bug to look at).
            if (m_Stats.hashChecks - r.repairedAtCheck < kRepeatRepairChecks || r.repairs >= kMaxRepairsPerChunk) {
                m.refused = true;
                r.stuck = true;
                ++m_Stats.repairsRefused;
                out.mismatches.push_back(m);
                return;
            }
        }
        // Different from the server's chunk: not trusted any more. Requested, so no event reaches it,
        // it is not walked on or hashed, and its ChunkData (7a's install and replay) makes it the
        // server's again.
        out.mismatches.push_back(m);
        SetState(r, ChunkSyncState::Requested);
        r.ready = false;
        r.repair = true;
        std::erase(m_Owed, chunk);
        m_Repairs.push_back(chunk);
    }

    HashCheckOutcome ChunkSync::CheckDeferred(const voxel::World& world, uint32_t applied) {
        HashCheckOutcome out;
        out.compared = true;
        std::vector<DeferredCheck> keep;
        for (const DeferredCheck& d : m_Deferred) {
            Record* r = Find(d.chunk);
            if (!r || r->entered != d.entered) {
                ++m_Stats.deferredDropped;          // left the window (or came back as a new record)
                continue;
            }
            if (!Current(*r) || r->base > applied) {
                // Still loading or its ack to come: wait. Data on its way (Awaiting, Requested,
                // Unsynced) is the server's anyway: drop.
                if (r->state == ChunkSyncState::NoBase || r->state == ChunkSyncState::Based) keep.push_back(d);
                else ++m_Stats.deferredDropped;
                continue;
            }
            uint64_t hash = 0;
            if (!voxel::ChunkHashAt(world, d.chunk, hash)) {
                keep.push_back(d);
                continue;
            }
            if (hash == d.hash) {
                ++out.equal;
                ++m_Stats.deferredEqual;
                continue;
            }
            Mismatch(*r, d.chunk, d.pristine, true, out);
        }
        m_Deferred = std::move(keep);
        return out;
    }

    std::vector<glm::ivec3> ChunkSync::TakeRepairs(size_t max) {
        std::vector<glm::ivec3> out;
        while (!m_Repairs.empty() && out.size() < max) {
            const glm::ivec3 c = m_Repairs.front();
            m_Repairs.erase(m_Repairs.begin());
            const Record* r = Find(c);
            if (r && r->state == ChunkSyncState::Requested && r->repair &&
                std::find(out.begin(), out.end(), c) == out.end())
                out.push_back(c);
        }
        m_Stats.requested += static_cast<uint32_t>(out.size());
        return out;
    }

    std::vector<glm::ivec3> ChunkSync::TakeRequests(uint32_t freeSlots, uint32_t reserve, const glm::ivec3& nearChunk, size_t max) {
        std::vector<glm::ivec3> out;
        while (!m_Owed.empty() && out.size() < max) {
            const glm::ivec3 c = m_Owed.back();
            m_Owed.pop_back();
            // Only a chunk still waiting for that request (one that left the window and came back is
            // a new record: its ack decides).
            const Record* r = Find(c);
            if (r && r->state == ChunkSyncState::Requested) out.push_back(c);
        }
        // The window's records are walked only when some are Unsynced (review of stage 7a: this runs
        // every Poll, and the game's window holds 101 376 records).
        // Slots that data on its way will take are not free (stage 7b M-b: a window step frees the
        // leaving slab's slots before the entering slab's data comes; asking for Unsynced chunks then
        // would take the slots that data needs and leave it Unsynced in turn - a request loop that a
        // terrain-mode client, whose every entering chunk is data, would run at every step).
        const uint32_t spare = freeSlots > m_Incoming ? freeSlots - m_Incoming : 0u;
        if (m_Unsynced > 0 && spare > reserve && out.size() < max) {
            std::vector<std::pair<int64_t, glm::ivec3>> unsynced;
            m_Stats.recordsVisited += m_Records.size();
            for (const Record& r : m_Records)
                if (r.assigned && r.state == ChunkSyncState::Unsynced) {
                    const glm::ivec3 d = r.coord - nearChunk;
                    unsynced.emplace_back(static_cast<int64_t>(d.x) * d.x + static_cast<int64_t>(d.y) * d.y + static_cast<int64_t>(d.z) * d.z, r.coord);
                }
            std::sort(unsynced.begin(), unsynced.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : std::tie(a.second.x, a.second.y, a.second.z) < std::tie(b.second.x, b.second.y, b.second.z);
            });
            const size_t take = std::min<size_t>({unsynced.size(), static_cast<size_t>(spare - reserve), max - out.size()});
            for (size_t i = 0; i < take; ++i) {
                SetState(*Find(unsynced[i].second), ChunkSyncState::Requested);
                out.push_back(unsynced[i].second);
            }
        }
        m_Stats.requested += static_cast<uint32_t>(out.size());
        return out;
    }

    ChunkSyncState ChunkSync::StateOf(const glm::ivec3& c) const {
        const Record* r = Find(c);
        return r ? r->state : ChunkSyncState::NoBase;
    }

    uint32_t ChunkSync::BaseOf(const glm::ivec3& c) const {
        const Record* r = Find(c);
        return r ? r->base : 0;
    }

    bool ChunkSync::IsCurrent(const glm::ivec3& c) const {
        const Record* r = Find(c);
        return r && Current(*r);
    }

    bool ChunkSync::AllCurrent(const glm::ivec3& lo, const glm::ivec3& hi) const {
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const glm::ivec3 c(x, y, z);
                    if (m_Window.Contains(c) && !IsCurrent(c)) return false;
                }
        return true;
    }

    bool ChunkSync::Synced() const {
        if (!m_Owed.empty() || !m_Repairs.empty() || m_Unsynced > 0) return false;
        for (const Record& r : m_Records)
            if (r.assigned && !Current(r)) return false;
        return true;
    }

    uint32_t ChunkSync::NotCurrent() const {
        uint32_t n = 0;
        for (const Record& r : m_Records) n += r.assigned && !Current(r) ? 1u : 0u;
        return n;
    }

    ChunkSyncCounts ChunkSync::Counts() const {
        ChunkSyncCounts n;
        for (const Record& r : m_Records) {
            if (!r.assigned) continue;
            if (Current(r)) ++n.current;
            else if (r.state == ChunkSyncState::NoBase) ++n.noBase;
            else if (r.state == ChunkSyncState::Awaiting) ++n.awaiting;
            else if (r.state == ChunkSyncState::Requested) ++n.requested;
            else if (r.state == ChunkSyncState::Unsynced) ++n.unsynced;
            else ++n.basedNotReady;
        }
        return n;
    }

    uint32_t ChunkSync::AppliedOps(const glm::ivec3& c) const {
        const Record* r = Find(c);
        return r ? r->appliedOps : 0;
    }

    bool ChunkSync::FromData(const glm::ivec3& c) const {
        const Record* r = Find(c);
        return r && r->fromData;
    }

    uint32_t ChunkSync::SkippedAtOrBelowBase(const glm::ivec3& c) const {
        const Record* r = Find(c);
        return r ? r->skippedOps : 0;
    }

    // --- HashCheckPlanner ----------------------------------------------------------------------

    bool HashCheckPlanner::LastChecked(const glm::ivec3& chunk, uint32_t& tick) const {
        const auto it = m_Checked.find(chunk);
        if (it == m_Checked.end()) return false;
        tick = it->second;
        return true;
    }

    std::vector<HashCheckPick> HashCheckPlanner::Plan(const HashCheckInput& in) {
        std::vector<HashCheckPick> out;
        if (in.box.Empty()) return out;
        // What was checked outside the box is forgotten (the map is bounded by the box).
        std::erase_if(m_Checked, [&](const auto& e) { return !in.box.Contains(e.first); });
        std::unordered_set<glm::ivec3, ChunkCoordHash> picked;
        const auto usable = [&](const glm::ivec3& c) {
            return in.box.Contains(c) && !picked.contains(c) && !(in.skip && in.skip(c));
        };
        const auto take = [&](const glm::ivec3& c, HashCheckKind kind) {
            picked.insert(c);
            out.push_back({c, kind});
        };
        const auto checkedKey = [&](const glm::ivec3& c) -> std::pair<int, uint32_t> {
            const auto it = m_Checked.find(c);
            return it == m_Checked.end() ? std::pair<int, uint32_t>{0, 0u} : std::pair<int, uint32_t>{1, it->second};
        };
        const double size = in.chunkMetres;
        const auto centre = [size](const glm::ivec3& c) { return (glm::dvec3(c) + glm::dvec3(0.5)) * size; };

        // 1. The chunks of the last operations, newest first.
        for (size_t i = in.recent.size(); i-- > 0 && out.size() < m_Settings.recent;) {
            const glm::ivec3 c = in.recent[i].second;
            if (usable(c)) take(c, HashCheckKind::Recent);
        }
        // 2. Modified chunks: least recently checked first, then nearest the feet, then (y, z, x).
        if (in.modified && m_Settings.modified > 0) {
            struct Candidate {
                std::pair<int, uint32_t> checked;
                double distance;
                glm::ivec3 c;
            };
            std::vector<Candidate> list;
            for (const auto& [c, seq] : *in.modified)
                if (usable(c)) list.push_back({checkedKey(c), glm::distance(centre(c), in.feet), c});
            const auto less = [](const Candidate& a, const Candidate& b) {
                if (a.checked != b.checked) return a.checked < b.checked;
                if (a.distance != b.distance) return a.distance < b.distance;
                return std::tie(a.c.y, a.c.z, a.c.x) < std::tie(b.c.y, b.c.z, b.c.x);
            };
            const size_t n = std::min<size_t>(list.size(), m_Settings.modified);
            std::partial_sort(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(n), list.end(), less);
            for (size_t i = 0; i < n; ++i) take(list[i].c, HashCheckKind::Modified);
        }
        // 3. Pristine chunks around the feet and along the way ahead (amendment 5).
        if (m_Settings.pristine > 0) {
            const glm::dvec3 v(in.velocity.x, 0.0, in.velocity.z);
            const glm::dvec3 a = in.feet, b = in.feet + v * static_cast<double>(m_Settings.lookAheadSeconds);
            const double reach = static_cast<double>(m_Settings.pristineMetres);
            const glm::dvec3 lo = glm::min(a, b) - glm::dvec3(reach), hi = glm::max(a, b) + glm::dvec3(reach);
            const glm::ivec3 clo = glm::ivec3(glm::floor(lo / size)), chi = glm::ivec3(glm::floor(hi / size));
            const glm::dvec3 ab = b - a;
            const double length2 = glm::dot(ab, ab);
            const double path = static_cast<double>(m_Settings.pathMetres);
            struct Candidate {
                int dense;                         // 0: the server holds it Dense or Packed
                std::pair<int, uint32_t> checked;
                double feet;                       // distance from the feet (centre)
                int64_t lane;                      // 0: on the path; else 1 + the box's distance to the way, whole chunks
                double arrival;                    // where along the way its box starts (metres from the feet)
                double side;                       // the box's distance to the way
                glm::ivec3 c;
            };
            std::vector<Candidate> near, ahead;
            for (int y = clo.y; y <= chi.y; ++y)
                for (int z = clo.z; z <= chi.z; ++z)
                    for (int x = clo.x; x <= chi.x; ++x) {
                        const glm::ivec3 c(x, y, z);
                        if (!usable(c) || (in.modified && in.modified->contains(c))) continue;
                        const glm::dvec3 p = centre(c), boxMin = glm::dvec3(c) * size, boxMax = boxMin + glm::dvec3(size);
                        const double t = length2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / length2, 0.0, 1.0) : 0.0;
                        const glm::dvec3 q = a + ab * t;  // the way's point nearest the centre
                        const double side = glm::distance(q, glm::clamp(q, boxMin, boxMax));
                        const double toFeet = glm::distance(p, a);
                        const double length = std::sqrt(length2);
                        // Where along the way the box starts and ends: its corners projected on it.
                        double arrival = 0.0, leaves = 0.0;
                        if (length2 > 0.0) {
                            const glm::dvec3 dir = ab / length;
                            arrival = std::numeric_limits<double>::max();
                            leaves = std::numeric_limits<double>::lowest();
                            for (int k = 0; k < 8; ++k) {
                                const glm::dvec3 corner((k & 1) ? boxMax.x : boxMin.x, (k & 2) ? boxMax.y : boxMin.y, (k & 4) ? boxMax.z : boxMin.z);
                                const double along = glm::dot(corner - a, dir);
                                arrival = std::min(arrival, along);
                                leaves = std::max(leaves, along);
                            }
                        }
                        Candidate k{in.dense && in.dense(c) ? 0 : 1,
                                    checkedKey(c),
                                    toFeet,
                                    side <= path ? 0 : 1 + static_cast<int64_t>(std::floor(side / size)),
                                    arrival,
                                    side,
                                    c};
                        if (toFeet <= reach) near.push_back(k);
                        // The way ahead: within reach of the way, and not wholly behind the feet (the
                        // feet's slots look after what the player stands in).
                        const bool behind = leaves < 0.0;
                        if (glm::distance(p, q) <= reach && !behind && length2 > 0.0) ahead.push_back(k);
                    }
            const auto lessNear = [](const Candidate& p, const Candidate& q) {
                if (p.dense != q.dense) return p.dense < q.dense;
                if (p.checked != q.checked) return p.checked < q.checked;
                if (p.feet != q.feet) return p.feet < q.feet;
                return std::tie(p.c.y, p.c.z, p.c.x) < std::tie(q.c.y, q.c.z, q.c.x);
            };
            const auto lessAhead = [](const Candidate& p, const Candidate& q) {
                if (p.dense != q.dense) return p.dense < q.dense;
                if (p.checked != q.checked) return p.checked < q.checked;
                if (p.lane != q.lane) return p.lane < q.lane;
                if (p.arrival != q.arrival) return p.arrival < q.arrival;
                if (p.side != q.side) return p.side < q.side;
                return std::tie(p.c.y, p.c.z, p.c.x) < std::tie(q.c.y, q.c.z, q.c.x);
            };
            std::sort(near.begin(), near.end(), lessNear);
            std::sort(ahead.begin(), ahead.end(), lessAhead);
            const uint32_t feetSlots = std::min(m_Settings.feetSlots, m_Settings.pristine);
            uint32_t taken = 0;
            for (const Candidate& k : near) {
                if (taken >= feetSlots) break;
                if (!usable(k.c)) continue;
                take(k.c, HashCheckKind::Pristine);
                ++taken;
            }
            for (const Candidate& k : ahead) {
                if (taken >= m_Settings.pristine) break;
                if (!usable(k.c)) continue;
                take(k.c, HashCheckKind::Pristine);
                ++taken;
            }
            // A player standing still: the way ahead is the feet; fill the rest from there.
            for (const Candidate& k : near) {
                if (taken >= m_Settings.pristine) break;
                if (!usable(k.c)) continue;
                take(k.c, HashCheckKind::Pristine);
                ++taken;
            }
        }
        for (const HashCheckPick& p : out) m_Checked[p.chunk] = in.tick;
        return out;
    }

    // --- BulkPacer -------------------------------------------------------------------------------

    void BulkPacer::Update(size_t inFlight, double rtt, double dt) {
        m_Time += std::max(0.0, dt);
        if (rtt > 0.0) {
            // The lowest RTT per period, the base the lowest of the kept periods (aged: review of
            // stage 7a).
            if (m_RttMinima.empty() || m_Time - m_RttMinima.back().first >= m_Settings.baseRttPeriod) {
                m_RttMinima.emplace_back(m_Time, rtt);
                while (m_RttMinima.size() > std::max<size_t>(1, m_Settings.baseRttWindows)) m_RttMinima.pop_front();
            } else {
                m_RttMinima.back().second = std::min(m_RttMinima.back().second, rtt);
            }
            m_BaseRtt = m_RttMinima.front().second;
            for (const auto& [start, lowest] : m_RttMinima) m_BaseRtt = std::min(m_BaseRtt, lowest);
        }
        if (m_LastInFlight > 0 && dt > 0.0) {
            const size_t drained = m_LastInFlight > inFlight ? m_LastInFlight - inFlight : 0;
            double sample = static_cast<double>(drained) / dt;
            // Everything went: the link may be faster than that.
            if (inFlight == 0) sample = std::max(sample, m_Rate);
            m_Rate = std::max(1024.0, m_Rate + (sample - m_Rate) / 8.0);
        }
        m_LastInFlight = inFlight;
        Recompute(rtt);
    }

    void BulkPacer::Recompute(double rtt) {
        const double base = std::max(0.0, m_BaseRtt);
        if (m_BaseRtt >= 0.0 && rtt - m_BaseRtt > m_Settings.targetDelay && m_Window > 0) {
            // The queue in front of the peer is longer than the target: back off, once per round trip
            // (a halving shows in the RTT only a round trip later).
            if (m_Time - m_LastHalving >= rtt) {
                m_Window = std::max(m_Settings.floorBytes, m_Window / 2);
                m_LastHalving = m_Time;
                ++m_Halvings;
            }
            return;
        }
        const double w = m_Rate * (base + m_Settings.targetDelay);
        m_Window = std::clamp(static_cast<size_t>(w), m_Settings.floorBytes, m_Settings.ceilingBytes);
    }
}
