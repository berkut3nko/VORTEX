module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <tuple>
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
    }

    template <typename Fn>
    void ChunkSync::ForEachEntering(const SyncBox& box, const SyncBox& prev, bool all, Fn&& fn) const {
        if (all || prev.min.y != box.min.y || prev.size.y != box.size.y) {
            ForEachChunk(box.min, box.Max() - glm::ivec3(1), fn);
            return;
        }
        // Rows are fixed: whole columns enter (one or two slabs for a step, not the whole box).
        for (int z = box.min.z; z < box.Max().z; ++z)
            for (int x = box.min.x; x < box.Max().x; ++x) {
                if (x >= prev.min.x && x < prev.Max().x && z >= prev.min.z && z < prev.Max().z) continue;
                for (int y = box.min.y; y < box.Max().y; ++y) fn(glm::ivec3(x, y, z));
            }
    }

    void ChunkSync::Start(uint32_t joinOpSeq, const SyncBox& serverWindow) {
        m_Ring.Reset(joinOpSeq);
        m_Applied = joinOpSeq;
        m_ServerWindow = serverWindow;
    }

    void ChunkSync::BeginWindow(const SyncBox& window, uint32_t seq, bool reset) {
        bool all = reset;
        if (window.size != m_Window.size || m_Records.empty()) {
            m_Records.assign(static_cast<size_t>(window.size.x) * static_cast<size_t>(window.size.y) * static_cast<size_t>(window.size.z), Record{});
            m_Unsynced = 0;
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
            r = Record{};
            r.coord = c;
            r.assigned = true;
            r.entered = seq;
            r.neverEdited = !m_ServerWindow.Contains(c);
        });
        // A chunk owed a request that left the window is not owed any more; after a reset every
        // chunk waits for this update's ack (which marks the modified ones), none is owed.
        if (reset) m_Owed.clear();
        else std::erase_if(m_Owed, [this](const glm::ivec3& c) { return Find(c) == nullptr; });
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

    ChunkSync::OpOutcome ChunkSync::Apply(voxel::World& world, uint32_t seq, const sim::WorldOp& op, std::vector<voxel::VoxelEdit>* edits) {
        OpOutcome out;
        m_Ring.Push(seq, op);
        m_Applied = seq;
        glm::ivec3 lo, hi;
        if (!sim::OpChunkRange(op, lo, hi)) {
            out.result = sim::ApplyOp(world, op, edits);
            return out;
        }
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

    ChunkSync::DataOutcome ChunkSync::Install(voxel::World& world, const ChunkData& data, std::vector<Record*>& installed) {
        Record* r = Find(data.chunk);
        if (!r || r->neverEdited || (r->state == ChunkSyncState::Based && r->base >= data.asOfOpSeq)) {
            ++m_Stats.staleChunkData;
            return DataOutcome::Stale;
        }
        const voxel::SetChunkResult res = world.SetChunkEncoded(data.chunk, data.rle);
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
        std::erase(m_Owed, data.chunk);
        r->fromData = true;
        SetState(*r, ChunkSyncState::Based);
        r->base = data.asOfOpSeq;
        r->ready = false;
        installed.push_back(r);
        return DataOutcome::Installed;
    }

    ChunkSync::DataOutcome ChunkSync::OnChunkData(voxel::World& world, const ChunkData& data, uint32_t applied) {
        std::vector<Record*> installed;
        const DataOutcome out = Install(world, data, installed);
        CatchUp(world, installed, applied);
        return out;
    }

    void ChunkSync::OnChunkData(voxel::World& world, std::span<const ChunkData> data, uint32_t applied) {
        std::vector<Record*> installed;
        for (const ChunkData& d : data) (void)Install(world, d, installed);
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
        if (m_Unsynced > 0 && freeSlots > reserve && out.size() < max) {
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
            const size_t take = std::min<size_t>({unsynced.size(), static_cast<size_t>(freeSlots - reserve), max - out.size()});
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
        if (!m_Owed.empty() || m_Unsynced > 0) return false;
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
