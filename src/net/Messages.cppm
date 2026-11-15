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

import vortex.sim;
import :wire;

export namespace vortex::net {

    /// Bumped whenever a message or an event changes its bytes. Part of Hello. 2: stage 4 (inputs,
    /// snapshots, the player tuning in Welcome; ServerStatus retired). 3: Snapshot::marginTick, the
    /// tick length and gravity in Welcome. 4: stage 5 (the owner block's lastCastTick / castRejected).
    /// 5: review of stage 5 (RefuseReason::LogTooLarge). 6: stage 6 (entity events, snapshot bodies,
    /// PlayerButton_Crate).
    inline constexpr uint16_t kProtocolVersion = 6;

    /// @brief First byte of every message. Wire format.
    enum class MessageType : uint8_t {
        Hello = 1,          ///< client -> server, events channel
        Welcome = 2,        ///< server -> client, events channel
        Refuse = 3,         ///< server -> client, events channel; the server then disconnects
        EventBatch = 4,     ///< server -> client, events channel: a run of the opSeq stream
        // 5 was ServerStatus (protocol 1); the snapshot header replaced it. Never reuse it.
        Inputs = 6,         ///< client -> server, state channel (unreliable): the unacknowledged inputs
        Snapshot = 7,       ///< server -> client, state channel (unreliable), even ticks
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
        LogTooLarge = 6,  ///< the event log a joiner would receive is over ServerSettings::maxJoinLogBytes (until stage 7)
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
     * @brief The server's answer to a valid Hello. Followed on the same channel by the whole event
     * log (EventBatch, opSeq 1..`logEnd`) and then the live stream (stage 3: "join = full event log").
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
        uint32_t logEnd = 0;              ///< opSeq of the last event already in the log.
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
