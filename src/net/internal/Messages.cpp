module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

module vortex.net;

import vortex.sim;

namespace vortex::net {

    const char* ToString(RefuseReason reason) {
        switch (reason) {
            case RefuseReason::Protocol: return "another protocol version";
            case RefuseReason::Build: return "another build (generator or material table)";
            case RefuseReason::Token: return "wrong token";
            case RefuseReason::Full: return "the session is full";
            case RefuseReason::Malformed: return "malformed message";
        }
        return "unknown reason";
    }

    namespace {
        constexpr uint32_t kMaxProbes = 256;

        bool Begin(ByteReader& in, MessageType type) { return in.U8() == static_cast<uint8_t>(type) && !in.Failed(); }
        bool End(const ByteReader& in) { return in.AtEnd(); }
    }

    std::vector<uint8_t> Encode(const Hello& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::Hello));
        w.U16(m.protocolVersion);
        w.U64(m.buildHash);
        w.Bytes(m.token);
        return w.Take();
    }

    std::vector<uint8_t> Encode(const Refuse& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::Refuse));
        w.U8(static_cast<uint8_t>(m.reason));
        return w.Take();
    }

    std::vector<uint8_t> Encode(const Welcome& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::Welcome));
        w.U8(m.slot);
        w.U32(m.tick);
        WriteRegion(w, m.region);
        w.F64(m.spawn.x);
        w.F64(m.spawn.y);
        w.F64(m.spawn.z);
        w.U32(m.logEnd);
        w.U16(static_cast<uint16_t>(m.probes.size()));
        for (const ProbeHash& p : m.probes) {
            w.I32(p.chunk.x);
            w.I32(p.chunk.y);
            w.I32(p.chunk.z);
            w.U64(p.hash);
        }
        return w.Take();
    }

    std::vector<uint8_t> Encode(const ServerStatus& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::ServerStatus));
        w.U32(m.tick);
        w.U32(m.lastOpSeq);
        return w.Take();
    }

    std::vector<uint8_t> EncodeBatch(uint32_t firstOpSeq, uint32_t tick, uint32_t count, std::span<const uint8_t> eventBytes) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::EventBatch));
        w.U32(firstOpSeq);
        w.U32(tick);
        w.U16(static_cast<uint16_t>(count));
        w.Bytes(eventBytes);
        return w.Take();
    }

    std::vector<uint8_t> Encode(const EventBatch& m) {
        ByteWriter events;
        for (const Event& e : m.events) WriteEvent(events, e);
        return EncodeBatch(m.firstOpSeq, m.tick, static_cast<uint32_t>(m.events.size()), events.Data());
    }

    std::optional<MessageType> PeekType(std::span<const uint8_t> bytes) {
        if (bytes.empty()) return std::nullopt;
        return static_cast<MessageType>(bytes[0]);
    }

    bool Decode(std::span<const uint8_t> bytes, Hello& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::Hello)) return false;
        out.protocolVersion = in.U16();
        out.buildHash = in.U64();
        const std::span<const uint8_t> token = in.Bytes(out.token.size());
        if (in.Failed()) return false;
        for (size_t i = 0; i < out.token.size(); ++i) out.token[i] = token[i];
        return End(in);
    }

    bool Decode(std::span<const uint8_t> bytes, Refuse& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::Refuse)) return false;
        out.reason = static_cast<RefuseReason>(in.U8());
        return End(in);
    }

    bool Decode(std::span<const uint8_t> bytes, Welcome& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::Welcome)) return false;
        out.slot = in.U8();
        out.tick = in.U32();
        if (!ReadRegion(in, out.region)) return false;
        out.spawn.x = in.F64();
        out.spawn.y = in.F64();
        out.spawn.z = in.F64();
        out.logEnd = in.U32();
        const uint32_t probes = in.U16();
        if (in.Failed() || probes > kMaxProbes) return false;
        out.probes.clear();
        for (uint32_t i = 0; i < probes; ++i) {
            ProbeHash p;
            p.chunk.x = in.I32();
            p.chunk.y = in.I32();
            p.chunk.z = in.I32();
            p.hash = in.U64();
            out.probes.push_back(p);
        }
        return !in.Failed() && End(in);
    }

    bool Decode(std::span<const uint8_t> bytes, EventBatch& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::EventBatch)) return false;
        out.firstOpSeq = in.U32();
        out.tick = in.U32();
        const uint32_t count = in.U16();
        if (in.Failed()) return false;
        out.events.clear();
        out.events.reserve(std::min<size_t>(count, in.Remaining() / 16));   // an event is at least 16 bytes
        // The masks of one batch decode to at most kMaxMaskVoxels in all (checked before each is
        // allocated): a few bytes of runs cannot be repeated into gigabytes.
        uint64_t maskBudget = kMaxMaskVoxels;
        for (uint32_t i = 0; i < count; ++i) {
            Event e;
            if (!ReadEvent(in, e, &maskBudget)) return false;
            out.events.push_back(std::move(e));
        }
        return End(in);
    }

    bool Decode(std::span<const uint8_t> bytes, ServerStatus& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::ServerStatus)) return false;
        out.tick = in.U32();
        out.lastOpSeq = in.U32();
        return End(in);
    }
}
