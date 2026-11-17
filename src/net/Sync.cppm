module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <utility>
#include <vector>

export module vortex.net:sync;

import vortex.voxel;
import vortex.sim;
import :wire;
import :messages;

// docs/design/networking.md "Chunk sync" and "Stage 7a plan" (with its amendments): the pieces of
// chunk sync that are tested alone - the chunk box of a window, the client's op ring and per-chunk
// bases (ChunkSync), and the server's bulk pacer.
export namespace vortex::net {

    /// @brief A box of chunks [min, min + size).
    struct SyncBox {
        glm::ivec3 min{0};
        glm::ivec3 size{0};
        bool Contains(const glm::ivec3& c) const {
            return c.x >= min.x && c.y >= min.y && c.z >= min.z && c.x < min.x + size.x && c.y < min.y + size.y && c.z < min.z + size.z;
        }
        bool Empty() const { return size.x <= 0 || size.y <= 0 || size.z <= 0; }
        glm::ivec3 Max() const { return min + size; }   ///< exclusive
        friend bool operator==(const SyncBox&, const SyncBox&) = default;
    };
    /// @brief The overlap of two boxes (empty when they do not overlap).
    SyncBox Intersect(const SyncBox& a, const SyncBox& b);
    /// @brief The server's window of a region: play bounds + sim::kServerWindowMarginChunks, the
    /// region's rows. No operation ever changes a chunk outside it (the server would fail).
    SyncBox RegionSyncBox(const sim::RegionDesc& region);

    /// @brief Hash for chunk coordinates as map keys.
    struct ChunkCoordHash {
        size_t operator()(const glm::ivec3& c) const noexcept {
            uint64_t h = static_cast<uint32_t>(c.x) * 0x9E3779B97F4A7C15ull;
            h ^= (static_cast<uint64_t>(static_cast<uint32_t>(c.y)) + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
            h ^= (static_cast<uint64_t>(static_cast<uint32_t>(c.z)) + 0x165667B19E3779F9ull) * 0x94D049BB133111EBull;
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };

    /**
     * @brief The client's ring of the terrain operations it applied (kinds 1-6), each kept as its
     * wire bytes (masks as runs; a Detach with netId 0 and no velocity) with its chunk range: at most
     * `maxOps` operations or `maxBytes` of those bytes, whichever is less. It covers a base b when
     * every operation after b up to the newest is still in it (b >= Floor()).
     */
    class OpRing {
    public:
        OpRing(size_t maxOps = 1024, size_t maxBytes = size_t{1} << 20) : m_MaxOps(maxOps), m_MaxBytes(maxBytes) {}
        /// @brief The stream starts after `seq` (the join point): nothing older is covered.
        void Reset(uint32_t seq);
        /// @brief Operation `seq` (the next of the stream).
        void Push(uint32_t seq, const sim::WorldOp& op);
        bool Covers(uint32_t base) const { return base >= m_Floor; }
        uint32_t Floor() const { return m_Floor; }
        size_t Count() const { return m_Ops.size(); }
        size_t Bytes() const { return m_Bytes; }
        /// @brief Operations decoded from their wire bytes so far (tests).
        uint64_t Decodes() const { return m_Decodes; }
        /// @brief The kept operations after `base` whose chunk range holds `chunk`, decoded, in order.
        void ForEachAfter(uint32_t base, const glm::ivec3& chunk, const std::function<void(uint32_t, const sim::WorldOp&)>& fn) const;
        /// @brief The kept operations after `base`, in order: `wants(seq, lo, hi)` (their chunk range)
        /// says whether one is needed; each wanted one is decoded once and handed to `fn(seq, lo, hi,
        /// op)` (a batch of chunks catching up walks the ring once: recheck of stage 7a, 3).
        void Walk(uint32_t base, const std::function<bool(uint32_t, const glm::ivec3&, const glm::ivec3&)>& wants,
                  const std::function<void(uint32_t, const glm::ivec3&, const glm::ivec3&, const sim::WorldOp&)>& fn) const;

    private:
        struct Entry {
            uint32_t seq = 0;
            glm::ivec3 lo{0}, hi{-1};
            std::vector<uint8_t> bytes;
        };
        size_t m_MaxOps, m_MaxBytes;
        std::deque<Entry> m_Ops;
        size_t m_Bytes = 0;
        uint32_t m_Floor = 0;
        mutable uint64_t m_Decodes = 0;
    };

    /// @brief A client chunk's sync state (networking.md "Chunk sync"; the plan's table).
    enum class ChunkSyncState : uint8_t {
        NoBase,      ///< entered the window, no ack yet: accepts no events
        Awaiting,    ///< the ack said "modified": its ChunkData is coming
        Based,       ///< has a base (the ack's asOf for a pristine chunk, a ChunkData's asOf)
        Requested,   ///< a ChunkRequest is out (the ring no longer covered its base, or it was unsynced)
        Unsynced,    ///< no store slot: its content is stale until a slot frees and it is requested
    };

    struct ChunkSyncStats {
        uint64_t opsFast = 0;             ///< operations applied whole (every chunk they touch current)
        uint64_t opsClipped = 0;          ///< ... applied chunk by chunk (some chunk they touch not current)
        uint64_t opsIncomplete = 0;       ///< operations that skipped a chunk they touch
        uint64_t chunkSkips = 0;          ///< (operation, touched chunk) pairs skipped: no base, base ahead, unsynced
        uint64_t replays = 0;             ///< operations replayed from the ring into a chunk
        uint32_t chunksInstalled = 0;     ///< ChunkData installed
        uint32_t staleChunkData = 0;      ///< ChunkData dropped: outside the window, or older than the chunk's base
        uint32_t unsynced = 0;            ///< chunks that became Unsynced (store full)
        uint32_t notCovered = 0;          ///< chunks the ring no longer covered: requested
        uint32_t requested = 0;           ///< coordinates sent in ChunkRequests
        uint32_t acks = 0;
        uint32_t ackedModified = 0;       ///< chunks the acks marked modified
        /// Records looked at by window steps, acks and the Unsynced scan (review of stage 7a: a step
        /// costs its entering slab, a Poll with nothing Unsynced costs nothing).
        uint64_t recordsVisited = 0;
        /// Ring operations decoded for replays (recheck of stage 7a, 3: once per batch of chunks
        /// catching up, not once per chunk).
        uint64_t opsDecoded = 0;
    };

    /// @brief How many chunks of the window are in each state (what Synced() waits for).
    struct ChunkSyncCounts {
        uint32_t current = 0;
        uint32_t noBase = 0, awaiting = 0, requested = 0, unsynced = 0;
        uint32_t basedNotReady = 0;       ///< Based, not loaded (or not caught up) yet
    };

    /**
     * @brief The client's per-chunk bases (networking.md "Chunk sync"): one record per cell of its
     * window, indexed like the world window (column-major, toroidal). The one rule: an operation is
     * applied to a chunk only if the chunk is current (Based and caught up) and the operation's opSeq
     * is above the chunk's base, live or replayed from the ring.
     */
    class ChunkSync {
    public:
        explicit ChunkSync(size_t ringOps = 1024, size_t ringBytes = size_t{1} << 20) : m_Ring(ringOps, ringBytes) {}

        /// @brief The join point (the stream starts after `joinOpSeq`) and the server's window
        /// (chunks outside it are never edited: current as soon as they load).
        void Start(uint32_t joinOpSeq, const SyncBox& serverWindow);
        /// @brief The window of WindowUpdate `seq`: cells whose chunk changed (all of them with
        /// `reset`) become NoBase, entered with `seq`.
        void BeginWindow(const SyncBox& window, uint32_t seq, bool reset);
        const SyncBox& Window() const { return m_Window; }
        /// @brief WindowAck: the NoBase records entered with its seq become Awaiting (bit set) or
        /// Based(asOf). False for an ack of a seq this client did not send (or sent with another size).
        bool OnAck(voxel::World& world, const WindowAck& ack, uint32_t applied);

        /// What happened to one operation of the stream.
        struct OpOutcome {
            sim::OpResult result;
            bool complete = true;         ///< every chunk the operation touches was current with a base below it
        };
        /// @brief Operation `seq` of the stream (applied = seq - 1 before): kept in the ring, applied
        /// to the chunks that pass the rule (one ApplyOp when all of them do).
        OpOutcome Apply(voxel::World& world, uint32_t seq, const sim::WorldOp& op, std::vector<voxel::VoxelEdit>* edits);

        enum class DataOutcome : uint8_t { Installed, Stale, StoreFull };
        /// @brief A ChunkData: installed (World::SetChunkEncoded) and caught up with the ring, unless
        /// it is outside the window or not newer than the chunk's base.
        DataOutcome OnChunkData(voxel::World& world, const ChunkData& data, uint32_t applied);
        /// @brief Several ChunkData in arrival order (one Poll's): each installed as above, then the
        /// installed chunks catch up together, one walk of the ring (recheck of stage 7a, 3).
        void OnChunkData(voxel::World& world, std::span<const ChunkData> data, uint32_t applied);
        /// @brief World::TakeLoadedChunks: a Based chunk that loaded catches up with the ring.
        void OnLoaded(voxel::World& world, std::span<const glm::ivec3> chunks, uint32_t applied);
        /// @brief Coordinates to put in a ChunkRequest now (at most `max`): chunks the ring no longer
        /// covered, and Unsynced chunks nearest `nearChunk` while the store has more than `reserve`
        /// free slots (as many as are free beyond it). They become Requested.
        std::vector<glm::ivec3> TakeRequests(uint32_t freeSlots, uint32_t reserve, const glm::ivec3& nearChunk, size_t max);

        ChunkSyncState StateOf(const glm::ivec3& chunk) const;
        uint32_t BaseOf(const glm::ivec3& chunk) const;
        /// @brief Based and caught up: events apply to it, a player may stand on it, the hash counts it.
        bool IsCurrent(const glm::ivec3& chunk) const;
        /// @brief Every chunk of [minChunk, maxChunk] (inclusive) inside the window is current.
        bool AllCurrent(const glm::ivec3& minChunk, const glm::ivec3& maxChunk) const;
        /// @brief Chunks of the window that are not current.
        uint32_t NotCurrent() const;
        ChunkSyncCounts Counts() const;
        /// @brief Every chunk of the window current and no request owed.
        bool Synced() const;
        /// @brief Per chunk: operations applied to it, and operations skipped because its base was at
        /// or above their opSeq (the base rule's two sides; tests).
        uint32_t AppliedOps(const glm::ivec3& chunk) const;
        uint32_t SkippedAtOrBelowBase(const glm::ivec3& chunk) const;
        /// @brief The chunk's content came from a ChunkData since it entered the window (tests: a
        /// pristine chunk is never sent).
        bool FromData(const glm::ivec3& chunk) const;
        const OpRing& Ring() const { return m_Ring; }
        const ChunkSyncStats& Stats() const { return m_Stats; }

    private:
        struct Record {
            glm::ivec3 coord{0};
            bool assigned = false;
            ChunkSyncState state = ChunkSyncState::NoBase;
            bool ready = false;           ///< loaded and caught up to the stream
            bool neverEdited = false;     ///< outside the server's window
            bool fromData = false;        ///< installed from a ChunkData
            uint32_t base = 0;
            uint32_t entered = 0;         ///< the WindowUpdate seq it entered with
            uint32_t appliedOps = 0, skippedOps = 0;
        };
        struct SentWindow {
            uint32_t seq = 0;
            SyncBox box;
            /// The window before this one; `all`: every cell of `box` entered with it (the first
            /// window, a reset, a new size). Else only the cells of `box` outside `prev` did.
            SyncBox prev;
            bool all = true;
        };
        Record* Find(const glm::ivec3& c);
        const Record* Find(const glm::ivec3& c) const;
        bool Current(const Record& r) const { return r.state == ChunkSyncState::Based && r.ready; }
        /// Every state change goes through here (the count of Unsynced records: TakeRequests looks
        /// for them only when there are any; review of stage 7a).
        void SetState(Record& r, ChunkSyncState state);
        /// The cells of `box` that entered with a window: all of them, or those outside `prev`.
        template <typename Fn>
        void ForEachEntering(const SyncBox& box, const SyncBox& prev, bool all, Fn&& fn) const;
        /// Based records whose chunks are loaded: each replays (base, applied] clipped to it, or
        /// becomes Requested when the ring no longer covers its base. One walk of the ring for all of
        /// them, each operation decoded once; per chunk the operations keep their order (recheck of
        /// stage 7a, 3). Sorts `records` and drops repeats.
        void CatchUp(voxel::World& world, std::vector<Record*>& records, uint32_t applied);
        /// Installs one ChunkData; the record to catch up goes to `installed`.
        DataOutcome Install(voxel::World& world, const ChunkData& data, std::vector<Record*>& installed);

        OpRing m_Ring;
        SyncBox m_Window, m_ServerWindow;
        std::vector<Record> m_Records;
        std::deque<SentWindow> m_Sent;
        std::vector<glm::ivec3> m_Owed;   ///< not covered: to request
        uint32_t m_Unsynced = 0;          ///< records in the Unsynced state
        uint32_t m_Applied = 0;
        ChunkSyncStats m_Stats;
    };

    /// @brief The server's bulk pacing per peer (the plan's amendments 1-3).
    struct BulkPacerSettings {
        double targetDelay = 2.0 / 60.0;  ///< queue of bulk allowed in front of live events, seconds
        size_t floorBytes = 1200;         ///< the window never goes below one datagram
        size_t ceilingBytes = 32768;      ///< networking.md "Transport": at most 32 kB of bulk in the transport
        double startRate = 32768.0;       ///< bytes per second before anything was measured
        /// The base RTT is the lowest RTT of the last `baseRttWindows` periods of `baseRttPeriod`
        /// seconds (review of stage 7a, LEDBAT's ageing): a lasting rise of the path's RTT becomes the
        /// new base within about two minutes instead of pinning the window to its floor for good.
        double baseRttPeriod = 10.0;
        size_t baseRttWindows = 12;
    };

    /**
     * @brief Delay-based bulk window: window = clamp(drain rate x (base RTT + target), floor,
     * ceiling), with the base RTT the lowest of the last minutes (aged per period); while the RTT is
     * more than `targetDelay` above the base, the window halves instead, at most once per round trip
     * (the queue is its own: a window that followed the inflated RTT would grow with it). A bulk
     * message is handed over while the bytes in flight are under it.
     */
    class BulkPacer {
    public:
        explicit BulkPacer(BulkPacerSettings settings = {}) : m_Settings(settings), m_Rate(settings.startRate) { Recompute(0.0); }
        /// @brief Once per tick before sending: the peer's reliable bytes in flight, its RTT.
        void Update(size_t inFlight, double rtt, double dt);
        bool MayHandOver(size_t inFlight) const { return inFlight < m_Window; }
        /// @brief Reliable bytes handed over to the peer since the last Update (bulk, and the live
        /// events: the drain rate is what left, not what left minus what was added).
        void Sent(size_t bytes) { m_LastInFlight += bytes; }
        size_t Window() const { return m_Window; }
        double Rate() const { return m_Rate; }
        double BaseRtt() const { return m_BaseRtt; }
        uint32_t Halvings() const { return m_Halvings; }

    private:
        void Recompute(double rtt);
        BulkPacerSettings m_Settings;
        double m_Rate;
        double m_BaseRtt = -1.0;
        size_t m_Window = 0;
        size_t m_LastInFlight = 0;
        double m_Time = 0.0;
        double m_LastHalving = -1.0e9;
        uint32_t m_Halvings = 0;
        std::deque<std::pair<double, double>> m_RttMinima;   ///< (period start, lowest RTT in it), oldest first
    };
}
