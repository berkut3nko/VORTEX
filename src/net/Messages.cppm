module;

#include <glm/glm.hpp>

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

    /// Bumped whenever a message or an event changes its bytes. Part of Hello.
    inline constexpr uint16_t kProtocolVersion = 1;

    /// @brief First byte of every message. Wire format.
    enum class MessageType : uint8_t {
        Hello = 1,          ///< client -> server, events channel
        Welcome = 2,        ///< server -> client, events channel
        Refuse = 3,         ///< server -> client, events channel; the server then disconnects
        EventBatch = 4,     ///< server -> client, events channel: a run of the opSeq stream
        ServerStatus = 5,   ///< server -> client, state channel (unreliable), even ticks
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
        glm::dvec3 spawn{0.0};            ///< Where a player starts (the template's spawn point).
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

    /// @brief Sent unreliably on even ticks (stage 4 replaces it by the snapshot header).
    struct ServerStatus {
        uint32_t tick = 0;
        uint32_t lastOpSeq = 0;           ///< The last event the server has made.
    };

    std::vector<uint8_t> Encode(const Hello& m);
    std::vector<uint8_t> Encode(const Refuse& m);
    std::vector<uint8_t> Encode(const Welcome& m);
    std::vector<uint8_t> Encode(const ServerStatus& m);
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
    bool Decode(std::span<const uint8_t> bytes, ServerStatus& out);
}
