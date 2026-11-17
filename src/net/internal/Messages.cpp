module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
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

    const char* ToString(CastRejection rejection) {
        switch (rejection) {
            case CastRejection::None: return "none";
            case CastRejection::Cooldown: return "cooldown";
            case CastRejection::Missed: return "out of range";
            case CastRejection::Outside: return "outside the play area";
        }
        return "unknown";
    }

    namespace {
        constexpr uint32_t kMaxProbes = 256;

        bool Begin(ByteReader& in, MessageType type) { return in.U8() == static_cast<uint8_t>(type) && !in.Failed(); }
        bool End(const ByteReader& in) { return in.AtEnd(); }
        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        void WriteInput(ByteWriter& w, const sim::PlayerInput& in) {
            w.U8(static_cast<uint8_t>(in.moveX));
            w.U8(static_cast<uint8_t>(in.moveZ));
            w.U16(in.yaw);
            w.I16(in.pitch);
            w.U8(in.buttons);
        }

        sim::PlayerInput ReadInput(ByteReader& r) {
            sim::PlayerInput in;
            in.moveX = static_cast<int8_t>(r.U8());
            in.moveZ = static_cast<int8_t>(r.U8());
            in.yaw = r.U16();
            in.pitch = r.I16();
            in.buttons = r.U8();
            return in;
        }
    }

    glm::quat QuantizeRotation(const glm::quat& q) {
        glm::quat n = glm::normalize(q);
        if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) || !std::isfinite(n.w)) n = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        auto c = [](float v) { return static_cast<float>(static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))) / 32767.0f; };
        return glm::normalize(glm::quat(c(n.w), c(n.x), c(n.y), c(n.z)));
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
        WritePlayerTuning(w, m.player);
        w.F32(m.tickSeconds);
        w.F32(m.gravity.x);
        w.F32(m.gravity.y);
        w.F32(m.gravity.z);
        w.U32(m.joinOpSeq);
        w.U16(static_cast<uint16_t>(m.probes.size()));
        for (const ProbeHash& p : m.probes) {
            w.I32(p.chunk.x);
            w.I32(p.chunk.y);
            w.I32(p.chunk.z);
            w.U64(p.hash);
        }
        return w.Take();
    }

    std::vector<uint8_t> Encode(const InputBatch& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::Inputs));
        w.U32(m.firstTick);
        w.U8(static_cast<uint8_t>(m.inputs.size()));
        for (const sim::PlayerInput& in : m.inputs) WriteInput(w, in);
        return w.Take();
    }

    std::vector<uint8_t> Encode(const Snapshot& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::Snapshot));
        w.U32(m.tick);
        w.U32(m.lastOpSeq);
        w.U32(m.ackInputTick);
        w.U32(m.marginTick);
        w.I16(m.inputMargin);
        w.U8(m.hasOwner ? 1 : 0);
        if (m.hasOwner) {
            const sim::PlayerState& o = m.owner;
            for (const float f : {o.position.x, o.position.y, o.position.z, o.velocity.x, o.velocity.y, o.velocity.z}) w.F32(f);
            w.U8(o.flags);
            w.U8(o.cooldownTicks);
            w.U8(o.lastButtons);
            w.U32(m.lastCastTick);
            w.U8(static_cast<uint8_t>(m.castRejected));
        }
        w.U8(static_cast<uint8_t>(std::min<size_t>(m.others.size(), 255)));
        for (size_t i = 0; i < std::min<size_t>(m.others.size(), 255); ++i) {
            const RemotePlayer& p = m.others[i];
            w.U8(p.slot);
            w.F32(p.position.x);
            w.F32(p.position.y);
            w.F32(p.position.z);
            w.U16(p.yaw);
            w.U8(p.flags);
        }
        const size_t bodies = std::min<size_t>(m.bodies.size(), 255);
        w.U8(static_cast<uint8_t>(bodies));
        for (size_t i = 0; i < bodies; ++i) {
            const BodySample& b = m.bodies[i];
            w.U32(b.netId);
            w.F32(b.position.x);
            w.F32(b.position.y);
            w.F32(b.position.z);
            glm::quat q = glm::normalize(b.rotation);
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) q = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            for (const float c : {q.x, q.y, q.z, q.w}) w.I16(static_cast<int16_t>(std::lround(std::clamp(c, -1.0f, 1.0f) * 32767.0f)));
            const glm::vec3 v = QuantizeVelocity(b.velocity) * kVelocityQuanta;
            for (int a = 0; a < 3; ++a) w.I16(static_cast<int16_t>(std::lround(v[a])));
            w.U8(b.flags);
        }
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
        if (!ReadPlayerTuning(in, out.player)) return false;
        out.tickSeconds = in.F32();
        out.gravity.x = in.F32();
        out.gravity.y = in.F32();
        out.gravity.z = in.F32();
        out.joinOpSeq = in.U32();
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
        out.events.reserve(std::min<size_t>(count, in.Remaining() / 6));   // an event is at least 6 bytes (a despawn)
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

    bool Decode(std::span<const uint8_t> bytes, InputBatch& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::Inputs)) return false;
        out.firstTick = in.U32();
        const uint32_t count = in.U8();
        if (in.Failed() || count == 0 || count > kMaxBatchInputs || in.Remaining() != count * kInputBytes) return false;
        out.inputs.clear();
        for (uint32_t i = 0; i < count; ++i) {
            sim::PlayerInput input = ReadInput(in);
            input.tick = out.firstTick + i;
            out.inputs.push_back(input);
        }
        return End(in);
    }

    bool Decode(std::span<const uint8_t> bytes, Snapshot& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::Snapshot)) return false;
        out.tick = in.U32();
        out.lastOpSeq = in.U32();
        out.ackInputTick = in.U32();
        out.marginTick = in.U32();
        out.inputMargin = in.I16();
        const uint8_t hasOwner = in.U8();
        if (in.Failed() || hasOwner > 1) return false;
        out.hasOwner = hasOwner != 0;
        out.owner = sim::PlayerState{};
        out.lastCastTick = 0;
        out.castRejected = CastRejection::None;
        if (out.hasOwner) {
            sim::PlayerState& o = out.owner;
            for (float* f : {&o.position.x, &o.position.y, &o.position.z, &o.velocity.x, &o.velocity.y, &o.velocity.z}) *f = in.F32();
            o.flags = in.U8();
            o.cooldownTicks = in.U8();
            o.lastButtons = in.U8();
            out.lastCastTick = in.U32();
            const uint8_t rejected = in.U8();
            if (in.Failed() || rejected > kMaxCastRejection) return false;
            out.castRejected = static_cast<CastRejection>(rejected);
            if (!Finite(o.position) || !Finite(o.velocity)) return false;
        }
        const uint32_t count = in.U8();
        if (in.Failed() || in.Remaining() < count * kSnapshotPlayerBytes + 1) return false;
        out.others.clear();
        for (uint32_t i = 0; i < count; ++i) {
            RemotePlayer p;
            p.slot = in.U8();
            p.position.x = in.F32();
            p.position.y = in.F32();
            p.position.z = in.F32();
            p.yaw = in.U16();
            p.flags = in.U8();
            if (!Finite(p.position)) return false;
            out.others.push_back(p);
        }
        const uint32_t bodies = in.U8();
        if (in.Failed() || in.Remaining() != bodies * kSnapshotBodyBytes) return false;
        out.bodies.clear();
        for (uint32_t i = 0; i < bodies; ++i) {
            BodySample b;
            b.netId = in.U32();
            b.position.x = in.F32();
            b.position.y = in.F32();
            b.position.z = in.F32();
            float q[4];
            for (float& c : q) c = static_cast<float>(in.I16()) / 32767.0f;
            b.rotation = glm::quat(q[3], q[0], q[1], q[2]);
            const float len = glm::length(b.rotation);
            if (!(len > 0.5f)) return false;
            b.rotation = b.rotation / len;
            for (int a = 0; a < 3; ++a) b.velocity[a] = static_cast<float>(in.I16()) / kVelocityQuanta;
            b.flags = in.U8();
            if (!Finite(b.position)) return false;
            out.bodies.push_back(b);
        }
        return End(in);
    }

    // --- chunk sync (stage 7a) -------------------------------------------------------------------

    namespace {
        bool ValidChunk(const glm::ivec3& c) {
            for (int a = 0; a < 3; ++a)
                if (c[a] < -kMaxSyncChunkCoordinate || c[a] > kMaxSyncChunkCoordinate) return false;
            return true;
        }
        bool ValidWindowSize(const glm::ivec3& s) {
            for (int a = 0; a < 3; ++a)
                if (s[a] <= 0 || s[a] > kMaxClientWindowChunks) return false;
            return true;
        }
        void WriteChunk(ByteWriter& w, const glm::ivec3& c) {
            w.I32(c.x);
            w.I32(c.y);
            w.I32(c.z);
        }
        glm::ivec3 ReadChunk(ByteReader& r) {
            glm::ivec3 c;
            c.x = r.I32();
            c.y = r.I32();
            c.z = r.I32();
            return c;
        }
        void WriteSize(ByteWriter& w, const glm::ivec3& s) {
            w.U16(static_cast<uint16_t>(s.x));
            w.U16(static_cast<uint16_t>(s.y));
            w.U16(static_cast<uint16_t>(s.z));
        }
        glm::ivec3 ReadSize(ByteReader& r) {
            glm::ivec3 s;
            s.x = r.U16();
            s.y = r.U16();
            s.z = r.U16();
            return s;
        }
    }

    bool IsValidChunkStream(std::span<const uint8_t> bytes) {
        constexpr uint32_t kVolume = 32 * 32 * 32;
        if (bytes.empty() || bytes[0] > 2) return false;
        if (bytes[0] == 0) return bytes.size() == 1;
        if (bytes[0] == 1) return bytes.size() == 2 && bytes[1] != 0;
        if ((bytes.size() - 1) % 3 != 0) return false;
        uint32_t total = 0;
        for (size_t p = 1; p + 2 < bytes.size(); p += 3) {
            const uint32_t run = static_cast<uint32_t>(bytes[p + 1]) | (static_cast<uint32_t>(bytes[p + 2]) << 8);
            if (run == 0 || total + run > kVolume) return false;
            total += run;
        }
        return total == kVolume;
    }

    std::vector<uint8_t> Encode(const WindowUpdate& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::WindowUpdate));
        w.U32(m.windowSeq);
        w.U8(m.flags);
        WriteChunk(w, m.minChunk);
        WriteSize(w, m.size);
        return w.Take();
    }

    bool Decode(std::span<const uint8_t> bytes, WindowUpdate& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::WindowUpdate)) return false;
        out.windowSeq = in.U32();
        out.flags = in.U8();
        out.minChunk = ReadChunk(in);
        out.size = ReadSize(in);
        if (!End(in) || (out.flags & ~WindowUpdate::kReset) != 0) return false;
        return ValidWindowSize(out.size) && ValidChunk(out.minChunk) && ValidChunk(out.minChunk + out.size);
    }

    std::vector<uint8_t> Encode(const WindowAck& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::WindowAck));
        w.U32(m.windowSeq);
        w.U32(m.asOfOpSeq);
        WriteSize(w, m.modified.Size());
        WriteMaskRuns(w, m.modified);
        return w.Take();
    }

    bool Decode(std::span<const uint8_t> bytes, WindowAck& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::WindowAck)) return false;
        out.windowSeq = in.U32();
        out.asOfOpSeq = in.U32();
        const glm::ivec3 size = ReadSize(in);
        if (in.Failed() || !ValidWindowSize(size)) return false;
        if (!ReadMaskRuns(in, size, out.modified)) return false;
        return End(in);
    }

    std::vector<uint8_t> Encode(const ChunkData& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::ChunkData));
        WriteChunk(w, m.chunk);
        w.U32(m.asOfOpSeq);
        w.Bytes(m.rle);
        return w.Take();
    }

    bool Decode(std::span<const uint8_t> bytes, ChunkData& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::ChunkData)) return false;
        out.chunk = ReadChunk(in);
        out.asOfOpSeq = in.U32();
        if (in.Failed() || !ValidChunk(out.chunk)) return false;
        const std::span<const uint8_t> rle = in.Bytes(in.Remaining());
        if (in.Failed() || !IsValidChunkStream(rle)) return false;
        out.rle.assign(rle.begin(), rle.end());
        return End(in);
    }

    std::vector<uint8_t> Encode(const ChunkRequest& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::ChunkRequest));
        w.U16(static_cast<uint16_t>(m.chunks.size()));
        for (const glm::ivec3& c : m.chunks) WriteChunk(w, c);
        return w.Take();
    }

    bool Decode(std::span<const uint8_t> bytes, ChunkRequest& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::ChunkRequest)) return false;
        const uint32_t count = in.U16();
        if (in.Failed() || count == 0 || count > kMaxChunkRequest || in.Remaining() != count * 12u) return false;
        out.chunks.clear();
        for (uint32_t i = 0; i < count; ++i) {
            out.chunks.push_back(ReadChunk(in));
            if (!ValidChunk(out.chunks.back())) return false;
        }
        return End(in);
    }

    std::vector<uint8_t> Encode(const ModelRequest& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::ModelRequest));
        w.U16(static_cast<uint16_t>(m.netIds.size()));
        for (const uint32_t id : m.netIds) w.U32(id);
        return w.Take();
    }

    bool Decode(std::span<const uint8_t> bytes, ModelRequest& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::ModelRequest)) return false;
        const uint32_t count = in.U16();
        if (in.Failed() || count == 0 || count > kMaxModelRequest || in.Remaining() != count * 4u) return false;
        out.netIds.clear();
        for (uint32_t i = 0; i < count; ++i) {
            out.netIds.push_back(in.U32());
            if (out.netIds.back() == 0) return false;
        }
        return End(in);
    }

    std::vector<uint8_t> Encode(const BaselineEntity& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::BaselineEntity));
        w.U32(m.asOfOpSeq);
        w.U32(m.tick);
        w.U32(m.restTick);
        WriteEvent(w, Event{m.spawn});
        return w.Take();
    }

    bool Decode(std::span<const uint8_t> bytes, BaselineEntity& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::BaselineEntity)) return false;
        out.asOfOpSeq = in.U32();
        out.tick = in.U32();
        out.restTick = in.U32();
        Event event;
        uint64_t budget = kMaxMaskVoxels;
        if (in.Failed() || !ReadEvent(in, event, &budget)) return false;
        const EntitySpawn* spawn = std::get_if<EntitySpawn>(&event);
        if (!spawn || spawn->netId == 0) return false;
        out.spawn = *spawn;
        return End(in);
    }

    std::vector<uint8_t> Encode(const BaselineEnd& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::BaselineEnd));
        w.U32(m.asOfOpSeq);
        w.U32(m.count);
        return w.Take();
    }

    bool Decode(std::span<const uint8_t> bytes, BaselineEnd& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::BaselineEnd)) return false;
        out.asOfOpSeq = in.U32();
        out.count = in.U32();
        return End(in);
    }

    std::vector<uint8_t> Encode(const BaselineNear& m) {
        ByteWriter w;
        w.U8(static_cast<uint8_t>(MessageType::BaselineNear));
        w.U32(m.asOfOpSeq);
        w.U32(m.count);
        return w.Take();
    }

    bool Decode(std::span<const uint8_t> bytes, BaselineNear& out) {
        ByteReader in(bytes);
        if (!Begin(in, MessageType::BaselineNear)) return false;
        out.asOfOpSeq = in.U32();
        out.count = in.U32();
        return End(in);
    }
}
