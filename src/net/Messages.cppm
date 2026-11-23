module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

export module vortex.net:messages;

import vortex.voxel;
import vortex.sim;
import :wire;

export namespace vortex::net {

    /// Bumped whenever a message or an event changes its bytes. Part of Hello. 2: stage 4 (inputs,
    /// snapshots, the player tuning in Welcome; ServerStatus retired). 3: Snapshot::marginTick, the
    /// tick length and gravity in Welcome. 4: stage 5 (the owner block's lastCastTick / castRejected).
    /// 5: review of stage 5 (RefuseReason::LogTooLarge). 6: stage 6 (entity events, snapshot bodies,
    /// PlayerButton_Crate). 7: review of stage 6 (EntityWake). 8: stage 7a (chunk sync: WindowUpdate,
    /// WindowAck, ChunkData, ChunkRequest, ModelRequest, BaselineEntity, BaselineEnd; joins start at
    /// Welcome::joinOpSeq; RefuseReason 6 retired; BaselineNear 15 added by the review of 7a before
    /// protocol 8 was ever committed). 9: stage 7b (HashCheck, EntityCheck, EntityListRequest,
    /// EntityList, TerrainAck; ChunkRequest::reason; WindowUpdate::kTerrainMode accepted). 10: review
    /// of stage 7b (a TerrainAck codes the entering cells only, in ForEachEnteringChunk order).
    inline constexpr uint16_t kProtocolVersion = 10;

    /// @brief First byte of every message. Wire format.
    enum class MessageType : uint8_t {
        Hello = 1,          ///< client -> server, events channel
        Welcome = 2,        ///< server -> client, events channel
        Refuse = 3,         ///< server -> client, events channel; the server then disconnects
        EventBatch = 4,     ///< server -> client, events channel: a run of the opSeq stream
        // 5 was ServerStatus (protocol 1); the snapshot header replaced it. Never reuse it.
        Inputs = 6,         ///< client -> server, state channel (unreliable): the unacknowledged inputs
        Snapshot = 7,       ///< server -> client, state channel (unreliable), even ticks
        // Chunk sync (stage 7a, docs/design/networking.md "Stage 7a plan").
        WindowUpdate = 8,   ///< client -> server, events channel: the client's window
        WindowAck = 9,      ///< server -> client, events channel: the window's modified chunks at asOfOpSeq
        ChunkData = 10,     ///< server -> client, bulk channel: one chunk as of asOfOpSeq
        ChunkRequest = 11,  ///< client -> server, events channel: chunks to send (again)
        ModelRequest = 12,  ///< client -> server, events channel: replicated objects to send
        BaselineEntity = 13,///< server -> client, bulk channel: one replicated entity as of asOfOpSeq
        BaselineEnd = 14,   ///< server -> client, bulk channel: the join's baseline is complete
        BaselineNear = 15,  ///< server -> client, bulk channel: the baseline's entities near the spawn are out (review of 7a)
        // Stage 7b (networking.md "Stage 7b plan" with its amendments).
        TerrainAck = 16,    ///< server -> client, events channel: a terrain-mode window's entering chunks (server terrain mode)
        HashCheck = 17,     ///< server -> client, events channel: chunk hashes at asOfOpSeq, compared in stream order
        EntityCheck = 18,   ///< server -> client, events channel: the replicated objects' count and hashes at asOfOpSeq
        EntityListRequest = 19, ///< client -> server, events channel: an EntityCheck at asOfOpSeq did not match
        EntityList = 20,    ///< server -> client, events channel: every replicated object's NetId and model hash at asOfOpSeq
    };

    using Token = std::array<uint8_t, 16>;

    /// @brief Hello{protocolVersion, buildHash, token[16]} (docs/design/networking.md "Transport").
    struct Hello {
        uint16_t protocolVersion = kProtocolVersion;
        /// net::BuildHash: the generator's golden hash and the material table (shred.md R6).
        uint64_t buildHash = 0;
        Token token{};            ///< Compared in plain text with the session file (trusted testers).
    };

    enum class RefuseReason : uint8_t {
        Protocol = 1,     ///< another protocol version
        Build = 2,        ///< another generator or material table
        Token = 3,        ///< wrong token
        Full = 4,         ///< no free slot
        Malformed = 5,    ///< a message that does not decode
        // 6 was LogTooLarge (protocol 5-7: joins received the whole event log). Never reuse it.
    };
    const char* ToString(RefuseReason reason);

    struct Refuse {
        RefuseReason reason = RefuseReason::Malformed;
    };

    /// @brief One probe chunk: pristine generator output, never an edited chunk.
    struct ProbeHash {
        glm::ivec3 chunk{0};
        uint64_t hash = 0;
        friend bool operator==(const ProbeHash&, const ProbeHash&) = default;
    };

    /**
     * @brief The server's answer to a valid Hello. Followed on the same channel by the live stream
     * from `joinOpSeq + 1` on; what was made before reaches the joiner as chunk data and the entity
     * baseline (stage 7a; until protocol 7 the whole event log came instead).
     */
    struct Welcome {
        uint8_t slot = 0;
        uint32_t tick = 0;                ///< The server's tick when it answered.
        sim::RegionDesc region;
        glm::dvec3 spawn{0.0};            ///< Where this client's player starts (the template's spawn point; feet).
        /// The session's movement and cast tuning: the client steps its prediction with these, never
        /// with its own (a client checks sim::IsValid).
        sim::PlayerTuning player;
        /// The session's tick length and gravity (raw float bits): every predicted step depends on
        /// them, so a client builds its simulation with these (Client::MakeSimulationConfig) and
        /// refuses one with others (review of stage 4).
        float tickSeconds = 1.0f / 60.0f;
        glm::vec3 gravity{0.0f, -9.81f, 0.0f};
        /// The stream position the joiner starts after (J): no event at or before it is ever sent to it.
        uint32_t joinOpSeq = 0;
        std::vector<ProbeHash> probes;    ///< Hashes of pristine chunks chosen from the seed (ProbeChunks).
    };

    /// @brief A run of consecutive events, opSeq `firstOpSeq`, `firstOpSeq + 1`, ... made on server
    /// tick `tick`. Header 11 bytes (type, opSeq, tick, count).
    struct EventBatch {
        uint32_t firstOpSeq = 0;
        uint32_t tick = 0;
        std::vector<Event> events;
    };
    inline constexpr size_t kEventBatchHeaderBytes = 11;
    /// Events a decoder accepts in one batch.
    inline constexpr uint32_t kMaxBatchEvents = 65535;

    /// Inputs a decoder accepts in one InputBatch (a client sends at most ClientSettings::maxInputsPerPacket, 8).
    inline constexpr uint32_t kMaxBatchInputs = 32;
    /// Bytes of one input on the wire (moveX, moveZ, yaw, pitch, buttons) and of the batch header (type, first tick, count).
    inline constexpr size_t kInputBytes = 7;
    inline constexpr size_t kInputBatchHeaderBytes = 6;

    /**
     * @brief The inputs of consecutive ticks `firstTick`, `firstTick + 1`, ... (docs/design/networking.md
     * "Time and ticks": every packet carries the inputs the server has not acknowledged, at most 8).
     * Each input's `tick` is set by the decoder.
     */
    struct InputBatch {
        uint32_t firstTick = 0;
        std::vector<sim::PlayerInput> inputs;
    };

    /// @brief Another player as a snapshot shows it (interpolated by the client, drawn in the past).
    struct RemotePlayer {
        uint8_t slot = 0;
        glm::vec3 position{0.0f};         ///< feet
        uint16_t yaw = 0;                 ///< of its last input
        uint8_t flags = 0;                ///< sim::PlayerFlag
        friend bool operator==(const RemotePlayer&, const RemotePlayer&) = default;
    };

    /// @brief An awake replicated body in a snapshot (stage 6; 31 bytes: netId, position as raw
    /// floats, rotation as 4 x int16 / 32767, velocity as 3 x int16 in 1/256 m/s, flags).
    struct BodySample {
        uint32_t netId = 0;
        glm::vec3 position{0.0f};         ///< the body's shape origin (= model origin), world metres
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f};
        uint8_t flags = kBodyAwake;
        static constexpr uint8_t kBodyAwake = 1;
    };
    /// @brief The rotation and velocity a client gets for a BodySample (what the quantisation leaves).
    glm::quat QuantizeRotation(const glm::quat& q);

    /// @brief Why the server did not run a player's cast (Snapshot owner block). Wire format.
    enum class CastRejection : uint8_t {
        None = 0,       ///< queued (it runs when the cast queue reaches it) or run
        Cooldown = 1,   ///< the cooldown at the input's tick had not run out
        Missed = 2,     ///< the ray from the eye met no terrain within PlayerTuning::castRange (cast at dequeue)
        Outside = 3,    ///< the hit is outside the play bounds or the window's rows (sim::CastFitsRegion)
    };
    inline constexpr uint8_t kMaxCastRejection = 3;
    const char* ToString(CastRejection rejection);
    /// @brief The rejection a snapshot reports for a refusal of the rules seam (values are equal; [wp-6]).
    CastRejection RejectionOf(sim::ActionRefusal refusal);

    /// `Snapshot::inputMargin` before the server has had an input from this client.
    inline constexpr int16_t kNoInputMargin = -32768;
    /// Header (type, tick, lastOpSeq, ackInputTick, marginTick, inputMargin, owner flag, player
    /// count, body count), owner state, one other player and one body, in bytes.
    inline constexpr size_t kSnapshotHeaderBytes = 22;
    inline constexpr size_t kSnapshotOwnerBytes = 32;
    inline constexpr size_t kSnapshotPlayerBytes = 16;
    inline constexpr size_t kSnapshotBodyBytes = 31;
    /// A snapshot is at most this large (networking.md "Entity replication": 1200 B hold about 36
    /// bodies after the players); bodies that do not fit wait for a later one (priority).
    inline constexpr size_t kSnapshotBudgetBytes = 1200;

    /**
     * @brief The state channel down, on even ticks (docs/design/networking.md "Player movement"): the
     * server's tick, the event stream position at that tick, what the server has of this client's
     * inputs, the owner's own state (unquantised) and every other player.
     */
    struct Snapshot {
        uint32_t tick = 0;                ///< The tick just simulated: `owner` is the state after it.
        /// The last event the server had made by the end of `tick` (opSeqAtTick): a client compares
        /// `owner` with its prediction only once it has applied the events up to here.
        uint32_t lastOpSeq = 0;
        uint32_t ackInputTick = 0;        ///< The newest input tick the server has of this client (0 = none).
        /// The input `inputMargin` is about: the newest input of the packet with the newest inputs
        /// that arrived so far, dropped ones included (an input 16 or more ticks early is dropped and
        /// never acknowledged, but its margin is what the clock must hear; review of stage 4).
        uint32_t marginTick = 0;
        /// How many ticks before it was needed the input of `marginTick` arrived (negative: late);
        /// kNoInputMargin before the first input. Drives the client's clock.
        int16_t inputMargin = kNoInputMargin;
        bool hasOwner = false;
        sim::PlayerState owner;
        /// Owner block (stage 5): the input tick of the newest cast edge the server took from this
        /// player (0 = none yet) and, if it was not run, why. A cast still queued is None.
        uint32_t lastCastTick = 0;
        CastRejection castRejected = CastRejection::None;
        std::vector<RemotePlayer> others;
        /// Awake replicated bodies, by the server's priority for this client (stage 6). At most 255.
        std::vector<BodySample> bodies;
    };

    // --- chunk sync (stage 7a) -------------------------------------------------------------------

    /// Largest client window a decoder accepts per axis, and the chunk coordinates it accepts.
    inline constexpr int32_t kMaxClientWindowChunks = 128;
    inline constexpr int32_t kMaxSyncChunkCoordinate = 1 << 24;
    /// Most coordinates / NetIds in one ChunkRequest / ModelRequest.
    inline constexpr uint32_t kMaxChunkRequest = 256;
    inline constexpr uint32_t kMaxModelRequest = 64;

    /// @brief The client's window (a box of chunks), numbered 1, 2, ... per client. Every chunk of
    /// the window that was not in the previous one (all of them with `kReset` or for the first) is
    /// "entering": the server says which of them are modified (WindowAck) and sends those.
    struct WindowUpdate {
        uint32_t windowSeq = 0;
        uint8_t flags = 0;
        glm::ivec3 minChunk{0};
        glm::ivec3 size{0};
        static constexpr uint8_t kReset = 1;          ///< every chunk of this window counts as entering
        /// Server terrain mode (stage 7b): this client does not generate the server's terrain; every
        /// entering chunk is described by a TerrainAck (uniform inline, the others as ChunkData).
        /// Sticky: a peer that sent it once must send it on every later update.
        static constexpr uint8_t kTerrainMode = 2;
    };
    inline constexpr size_t kWindowUpdateBytes = 24;

    /// @brief The server's answer to WindowUpdate `windowSeq`: `modified` (over the update's box,
    /// VoxelMask order: x fastest, then y, then z) has a bit for every entering chunk the server's
    /// modified map holds at `asOfOpSeq`; it will come as ChunkData. An entering chunk without a bit
    /// is pristine at `asOfOpSeq` (the client's generator makes it).
    struct WindowAck {
        uint32_t windowSeq = 0;
        uint32_t asOfOpSeq = 0;
        voxel::VoxelMask modified;
    };

    /// @brief One chunk as it was after event `asOfOpSeq` (World::EncodeChunkAt bytes), encoded when
    /// the server dequeued it.
    struct ChunkData {
        glm::ivec3 chunk{0};
        uint32_t asOfOpSeq = 0;
        std::vector<uint8_t> rle;
    };
    inline constexpr size_t kChunkDataHeaderBytes = 17;

    /// @brief Why chunks are asked for (stage 7b). Wire format.
    enum class ChunkRequestReason : uint8_t {
        Sync = 0,       ///< the ring no longer covered the chunk's base, or a slot freed for an Unsynced one (7a)
        Repair = 1,     ///< a HashCheck found the chunk different from the server's (7b)
    };
    inline constexpr uint8_t kMaxChunkRequestReason = 1;

    struct ChunkRequest {
        std::vector<glm::ivec3> chunks;     ///< 1..kMaxChunkRequest
        ChunkRequestReason reason = ChunkRequestReason::Sync;
    };
    struct ModelRequest {
        std::vector<uint32_t> netIds;       ///< 1..kMaxModelRequest, none 0
    };

    /// @brief A replicated entity of a joiner's baseline (or a ModelRequest's answer) as it was after
    /// event `asOfOpSeq` on server tick `tick`: an EntitySpawn (a light, or an object; a resting object
    /// carries its exact EntityRest pose and zero velocity, `restTick` its rest's tick).
    struct BaselineEntity {
        uint32_t asOfOpSeq = 0;
        uint32_t tick = 0;
        uint32_t restTick = kAwake;
        EntitySpawn spawn;
        static constexpr uint32_t kAwake = 0xFFFFFFFFu;
    };
    /// @brief The join's baseline is complete: `count` BaselineEntity messages were sent before it.
    struct BaselineEnd {
        uint32_t asOfOpSeq = 0;
        uint32_t count = 0;
    };
    /// @brief The join's baseline entities within ServerSettings::baselineNearMetres of the spawn are
    /// sent (`count` BaselineEntity messages so far): the joiner's start gate may open. Same layout as
    /// BaselineEnd (9 bytes).
    struct BaselineNear {
        uint32_t asOfOpSeq = 0;
        uint32_t count = 0;
    };

    // --- checks and server terrain mode (stage 7b) ------------------------------------------------

    /// @brief What a TerrainAck says about one cell of the window (wire format).
    enum class TerrainCode : uint8_t {
        NotInAck = 0,     ///< not entering with this update, or outside the server's window (never edited: generated here)
        Data = 1,         ///< the server holds it Dense (visible or modified): ChunkData follows on bulk, installed as content (InstallMode::Content: shown, not an edit)
        Uniform = 2,      ///< every voxel has the run's id (0 = air) as of asOfOpSeq
        HiddenData = 3,   ///< the server holds it Packed (hidden, unmodified): ChunkData follows, installed packed (amendment 1)
    };
    inline constexpr uint8_t kMaxTerrainCode = 3;
    /// @brief `count` consecutive entering cells (ForEachEnteringChunk order) with one code.
    struct TerrainRun {
        uint32_t count = 0;               ///< at least 1
        TerrainCode code = TerrainCode::NotInAck;
        uint8_t id = 0;                   ///< Uniform only
        friend bool operator==(const TerrainRun&, const TerrainRun&) = default;
    };
    /// @brief The answer to a terrain-mode WindowUpdate (stage 7b, server terrain mode): one code per
    /// cell that entered with the update (all of its box for the first update, a reset or a new size;
    /// else the cells outside the previous window), in ForEachEnteringChunk order, as runs. On the wire
    /// the runs follow their total (LEB128, at most the box's cells): review of stage 7b, protocol 10 -
    /// a window step's ack is about its 4224 entering cells, not the 101 376 of the box.
    struct TerrainAck {
        uint32_t windowSeq = 0;
        uint32_t asOfOpSeq = 0;
        glm::ivec3 size{0};
        std::vector<TerrainRun> runs;
    };

    /// Most entries a HashCheck holds.
    inline constexpr uint32_t kMaxHashCheckEntries = 32;
    /// @brief One chunk of a HashCheck: the server's ChunkHash of it after event asOfOpSeq.
    struct HashCheckEntry {
        glm::ivec3 chunk{0};
        uint64_t hash = 0;
        bool pristine = false;            ///< not in the server's modified map: the client's generator made it
        friend bool operator==(const HashCheckEntry&, const HashCheckEntry&) = default;
    };
    /// @brief Chunk hashes after event `asOfOpSeq` (C), sent right after the events up to C: the
    /// client compares them in stream order (applied == C) for its current chunks whose base is at
    /// or below C, and asks for the ones that differ (ChunkRequest reason Repair).
    struct HashCheck {
        uint32_t asOfOpSeq = 0;
        std::vector<HashCheckEntry> entries;   ///< 1..kMaxHashCheckEntries
    };
    inline constexpr size_t kHashCheckHeaderBytes = 6;
    inline constexpr size_t kHashCheckEntryBytes = 21;

    /// @brief The replicated objects after event `asOfOpSeq` (C), in stream order like HashCheck:
    /// how many, the xor of their NetIds and the xor of HashMix64(netId, VolumeHash(model)) (a plain
    /// xor of model hashes would cancel for any two crates). Lights are not counted.
    struct EntityCheck {
        uint32_t asOfOpSeq = 0;
        uint32_t count = 0;
        uint32_t idXor = 0;
        uint64_t modelXor = 0;
        friend bool operator==(const EntityCheck&, const EntityCheck&) = default;
    };
    inline constexpr size_t kEntityCheckBytes = 21;
    /// @brief One object's term of EntityCheck::modelXor.
    uint64_t EntityModelTerm(uint32_t netId, uint64_t modelHash);

    /// @brief A client's EntityCheck at `asOfOpSeq` did not match: send the list (merged while one is owed).
    struct EntityListRequest {
        uint32_t asOfOpSeq = 0;
    };
    inline constexpr size_t kEntityListRequestBytes = 5;

    /// Most entries an EntityList holds.
    inline constexpr uint32_t kMaxEntityList = 16384;
    struct EntityListEntry {
        uint32_t netId = 0;
        uint64_t modelHash = 0;           ///< VolumeHash of its model
        friend bool operator==(const EntityListEntry&, const EntityListEntry&) = default;
    };
    /// @brief Every replicated object after event `asOfOpSeq` (Q), NetId order, sent right after the
    /// events up to Q (amendment 2: ids and models, not bodies; the client asks for the bodies it
    /// lacks or holds with another model, and drops the ones the list does not have).
    struct EntityList {
        uint32_t asOfOpSeq = 0;
        std::vector<EntityListEntry> entries;
    };
    inline constexpr size_t kEntityListHeaderBytes = 9;
    inline constexpr size_t kEntityListEntryBytes = 12;

    std::vector<uint8_t> Encode(const TerrainAck& m);
    std::vector<uint8_t> Encode(const HashCheck& m);
    std::vector<uint8_t> Encode(const EntityCheck& m);
    std::vector<uint8_t> Encode(const EntityListRequest& m);
    std::vector<uint8_t> Encode(const EntityList& m);
    /// @brief Refuses a size outside 1..kMaxClientWindowChunks, a run of 0, an unknown code, runs that
    /// do not add up to the box exactly (short or long).
    bool Decode(std::span<const uint8_t> bytes, TerrainAck& out);
    /// @brief Refuses a count of 0 or over kMaxHashCheckEntries, unknown flag bits, coordinates
    /// beyond kMaxSyncChunkCoordinate.
    bool Decode(std::span<const uint8_t> bytes, HashCheck& out);
    bool Decode(std::span<const uint8_t> bytes, EntityCheck& out);
    bool Decode(std::span<const uint8_t> bytes, EntityListRequest& out);
    /// @brief Refuses more than kMaxEntityList entries, a NetId of 0, NetIds not strictly ascending.
    bool Decode(std::span<const uint8_t> bytes, EntityList& out);

    /// @brief True if `bytes` is a valid EncodeChunk stream (kind, uniform id, runs adding up).
    bool IsValidChunkStream(std::span<const uint8_t> bytes);

    std::vector<uint8_t> Encode(const WindowUpdate& m);
    std::vector<uint8_t> Encode(const WindowAck& m);
    std::vector<uint8_t> Encode(const ChunkData& m);
    std::vector<uint8_t> Encode(const ChunkRequest& m);
    std::vector<uint8_t> Encode(const ModelRequest& m);
    std::vector<uint8_t> Encode(const BaselineEntity& m);
    std::vector<uint8_t> Encode(const BaselineEnd& m);
    std::vector<uint8_t> Encode(const BaselineNear& m);
    /// @brief Refuses a size of 0 or over kMaxClientWindowChunks, coordinates beyond
    /// kMaxSyncChunkCoordinate, flags other than kReset and kTerrainMode.
    bool Decode(std::span<const uint8_t> bytes, WindowUpdate& out);
    /// @brief Refuses runs that do not add up to the box.
    bool Decode(std::span<const uint8_t> bytes, WindowAck& out);
    /// @brief Refuses a chunk body that is not a valid codec stream.
    bool Decode(std::span<const uint8_t> bytes, ChunkData& out);
    bool Decode(std::span<const uint8_t> bytes, ChunkRequest& out);
    bool Decode(std::span<const uint8_t> bytes, ModelRequest& out);
    /// @brief Refuses an event that is not an EntitySpawn with a NetId, or a model over the batch budget.
    bool Decode(std::span<const uint8_t> bytes, BaselineEntity& out);
    bool Decode(std::span<const uint8_t> bytes, BaselineEnd& out);
    bool Decode(std::span<const uint8_t> bytes, BaselineNear& out);

    std::vector<uint8_t> Encode(const Hello& m);
    std::vector<uint8_t> Encode(const Refuse& m);
    std::vector<uint8_t> Encode(const Welcome& m);
    std::vector<uint8_t> Encode(const InputBatch& m);
    std::vector<uint8_t> Encode(const Snapshot& m);
    /// @brief A batch whose events are already encoded (WriteEvent bytes, concatenated): the
    /// server encodes each event once, when it is made.
    std::vector<uint8_t> EncodeBatch(uint32_t firstOpSeq, uint32_t tick, uint32_t count, std::span<const uint8_t> eventBytes);
    std::vector<uint8_t> Encode(const EventBatch& m);

    /// @brief The type byte, or nothing for an empty message.
    std::optional<MessageType> PeekType(std::span<const uint8_t> bytes);
    /// @brief Each decoder checks the type byte, every field and that nothing is left over. An
    /// EventBatch decodes at most kMaxMaskVoxels of masks in all.
    bool Decode(std::span<const uint8_t> bytes, Hello& out);
    bool Decode(std::span<const uint8_t> bytes, Refuse& out);
    bool Decode(std::span<const uint8_t> bytes, Welcome& out);
    bool Decode(std::span<const uint8_t> bytes, EventBatch& out);
    /// @brief At most kMaxBatchInputs inputs, at least one.
    bool Decode(std::span<const uint8_t> bytes, InputBatch& out);
    /// @brief Refuses non-finite positions and velocities.
    bool Decode(std::span<const uint8_t> bytes, Snapshot& out);
}
