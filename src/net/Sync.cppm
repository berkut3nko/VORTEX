module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <unordered_map>
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

    /// @brief Every cell of window `box` enters with its update: the first window, a reset, or a new
    /// size (stage 7a's rule, the same on both sides).
    inline bool WindowEntersWhole(bool first, bool reset, const SyncBox& prev, const SyncBox& box) {
        return first || reset || prev.size != box.size;
    }

    /**
     * @brief The cells that entered with window `box` after `prev`: every cell of `box` when `all`
     * (WindowEntersWhole) or when the rows differ, in (y, z, x) order; else the whole columns outside
     * `prev` (the rows are fixed), in (z, x, y) order. Client (ChunkSync) and server (the TerrainAck)
     * walk the same cells in the same order: the order is wire format since the review of stage 7b
     * (protocol 10: a TerrainAck codes these cells only). A step of the game's window visits its
     * entering slab (4224 cells for two chunks), not the box (101 376).
     */
    template <typename Fn>
    void ForEachEnteringChunk(const SyncBox& box, const SyncBox& prev, bool all, Fn&& fn) {
        if (all || prev.min.y != box.min.y || prev.size.y != box.size.y) {
            for (int y = box.min.y; y < box.Max().y; ++y)
                for (int z = box.min.z; z < box.Max().z; ++z)
                    for (int x = box.min.x; x < box.Max().x; ++x) fn(glm::ivec3(x, y, z));
            return;
        }
        for (int z = box.min.z; z < box.Max().z; ++z)
            for (int x = box.min.x; x < box.Max().x; ++x) {
                if (x >= prev.min.x && x < prev.Max().x && z >= prev.min.z && z < prev.Max().z) continue;
                for (int y = box.min.y; y < box.Max().y; ++y) fn(glm::ivec3(x, y, z));
            }
    }
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
        /// @brief Every operation after `base` that touches `chunk` is still kept (stage 8): true when the
        /// ring covers `base`, or when no operation dropped from it after `base` had a chunk range holding
        /// `chunk` (the soak: on the arena under a drill of 10 casts a second the ring wrapped in about 17 s
        /// and every pristine chunk still loading was asked for whole - 21 000 requests in a minute).
        bool CoversChunk(uint32_t base, const glm::ivec3& chunk) const;
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
        /// @brief Some kept operation in (after, upTo] has a chunk range that holds `chunk` or one of
        /// its neighbours (stage 7b: an edit there may have revealed the chunk on the server), from the
        /// ranges alone (nothing decoded). True when the ring has no operation up to `upTo` yet
        /// (unknown: the safe answer); when it no longer covers `after`, the dropped operations count by
        /// the chunks their ranges held (stage 8; one too large to list makes everything older unknown).
        bool TouchesNear(uint32_t after, uint32_t upTo, const glm::ivec3& chunk, uint32_t newest) const;

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
        /// The newest dropped operation touching each chunk (its chunk range), and the newest dropped one
        /// whose range was unknown or too large to list (stage 8: CoversChunk).
        std::unordered_map<glm::ivec3, uint32_t, ChunkCoordHash> m_DroppedTouch;
        uint32_t m_DroppedUnknown = 0;
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
        // Server terrain mode (stage 7b): TerrainAcks and what they said of the entering chunks.
        uint32_t terrainAcks = 0;
        uint32_t uniformBased = 0;        ///< uniform chunks the cell already held: based, no install
        uint32_t uniformInstalled = 0;    ///< uniform chunks installed from the ack
        uint32_t awaitingData = 0;        ///< chunks the acks said data follows for (codes 1 and 3)
        uint32_t awaitingHidden = 0;      ///< ... of them held packed by the server (code 3)
        uint32_t installedHidden = 0;     ///< terrain mode: ChunkData installed packed (InstallMode::HiddenContent in a world that packs)
        uint32_t installedShown = 0;      ///< ... installed resident (shown; or hidden in a world that packs nothing)
        // HashCheck (stage 7b): checks processed, their entries by outcome, repairs.
        uint32_t hashChecks = 0;          ///< HashChecks compared (processed at their stream position)
        uint32_t checksOutOfOrder = 0;    ///< HashChecks not at applied == asOfOpSeq: ignored (expected 0)
        uint64_t checkEqual = 0;          ///< entries equal
        uint64_t checkAhead = 0;          ///< ... skipped: current, but its base is above the check's opSeq
        uint64_t checkNotCurrent = 0;     ///< ... skipped: not current (no base, awaiting, requested, unsynced, loading)
        uint64_t checkOutside = 0;        ///< ... skipped: outside this window or the server's
        uint32_t mismatches = 0;          ///< entries that differed: the chunk became Requested (reason Repair)
        uint32_t repairsInstalled = 0;    ///< ChunkData installed for a chunk whose last request was a repair
        uint32_t repeatRepairs = 0;       ///< mismatches of a chunk already repaired while in the window (a bug, logged)
        /// Review of stage 7b: repeats not asked for again - within kRepeatRepairChecks checks of the last
        /// repair, or after kMaxRepairsPerChunk repairs - the chunk stays current (the player is not
        /// held) and is logged once; later mismatches of it are only counted (`stuckMismatches`).
        uint32_t repairsRefused = 0;
        uint32_t stuckMismatches = 0;
        /// Review of stage 7b: entries skipped as not current because the chunk was still loading here
        /// (NoBase, or Based and not loaded): kept with the server's hash and compared once the chunk is
        /// current, as long as no operation of the stream touched it since (CheckDeferred).
        uint32_t checkDeferred = 0;
        uint32_t deferredEqual = 0;       ///< ... compared later and equal
        uint32_t deferredMismatches = 0;  ///< ... compared later and different (also in `mismatches`)
        uint32_t deferredDropped = 0;     ///< ... dropped: touched by an operation, left the window, data on its way, or over the bound
    };

    /// @brief What one HashCheck found (ChunkSync::OnHashCheck).
    struct HashCheckOutcome {
        bool compared = false;            ///< false: not at its stream position (applied != asOfOpSeq), nothing looked at
        uint32_t equal = 0, ahead = 0, notCurrent = 0, outside = 0;
        struct Mismatch {
            glm::ivec3 chunk{0};
            bool pristine = false;        ///< the server's flag: never modified there
            bool generated = false;       ///< this client's generator made the chunk (not from a ChunkData)
            bool repeat = false;          ///< repaired before while in the window
            bool refused = false;         ///< a repeat not asked for again (stays current; review of stage 7b)
            bool deferred = false;        ///< found by CheckDeferred (an entry kept while the chunk loaded)
        };
        std::vector<Mismatch> mismatches;
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
        /**
         * @brief A HashCheck in stream order (stage 7b): compared only when `applied` equals its
         * asOfOpSeq C, and then only for chunks that are current with a base at or below C - such a
         * chunk holds its base content plus every operation in (base, C] that touches it, which is
         * the server's chunk after operation C. Chunks whose data is ahead (base > C), that are not
         * current, or outside this window or the server's are skipped. A chunk that differs becomes
         * Requested and is owed a ChunkRequest with reason Repair (TakeRepairs): from there it is
         * 7a's path (no event reaches it, its ChunkData installs and replays, or is requested again).
         */
        HashCheckOutcome OnHashCheck(const voxel::World& world, const HashCheck& check, uint32_t applied);
        /**
         * @brief Entries a HashCheck skipped because their chunk was still loading here (NoBase, or Based
         * and not loaded), compared now (review of stage 7b: the server's planner counts a pick as
         * checked whether or not this client could compare it, so such a chunk may not be picked again
         * before the player reaches it). An entry is compared once its chunk is current with a base at
         * or below `applied`; every stream operation whose chunk range holds the chunk drops it (Apply),
         * so the server's hash at the check's opSeq is still its hash now. A mismatch is OnHashCheck's.
         */
        HashCheckOutcome CheckDeferred(const voxel::World& world, uint32_t applied);
        bool HasDeferredChecks() const { return !m_Deferred.empty(); }
        /// Most entries kept by CheckDeferred's rule (the oldest goes first).
        static constexpr size_t kMaxDeferredChecks = 256;
        /// A chunk that mismatches again within this many HashChecks of its last repair, or that was
        /// repaired kMaxRepairsPerChunk times since it entered the window, is not asked for again.
        static constexpr uint32_t kRepeatRepairChecks = 5;
        static constexpr uint8_t kMaxRepairsPerChunk = 3;
        /**
         * @brief Server terrain mode (stage 7b): the answer to a terrain-mode WindowUpdate, in stream
         * order (applied == its asOf A). For each record that entered with that update and has no
         * base: a uniform chunk (code 2) is based at A - installed first unless the cell already holds
         * exactly that content; data (codes 1, 3) makes it Awaiting (code 3: the server holds it
         * packed, so its ChunkData is installed packed only if no operation over it or a face
         * neighbour came between the ack and the stream's position - max(asOf, applied) - and no face
         * neighbour is an edit here; a later operation this world skips next to it shows it again,
         * World::ShowServerHiddenNeighbours); outside the server's window (code 0) it is based at A as in 7a. Every
         * chunk it bases counts as from data (the server vouched for it). False (malformed): no such
         * update sent, another size, or code 0 for an entering chunk inside the server's window.
         */
        bool OnTerrainAck(voxel::World& world, const TerrainAck& ack, uint32_t applied);
        /// @brief Terrain mode: ChunkData installs as content by the server's packing (InstallMode::
        /// Content / HiddenContent), never as 7a's edits. Sticky for the session (the client's choice 12).
        void SetTerrainMode(bool on) { m_TerrainMode = on; }
        bool TerrainMode() const { return m_TerrainMode; }
        /// @brief Coordinates owed a ChunkRequest with reason Repair (at most `max`), of chunks still
        /// waiting for that repair.
        std::vector<glm::ivec3> TakeRepairs(size_t max);
        /// @brief Coordinates to put in a ChunkRequest now (at most `max`): chunks the ring no longer
        /// covered, and Unsynced chunks nearest `nearChunk` while the store has more than `reserve`
        /// free slots beyond those the content still to come may take (NoBase, Awaiting and Requested
        /// records; stage 7b M-b), as many as are spare beyond it. They become Requested.
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
            bool repair = false;          ///< its last request is a repair (stage 7b)
            bool repaired = false;        ///< a repair was installed since it entered the window
            bool hidden = false;          ///< terrain mode: Awaiting data the server holds packed (code 3)
            bool incoming = false;        ///< counted in m_Incoming (its content may take a store slot when it comes)
            bool stuck = false;           ///< a repeat repair was refused: mismatches only counted (review of stage 7b)
            uint8_t repairs = 0;          ///< repairs installed since it entered the window
            uint32_t repairedAtCheck = 0; ///< m_Stats.hashChecks when its last repair was installed
            uint32_t ackedAt = 0;         ///< terrain mode: the asOf of the TerrainAck that made it Awaiting
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
        /// Keeps m_Incoming: a record whose content is still to come and may take a slot (NoBase,
        /// Requested, Awaiting unless it is hidden data in a world that packs it). After every change
        /// of state or `hidden`.
        void Recount(Record& r);
        /// A record whose content differs from the server's (OnHashCheck, CheckDeferred).
        void Mismatch(Record& r, const glm::ivec3& chunk, bool pristine, bool deferred, HashCheckOutcome& out);
        /// The cells of `box` that entered with a window: all of them, or those outside `prev`.
        template <typename Fn>
        void ForEachEntering(const SyncBox& box, const SyncBox& prev, bool all, Fn&& fn) const;
        /// Based records whose chunks are loaded: each replays (base, applied] clipped to it, or
        /// becomes Requested when the ring no longer covers its base. One walk of the ring for all of
        /// them, each operation decoded once; per chunk the operations keep their order (recheck of
        /// stage 7a, 3). Sorts `records` and drops repeats.
        void CatchUp(voxel::World& world, std::vector<Record*>& records, uint32_t applied);
        /// Installs one ChunkData; the record to catch up goes to `installed`.
        DataOutcome Install(voxel::World& world, const ChunkData& data, uint32_t applied, std::vector<Record*>& installed);

        OpRing m_Ring;
        SyncBox m_Window, m_ServerWindow;
        std::vector<Record> m_Records;
        std::deque<SentWindow> m_Sent;
        std::vector<glm::ivec3> m_Owed;   ///< not covered: to request
        std::vector<glm::ivec3> m_Repairs;   ///< HashCheck mismatches: to request with reason Repair
        uint32_t m_Unsynced = 0;          ///< records in the Unsynced state
        /// Records whose content is still to come and may take a store slot when it comes (NoBase: its
        /// ack, then data or a load; Awaiting, Requested: data; not hidden data a packing world installs
        /// packed - review of stage 7b), so TakeRequests does not count those slots as free (stage 7b M-b).
        uint32_t m_Incoming = 0;
        bool m_PacksHidden = false;       ///< the world packs HiddenContent installs (seen at each TerrainAck)
        struct DeferredCheck {
            glm::ivec3 chunk{0};
            uint64_t hash = 0;
            uint32_t entered = 0;         ///< the record's WindowUpdate seq when it was kept
            bool pristine = false;
        };
        std::vector<DeferredCheck> m_Deferred;
        uint32_t m_Applied = 0;
        bool m_TerrainMode = false;
        ChunkSyncStats m_Stats;
    };

    // --- HashCheck planning (stage 7b, server side) -------------------------------------------------

    struct HashCheckSettings {
        uint32_t recent = 4;              ///< chunks of the last operations, newest first
        uint32_t modified = 8;            ///< modified chunks, least recently checked first
        uint32_t pristine = 12;           ///< pristine chunks near the player and along its way (amendment 5, as built)
        uint32_t feetSlots = 2;           ///< ... of them for the chunks around the feet
        float pristineMetres = 12.0f;     ///< pristine candidates within this of the feet or the way ahead
        float lookAheadSeconds = 4.0f;    ///< the way ahead: feet + horizontal velocity x this
        /// A chunk whose box lies within this of the way (sideways) is on the path: the player's reach
        /// box (bounds + 1 m, sim::PlayerReach) will touch it. On-path chunks go first, by arrival.
        float pathMetres = 1.5f;
    };

    enum class HashCheckKind : uint8_t { Recent, Modified, Pristine };
    struct HashCheckPick {
        glm::ivec3 chunk{0};
        HashCheckKind kind = HashCheckKind::Recent;
    };

    using ModifiedMap = std::unordered_map<glm::ivec3, uint32_t, ChunkCoordHash>;

    /// @brief What the planner reads about one peer at one check.
    struct HashCheckInput {
        SyncBox box;                      ///< the peer's window inside the server's
        const ModifiedMap* modified = nullptr;   ///< the server's modified map (chunk -> last opSeq)
        /// The chunks changed by the server's last operations, oldest first (a ring, `(opSeq, chunk)`).
        std::span<const std::pair<uint32_t, glm::ivec3>> recent;
        std::function<bool(const glm::ivec3&)> dense;   ///< the server holds the chunk Dense or Packed
        std::function<bool(const glm::ivec3&)> skip;    ///< queued for the peer (its data is on the way)
        glm::dvec3 feet{0.0};
        glm::dvec3 velocity{0.0};
        double chunkMetres = 4.0;
        uint32_t tick = 0;
    };

    /**
     * @brief Picks the chunks of one peer's HashCheck (networking.md "Stage 7b plan", "HashCheck:
     * server", amendments 4 and 5): the chunks of the last operations (newest first), then modified
     * chunks least recently checked first (ties nearest the feet), then pristine chunks - one around
     * the feet, the others within `pristineMetres` of the way ahead (the segment from the feet to feet
     * + velocity x `lookAheadSeconds`) and not behind the feet, server-dense first, never checked
     * first, then the chunks on the path (their box within `pathMetres` of the way) by arrival, then
     * the others by distance to the way in whole chunks. Only chunks of the peer's box, none skipped, none
     * twice. One planner per peer: it keeps when it last checked each chunk (entries outside the box
     * are dropped at every check).
     */
    class HashCheckPlanner {
    public:
        explicit HashCheckPlanner(HashCheckSettings settings = {}) : m_Settings(settings) {}
        std::vector<HashCheckPick> Plan(const HashCheckInput& in);
        const HashCheckSettings& Settings() const { return m_Settings; }
        /// @brief The tick a chunk was last checked for this peer (false: never, or it left the box).
        bool LastChecked(const glm::ivec3& chunk, uint32_t& tick) const;
        size_t Tracked() const { return m_Checked.size(); }

    private:
        HashCheckSettings m_Settings;
        std::unordered_map<glm::ivec3, uint32_t, ChunkCoordHash> m_Checked;
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
        /// Stage 8 (O2 / O5): the window's rate is the highest delivery rate of the last
        /// `deliveryWindows` intervals of `deliveryInterval` seconds (or the smoothed drain rate when that is
        /// higher). A lost datagram keeps its bytes in flight for a resend timeout, so the rate sampled tick
        /// by tick falls under loss and the window followed it down (5 % loss each way: 73 kB/s against 161
        /// through a fixed 32 KiB window); a delivery rate over half a second hardly exceeds the link's
        /// (a client that stalls and then drains its inbox can make one interval look faster: half a second,
        /// not a quarter, kept Session.ALateJoinsBulkIsPacedAndLiveEventsKeepFlowing's latency at 22 ticks),
        /// so the highest one is about the link's capacity, and the RTT rule still bounds the queue.
        double deliveryInterval = 0.5;
        size_t deliveryWindows = 8;
    };

    /**
     * @brief Delay-based bulk window: window = clamp(rate x (base RTT + target), floor,
     * ceiling) with the rate the highest recent delivery rate (stage 8), with the base RTT the lowest of the last minutes (aged per period); while the RTT is
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
        /// @brief The rate the window is sized by: max(Rate(), the highest recent delivery rate).
        double WindowRate() const;
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
        double m_IntervalBytes = 0.0, m_IntervalTime = 0.0;  ///< delivered in the current interval, and its length
        std::deque<double> m_Deliveries;                     ///< delivery rate of each recent interval, oldest first
    };
}
