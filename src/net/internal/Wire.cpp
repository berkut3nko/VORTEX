module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

module vortex.net;

import vortex.voxel;
import vortex.scene;
import vortex.sim;

namespace vortex::net {

    // --- ByteWriter / ByteReader ------------------------------------------------------------------

    void ByteWriter::U16(uint16_t v) {
        m_Bytes.push_back(static_cast<uint8_t>(v));
        m_Bytes.push_back(static_cast<uint8_t>(v >> 8));
    }
    void ByteWriter::U32(uint32_t v) {
        for (int i = 0; i < 4; ++i) m_Bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void ByteWriter::U64(uint64_t v) {
        for (int i = 0; i < 8; ++i) m_Bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void ByteWriter::F32(float v) { U32(std::bit_cast<uint32_t>(v)); }
    void ByteWriter::F64(double v) { U64(std::bit_cast<uint64_t>(v)); }
    void ByteWriter::VarU(uint64_t v) {
        while (v >= 0x80) {
            m_Bytes.push_back(static_cast<uint8_t>(v | 0x80));
            v >>= 7;
        }
        m_Bytes.push_back(static_cast<uint8_t>(v));
    }

    uint8_t ByteReader::U8() {
        if (m_Failed || m_Pos >= m_Bytes.size()) {
            m_Failed = true;
            return 0;
        }
        return m_Bytes[m_Pos++];
    }
    uint16_t ByteReader::U16() {
        const uint16_t lo = U8();
        return static_cast<uint16_t>(lo | (static_cast<uint16_t>(U8()) << 8));
    }
    uint32_t ByteReader::U32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(U8()) << (8 * i);
        return v;
    }
    uint64_t ByteReader::U64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(U8()) << (8 * i);
        return v;
    }
    float ByteReader::F32() { return std::bit_cast<float>(U32()); }
    double ByteReader::F64() { return std::bit_cast<double>(U64()); }
    uint64_t ByteReader::VarU() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const uint8_t b = U8();
            if (m_Failed) return 0;
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) return v;
        }
        m_Failed = true;     // more than 10 bytes
        return 0;
    }
    std::span<const uint8_t> ByteReader::Bytes(size_t count) {
        if (m_Failed || m_Bytes.size() - m_Pos < count) {
            m_Failed = true;
            return {};
        }
        const std::span<const uint8_t> out = m_Bytes.subspan(m_Pos, count);
        m_Pos += count;
        return out;
    }

    // --- masks --------------------------------------------------------------------------------------

    void WriteMaskRuns(ByteWriter& out, const voxel::VoxelMask& mask) {
        const size_t total = mask.VoxelCount();
        const std::vector<uint8_t>& bits = mask.Bytes();
        bool value = false;
        uint64_t run = 0;
        size_t i = 0;
        while (i < total) {
            // Whole bytes equal to the run's value go at once (masks are mostly empty or full).
            if ((i & 7u) == 0 && i + 8 <= total && bits[i >> 3] == (value ? 0xffu : 0x00u)) {
                run += 8;
                i += 8;
                continue;
            }
            const bool bit = ((bits[i >> 3] >> (i & 7u)) & 1u) != 0;
            if (bit != value) {
                out.VarU(run);
                value = bit;
                run = 0;
            }
            ++run;
            ++i;
        }
        out.VarU(run);
    }

    bool ReadMaskRuns(ByteReader& in, const glm::ivec3& size, voxel::VoxelMask& mask, uint64_t* voxelBudget) {
        if (size.x < 0 || size.y < 0 || size.z < 0 || size.x > kMaxMaskAxis || size.y > kMaxMaskAxis || size.z > kMaxMaskAxis) return false;
        const uint64_t total = static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.y) * static_cast<uint64_t>(size.z);
        if (total > kMaxMaskVoxels) return false;
        if (voxelBudget) {
            if (total > *voxelBudget) return false;
            *voxelBudget -= total;
        }
        voxel::VoxelMask out(size);
        uint64_t at = 0;
        bool value = false;
        bool first = true;
        for (;;) {
            const uint64_t run = in.VarU();
            if (in.Failed()) return false;
            if (run > total - at) return false;
            if (run == 0 && !first) return false;   // only the leading clear run may be empty
            if (value) out.SetRange(static_cast<size_t>(at), static_cast<size_t>(at + run));
            at += run;
            first = false;
            value = !value;
            if (at == total) break;
        }
        mask = std::move(out);
        return true;
    }

    // --- object models -------------------------------------------------------------------------------

    void WriteVolumeRuns(ByteWriter& out, const voxel::VoxelVolume& volume) {
        const glm::ivec3 size = volume.Size();
        out.U16(static_cast<uint16_t>(size.x));
        out.U16(static_cast<uint16_t>(size.y));
        out.U16(static_cast<uint16_t>(size.z));
        const std::vector<uint8_t>& data = volume.Data();
        size_t i = 0;
        while (i < data.size()) {
            size_t j = i + 1;
            while (j < data.size() && data[j] == data[i]) ++j;
            out.VarU(j - i);
            out.U8(data[i]);
            i = j;
        }
    }

    bool ReadVolumeRuns(ByteReader& in, voxel::VoxelVolume& volume, uint64_t* voxelBudget) {
        glm::ivec3 size;
        size.x = in.U16();
        size.y = in.U16();
        size.z = in.U16();
        if (in.Failed() || size.x > kMaxModelAxis || size.y > kMaxModelAxis || size.z > kMaxModelAxis) return false;
        const uint64_t total = static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.y) * static_cast<uint64_t>(size.z);
        if (total > kMaxMaskVoxels) return false;
        if (voxelBudget) {
            if (total > *voxelBudget) return false;
            *voxelBudget -= total;
        }
        voxel::VoxelVolume out(size);
        std::vector<uint8_t>& data = out.Data();
        uint64_t at = 0;
        while (at < total) {
            const uint64_t run = in.VarU();
            const uint8_t id = in.U8();
            if (in.Failed() || run == 0 || run > total - at) return false;
            std::fill(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + run), id);
            at += run;
        }
        volume = std::move(out);
        return true;
    }

    uint64_t VolumeHash(const voxel::VoxelVolume& volume) {
        uint64_t h = voxel::HashMix64(0x6d6f64656cull, static_cast<uint64_t>(static_cast<uint32_t>(volume.Size().x)));
        h = voxel::HashMix64(h, static_cast<uint64_t>(static_cast<uint32_t>(volume.Size().y)));
        h = voxel::HashMix64(h, static_cast<uint64_t>(static_cast<uint32_t>(volume.Size().z)));
        const std::vector<uint8_t>& data = volume.Data();
        for (size_t i = 0; i < data.size(); i += 8) {
            uint64_t word = 0;
            for (size_t k = 0; k < 8 && i + k < data.size(); ++k) word |= static_cast<uint64_t>(data[i + k]) << (8 * k);
            h = voxel::HashMix64(h, word);
        }
        return h;
    }

    namespace {
        bool SameVolume(const voxel::VoxelVolume& a, const voxel::VoxelVolume& b) { return a.Size() == b.Size() && a.Data() == b.Data(); }
    }

    bool operator==(const EntitySpawn& a, const EntitySpawn& b) {
        if (a.netId != b.netId || a.kind != b.kind) return false;
        if (a.kind == EntityKind::Light) return a.light == b.light;
        const ObjectSpawn& x = a.object;
        const ObjectSpawn& y = b.object;
        const bool held = (x.flags & kObjectHeld) == 0 || (x.holder == y.holder && x.relYaw == y.relYaw && x.holdSince == y.holdSince);
        return x.position == y.position && x.rotation == y.rotation && x.linearVelocity == y.linearVelocity &&
               x.angularVelocity == y.angularVelocity && x.motion == y.motion && x.flags == y.flags && SameVolume(x.volume, y.volume) &&
               held && (a.kind != EntityKind::Mechanism || a.mechanism == b.mechanism);
    }

    Event HoldEvent(const sim::HoldChange& change, uint32_t netId) {
        if (change.hold) return EntityHold{netId, change.holder, change.relYaw, change.tick};
        return EntityRelease{netId, change.tick, change.reason, change.position, change.rotation, QuantizeVelocity(change.velocity)};
    }

    bool operator==(const ObjectModel& a, const ObjectModel& b) {
        return a.netId == b.netId && a.offset == b.offset && a.motion == b.motion && SameVolume(a.volume, b.volume);
    }

    // --- events -----------------------------------------------------------------------------------

    glm::vec3 QuantizeVelocity(const glm::vec3& v) { return sim::QuantizeVelocity(v); }   // one formula ([mech-1] releases)

    namespace {
        void WriteIvec3(ByteWriter& out, const glm::ivec3& v) {
            out.I32(v.x);
            out.I32(v.y);
            out.I32(v.z);
        }
        glm::ivec3 ReadIvec3(ByteReader& in) {
            const int32_t x = in.I32(), y = in.I32(), z = in.I32();
            return {x, y, z};
        }

        bool ValidMap(uint8_t map) { return map != 0 && map < voxel::kTurningMapCount; }

        /// MapMask's runs in the order of `axis` (voxel::MaskRuns), LEB128; the first run is clear and may be 0.
        void WriteAxisRuns(ByteWriter& out, const voxel::VoxelMask& mask, uint8_t axis) {
            for (const uint32_t run : voxel::MaskRuns(mask, axis)) out.VarU(run);
        }

        bool ReadAxisRuns(ByteReader& in, const glm::ivec3& size, uint8_t axis, voxel::VoxelMask& mask, uint64_t* voxelBudget) {
            if (axis > 2) return false;
            for (int a = 0; a < 3; ++a)
                if (size[a] < 0 || size[a] > kMaxMaskAxis) return false;
            const uint64_t total = static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.y) * static_cast<uint64_t>(size.z);
            if (total > kMaxMaskVoxels) return false;
            if (voxelBudget) {
                if (total > *voxelBudget) return false;
                *voxelBudget -= total;
            }
            // Every run after the first is at least one voxel and one byte: the list is bounded by the message.
            std::vector<uint32_t> runs;
            uint64_t at = 0;
            bool first = true;
            while (first || at < total) {
                const uint64_t run = in.VarU();
                if (in.Failed() || run > total - at || (run == 0 && !first)) return false;
                runs.push_back(static_cast<uint32_t>(run));
                at += run;
                first = false;
            }
            return voxel::MaskFromRuns(size, axis, runs, mask);
        }
    }

    void WriteShape(ByteWriter& out, const voxel::ShapeQ& shape) {
        out.U8(shape.kind);
        switch (shape.kind) {
            case voxel::kShapeSphere:
                WriteIvec3(out, shape.a);
                out.U16(shape.radiusQ);
                break;
            case voxel::kShapeCapsule:
                WriteIvec3(out, shape.a);
                WriteIvec3(out, shape.b);
                out.U16(shape.radiusQ);
                break;
            case voxel::kShapeBox:
                WriteIvec3(out, shape.a);
                WriteIvec3(out, shape.b);
                break;
            case voxel::kShapeCylinder:
                WriteIvec3(out, shape.a);
                out.U16(shape.radiusQ);
                out.U16(shape.halfHeightQ);
                break;
            default: break;
        }
    }

    bool ReadShape(ByteReader& in, voxel::ShapeQ& shape) {
        shape = voxel::ShapeQ{};
        shape.kind = in.U8();
        switch (shape.kind) {
            case voxel::kShapeSphere:
                shape.a = ReadIvec3(in);
                shape.radiusQ = in.U16();
                return !in.Failed() && shape.radiusQ <= sim::kMaxCastRadiusQ;
            case voxel::kShapeCapsule: {
                shape.a = ReadIvec3(in);
                shape.b = ReadIvec3(in);
                shape.radiusQ = in.U16();
                if (in.Failed() || shape.radiusQ > kMaxCapsuleRadiusQ) return false;
                const glm::i64vec3 d = glm::i64vec3(shape.b) - glm::i64vec3(shape.a);
                const int64_t max = kMaxCapsuleLengthQ;
                for (int a = 0; a < 3; ++a)
                    if (d[a] > max || d[a] < -max) return false;
                return d.x * d.x + d.y * d.y + d.z * d.z <= max * max;
            }
            case voxel::kShapeBox: {
                shape.a = ReadIvec3(in);
                shape.b = ReadIvec3(in);
                if (in.Failed()) return false;
                const int64_t limit = int64_t{kMaxSyncChunkCoordinate} * voxel::kChunkSize;
                for (int a = 0; a < 3; ++a)
                    for (const int32_t v : {shape.a[a], shape.b[a]})
                        if (v < -limit || v > limit) return false;
                return true;
            }
            case voxel::kShapeCylinder:
                shape.a = ReadIvec3(in);
                shape.radiusQ = in.U16();
                shape.halfHeightQ = in.U16();
                return !in.Failed() && shape.radiusQ <= sim::kMaxCastRadiusQ;
            default: return false;
        }
    }

    void WriteZone(ByteWriter& out, const sim::Zone& z) {
        out.U16(z.zoneId);
        out.U8(static_cast<uint8_t>(z.kind));
        out.U8(z.teamMask);
        out.U32(z.startTick);
        out.U16(z.duration);
        out.I16(z.strength[0]);
        out.I16(z.strength[1]);
        WriteShape(out, z.shape);
    }

    bool ReadZone(ByteReader& in, sim::Zone& z) {
        z = sim::Zone{};
        z.zoneId = in.U16();
        const uint8_t kind = in.U8();
        z.teamMask = in.U8();
        z.startTick = in.U32();
        z.duration = in.U16();
        z.strength[0] = in.I16();
        z.strength[1] = in.I16();
        if (in.Failed() || kind == 0 || kind > kMaxZoneKind || z.duration == 0 || z.duration > kMaxZoneDuration) return false;
        z.kind = static_cast<sim::ZoneKind>(kind);
        return ReadShape(in, z.shape);
    }

    namespace {

        void WriteOp(ByteWriter& out, const sim::WorldOp& op) {
            out.U8(static_cast<uint8_t>(op.kind));
            switch (op.kind) {
                case sim::WorldOpKind::CarveSphere:
                    WriteIvec3(out, op.sphere.center);
                    out.U16(op.sphere.radius);
                    out.U16(op.power);
                    break;
                case sim::WorldOpKind::FillSphere:
                    WriteIvec3(out, op.sphere.center);
                    out.U16(op.sphere.radius);
                    out.U8(op.id);
                    break;
                case sim::WorldOpKind::SwapSphere:
                    WriteIvec3(out, op.sphere.center);
                    out.U16(op.sphere.radius);
                    out.U8(op.from);
                    out.U8(op.id);
                    break;
                case sim::WorldOpKind::FillBox:
                    WriteIvec3(out, op.min);
                    WriteIvec3(out, op.max);
                    out.U8(op.id);
                    break;
                case sim::WorldOpKind::Detach:
                case sim::WorldOpKind::ClearMask: {
                    if (op.kind == sim::WorldOpKind::Detach) out.U32(op.netId);
                    WriteIvec3(out, op.min);
                    const glm::ivec3 size = op.mask.Size();
                    out.U16(static_cast<uint16_t>(size.x));
                    out.U16(static_cast<uint16_t>(size.y));
                    out.U16(static_cast<uint16_t>(size.z));
                    if (op.kind == sim::WorldOpKind::Detach) {
                        const glm::vec3 q = QuantizeVelocity(op.velocity) * kVelocityQuanta;
                        for (int a = 0; a < 3; ++a) out.I16(static_cast<int16_t>(std::lround(q[a])));
                    }
                    WriteMaskRuns(out, op.mask);
                    break;
                }
                case sim::WorldOpKind::MapShape:
                    out.U8(op.map);
                    WriteShape(out, op.shape);
                    break;
                case sim::WorldOpKind::MapMask: {
                    out.U8(op.map);
                    out.U8(op.axis);
                    WriteIvec3(out, op.min);
                    const glm::ivec3 size = op.mask.Size();
                    out.U16(static_cast<uint16_t>(size.x));
                    out.U16(static_cast<uint16_t>(size.y));
                    out.U16(static_cast<uint16_t>(size.z));
                    WriteAxisRuns(out, op.mask, op.axis);
                    break;
                }
            }
        }

        bool ReadOp(ByteReader& in, uint8_t kind, sim::WorldOp& op, uint64_t* maskVoxelBudget) {
            op = sim::WorldOp{};
            op.kind = static_cast<sim::WorldOpKind>(kind);
            switch (op.kind) {
                case sim::WorldOpKind::CarveSphere:
                    op.sphere.center = ReadIvec3(in);
                    op.sphere.radius = in.U16();
                    op.power = in.U16();
                    return !in.Failed();
                case sim::WorldOpKind::FillSphere:
                    op.sphere.center = ReadIvec3(in);
                    op.sphere.radius = in.U16();
                    op.id = in.U8();
                    return !in.Failed();
                case sim::WorldOpKind::SwapSphere:
                    op.sphere.center = ReadIvec3(in);
                    op.sphere.radius = in.U16();
                    op.from = in.U8();
                    op.id = in.U8();
                    return !in.Failed();
                case sim::WorldOpKind::FillBox:
                    op.min = ReadIvec3(in);
                    op.max = ReadIvec3(in);
                    op.id = in.U8();
                    return !in.Failed();
                case sim::WorldOpKind::Detach:
                case sim::WorldOpKind::ClearMask: {
                    if (op.kind == sim::WorldOpKind::Detach) op.netId = in.U32();
                    op.min = ReadIvec3(in);
                    glm::ivec3 size;
                    size.x = in.U16();
                    size.y = in.U16();
                    size.z = in.U16();
                    if (op.kind == sim::WorldOpKind::Detach) {
                        for (int a = 0; a < 3; ++a) op.velocity[a] = static_cast<float>(in.I16()) / kVelocityQuanta;
                    }
                    if (in.Failed()) return false;
                    return ReadMaskRuns(in, size, op.mask, maskVoxelBudget);
                }
                case sim::WorldOpKind::MapShape:
                    op.map = in.U8();
                    return !in.Failed() && ValidMap(op.map) && ReadShape(in, op.shape);
                case sim::WorldOpKind::MapMask: {
                    op.map = in.U8();
                    op.axis = in.U8();
                    op.min = ReadIvec3(in);
                    glm::ivec3 size;
                    size.x = in.U16();
                    size.y = in.U16();
                    size.z = in.U16();
                    if (in.Failed() || !ValidMap(op.map)) return false;
                    return ReadAxisRuns(in, size, op.axis, op.mask, maskVoxelBudget);
                }
            }
            return false;
        }
    }

    namespace {
        void WriteVec3(ByteWriter& out, const glm::vec3& v) {
            out.F32(v.x);
            out.F32(v.y);
            out.F32(v.z);
        }
        glm::vec3 ReadVec3(ByteReader& in) {
            const float x = in.F32(), y = in.F32(), z = in.F32();
            return {x, y, z};
        }
        void WriteQuat(ByteWriter& out, const glm::quat& q) {
            out.F32(q.x);
            out.F32(q.y);
            out.F32(q.z);
            out.F32(q.w);
        }
        glm::quat ReadQuat(ByteReader& in) {
            const float x = in.F32(), y = in.F32(), z = in.F32(), w = in.F32();
            return glm::quat(w, x, y, z);
        }
        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        bool Finite(const glm::quat& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

        void WriteBox(ByteWriter& out, const scene::MechanismBox& b) {
            for (int a = 0; a < 3; ++a) out.U8(static_cast<uint8_t>(b.min[a]));
            for (int a = 0; a < 3; ++a) out.U8(static_cast<uint8_t>(b.max[a]));
        }
        scene::MechanismBox ReadBox(ByteReader& in) {
            scene::MechanismBox b;
            for (int a = 0; a < 3; ++a) b.min[a] = in.U8();
            for (int a = 0; a < 3; ++a) b.max[a] = in.U8();
            return b;
        }
        void WriteSegment(ByteWriter& out, const scene::MechanismSegment& s) {
            out.U32(s.startTick);
            out.I16(s.startPos);
            out.I16(s.targetPos);
            out.U16(s.durationTicks);
            out.U8(s.flags);
        }
        scene::MechanismSegment ReadSegment(ByteReader& in) {
            scene::MechanismSegment s;
            s.startTick = in.U32();
            s.startPos = in.I16();
            s.targetPos = in.I16();
            s.durationTicks = in.U16();
            s.flags = in.U8();
            return s;
        }

        /// The 63 B desc and the 24 B state of a mechanism spawn, then its segments ([mech-1] card 8).
        void WriteMechanism(ByteWriter& out, const MechanismState& m) {
            const scene::MechanismDesc& d = m.desc;
            out.U8(static_cast<uint8_t>(d.kind));
            out.U8(d.flags);
            for (int a = 0; a < 3; ++a) out.I16(static_cast<int16_t>(d.pivotQ[a]));
            out.U8(d.axis);
            out.I16(d.minPos);
            out.I16(d.maxPos);
            out.U16(d.openTicks);
            out.U16(d.closeTicks);
            out.U16(d.autoReturnTicks);
            WriteBox(out, d.useBox);
            WriteBox(out, d.blockBox);
            WriteBox(out, d.hingeBox);
            out.U8(d.pushAxis);
            out.U8(d.anchorCount);
            for (const glm::ivec3& anchor : d.anchors)
                for (int a = 0; a < 3; ++a) out.I32(anchor[a]);
            out.U8(static_cast<uint8_t>(m.segments.size()));
            for (int a = 0; a < 3; ++a) out.I16(static_cast<int16_t>(m.modelOffset[a]));
            out.U32(m.linkedNetId);
            for (int a = 0; a < 3; ++a) out.I32(m.spawnVoxel[a]);
            out.U8(m.quarterTurns);
            for (const scene::MechanismSegment& s : m.segments) WriteSegment(out, s);
        }

        bool ReadMechanism(ByteReader& in, const glm::ivec3& modelSize, MechanismState& m) {
            scene::MechanismDesc& d = m.desc;
            const uint8_t kind = in.U8();
            d.flags = in.U8();
            for (int a = 0; a < 3; ++a) d.pivotQ[a] = in.I16();
            d.axis = in.U8();
            d.minPos = in.I16();
            d.maxPos = in.I16();
            d.openTicks = in.U16();
            d.closeTicks = in.U16();
            d.autoReturnTicks = in.U16();
            d.useBox = ReadBox(in);
            d.blockBox = ReadBox(in);
            d.hingeBox = ReadBox(in);
            d.pushAxis = in.U8();
            d.anchorCount = in.U8();
            for (glm::ivec3& anchor : d.anchors)
                for (int a = 0; a < 3; ++a) anchor[a] = in.I32();
            const uint8_t count = in.U8();
            for (int a = 0; a < 3; ++a) m.modelOffset[a] = in.I16();
            m.linkedNetId = in.U32();
            for (int a = 0; a < 3; ++a) m.spawnVoxel[a] = in.I32();
            m.quarterTurns = in.U8();
            if (in.Failed() || kind < 1 || kind > 3 || count == 0 || count > scene::kMechanismSegments || m.quarterTurns > 3) return false;
            d.kind = static_cast<scene::MechanismKind>(kind);
            if (!sim::ValidateMechanismDesc(d, modelSize)) return false;
            m.segments.clear();
            for (uint8_t i = 0; i < count; ++i) m.segments.push_back(ReadSegment(in));
            return !in.Failed();
        }

        struct EventWriter {
            ByteWriter& out;
            void operator()(const sim::WorldOp& op) const { WriteOp(out, op); }
            void operator()(const EntitySpawn& spawn) const {
                out.U8(static_cast<uint8_t>(EventKind::EntitySpawn));
                out.U32(spawn.netId);
                out.U8(static_cast<uint8_t>(spawn.kind));
                if (spawn.kind == EntityKind::Light) {
                    out.F64(spawn.light.position.x);
                    out.F64(spawn.light.position.y);
                    out.F64(spawn.light.position.z);
                    out.F32(spawn.light.color.r);
                    out.F32(spawn.light.color.g);
                    out.F32(spawn.light.color.b);
                    out.F32(spawn.light.intensity);
                    out.F32(spawn.light.radius);
                    return;
                }
                const ObjectSpawn& o = spawn.object;
                WriteVec3(out, o.position);
                WriteQuat(out, o.rotation);
                WriteVec3(out, o.linearVelocity);
                WriteVec3(out, o.angularVelocity);
                out.U8(o.motion);
                out.U8(o.flags);
                WriteVolumeRuns(out, o.volume);
                if ((o.flags & kObjectHeld) != 0) {
                    out.U8(o.holder);
                    out.U16(o.relYaw);
                    out.U32(o.holdSince);
                }
                if (spawn.kind == EntityKind::Mechanism) WriteMechanism(out, spawn.mechanism);
            }
            void operator()(const MechanismMove& m) const {
                out.U8(static_cast<uint8_t>(EventKind::MechanismMove));
                out.U32(m.netId);
                WriteSegment(out, m.segment);
            }
            void operator()(const EntityHold& h) const {
                out.U8(static_cast<uint8_t>(EventKind::EntityHold));
                out.U32(h.netId);
                out.U8(h.holder);
                out.U16(h.relYaw);
                out.U32(h.tick);
            }
            void operator()(const EntityRelease& r) const {
                out.U8(static_cast<uint8_t>(EventKind::EntityRelease));
                out.U32(r.netId);
                out.U32(r.tick);
                out.U8(r.reason);
                WriteVec3(out, r.position);
                WriteQuat(out, r.rotation);
                const glm::vec3 v = QuantizeVelocity(r.velocity) * kVelocityQuanta;
                for (int a = 0; a < 3; ++a) out.I16(static_cast<int16_t>(std::lround(v[a])));
            }
            void operator()(const EntityDespawn& d) const {
                out.U8(static_cast<uint8_t>(EventKind::EntityDespawn));
                out.U32(d.netId);
                out.U8(static_cast<uint8_t>(d.reason));
            }
            void operator()(const EntityRest& r) const {
                out.U8(static_cast<uint8_t>(EventKind::EntityRest));
                out.U32(r.netId);
                out.U32(r.tick);
                WriteVec3(out, r.position);
                WriteQuat(out, r.rotation);
            }
            void operator()(const EntityWake& k) const {
                out.U8(static_cast<uint8_t>(EventKind::EntityWake));
                out.U32(k.netId);
                out.U32(k.tick);
            }
            void operator()(const ZoneStartEvent& e) const {
                out.U8(static_cast<uint8_t>(EventKind::ZoneStart));
                WriteZone(out, e.zone);
            }
            void operator()(const ZoneCancelEvent& e) const {
                out.U8(static_cast<uint8_t>(EventKind::ZoneCancel));
                out.U16(e.zoneId);
                out.U32(e.endTick);
            }
            void operator()(const CastFx& fx) const {
                out.U8(static_cast<uint8_t>(EventKind::CastFx));
                out.U8(fx.slot);
                out.U8(fx.spell);
                WriteIvec3(out, fx.originQ);
                WriteIvec3(out, fx.hitQ);
                out.U8(fx.face);
                out.U32(fx.tick);
            }
            void operator()(const ObjectModel& m) const {
                out.U8(static_cast<uint8_t>(EventKind::ObjectModel));
                out.U32(m.netId);
                out.I16(static_cast<int16_t>(m.offset.x));
                out.I16(static_cast<int16_t>(m.offset.y));
                out.I16(static_cast<int16_t>(m.offset.z));
                out.U8(m.motion);
                WriteVolumeRuns(out, m.volume);
            }
        };
    }

    void WriteEvent(ByteWriter& out, const Event& event) { std::visit(EventWriter{out}, event); }

    bool ReadEvent(ByteReader& in, Event& event, uint64_t* maskVoxelBudget) {
        const uint8_t kind = in.U8();
        if (in.Failed()) return false;
        if (kind >= 1 && kind <= static_cast<uint8_t>(sim::WorldOpKind::MapMask)) {
            sim::WorldOp op;
            if (!ReadOp(in, kind, op, maskVoxelBudget)) return false;
            event = std::move(op);
            return true;
        }
        switch (static_cast<EventKind>(kind)) {
            case EventKind::EntitySpawn: {
                EntitySpawn spawn;
                spawn.netId = in.U32();
                const uint8_t entity = in.U8();
                if (entity == static_cast<uint8_t>(EntityKind::Light)) {
                    spawn.kind = EntityKind::Light;
                    spawn.light.position.x = in.F64();
                    spawn.light.position.y = in.F64();
                    spawn.light.position.z = in.F64();
                    spawn.light.color.r = in.F32();
                    spawn.light.color.g = in.F32();
                    spawn.light.color.b = in.F32();
                    spawn.light.intensity = in.F32();
                    spawn.light.radius = in.F32();
                    if (in.Failed()) return false;
                } else if (entity == static_cast<uint8_t>(EntityKind::Object) || entity == static_cast<uint8_t>(EntityKind::Mechanism)) {
                    spawn.kind = static_cast<EntityKind>(entity);
                    ObjectSpawn& o = spawn.object;
                    o.position = ReadVec3(in);
                    o.rotation = ReadQuat(in);
                    o.linearVelocity = ReadVec3(in);
                    o.angularVelocity = ReadVec3(in);
                    o.motion = in.U8();
                    o.flags = in.U8();
                    if (in.Failed() || o.motion > 2 || !Finite(o.position) || !Finite(o.rotation) || !Finite(o.linearVelocity) ||
                        !Finite(o.angularVelocity))
                        return false;
                    if (!ReadVolumeRuns(in, o.volume, maskVoxelBudget)) return false;
                    if ((o.flags & ~(kObjectResting | kObjectHeld)) != 0) return false;
                    if ((o.flags & kObjectHeld) != 0) {
                        if ((o.flags & kObjectResting) != 0) return false;   // a held body is never resting
                        o.holder = in.U8();
                        o.relYaw = in.U16();
                        o.holdSince = in.U32();
                        if (in.Failed()) return false;
                    }
                    if (spawn.kind == EntityKind::Mechanism && !ReadMechanism(in, o.volume.Size(), spawn.mechanism)) return false;
                } else {
                    return false;
                }
                event = std::move(spawn);
                return true;
            }
            case EventKind::EntityDespawn: {
                EntityDespawn d;
                d.netId = in.U32();
                const uint8_t reason = in.U8();
                if (in.Failed() || reason < 1 || reason > 3) return false;
                d.reason = static_cast<scene::DespawnReason>(reason);
                event = d;
                return true;
            }
            case EventKind::EntityRest: {
                EntityRest r;
                r.netId = in.U32();
                r.tick = in.U32();
                r.position = ReadVec3(in);
                r.rotation = ReadQuat(in);
                if (in.Failed() || !Finite(r.position) || !Finite(r.rotation)) return false;
                event = r;
                return true;
            }
            case EventKind::EntityWake: {
                EntityWake k;
                k.netId = in.U32();
                k.tick = in.U32();
                if (in.Failed()) return false;
                event = k;
                return true;
            }
            case EventKind::MechanismMove: {
                MechanismMove m;
                m.netId = in.U32();
                m.segment = ReadSegment(in);
                if (in.Failed()) return false;
                event = m;
                return true;
            }
            case EventKind::EntityHold: {
                EntityHold h;
                h.netId = in.U32();
                h.holder = in.U8();
                h.relYaw = in.U16();
                h.tick = in.U32();
                if (in.Failed()) return false;
                event = h;
                return true;
            }
            case EventKind::EntityRelease: {
                EntityRelease r;
                r.netId = in.U32();
                r.tick = in.U32();
                r.reason = in.U8();
                r.position = ReadVec3(in);
                r.rotation = ReadQuat(in);
                for (int a = 0; a < 3; ++a) r.velocity[a] = static_cast<float>(in.I16()) / kVelocityQuanta;
                if (in.Failed() || r.reason == 0 || r.reason > sim::kMaxHoldEnd || !Finite(r.position) || !Finite(r.rotation)) return false;
                event = r;
                return true;
            }
            case EventKind::ZoneStart: {
                ZoneStartEvent e;
                if (!ReadZone(in, e.zone)) return false;
                event = e;
                return true;
            }
            case EventKind::ZoneCancel: {
                ZoneCancelEvent e;
                e.zoneId = in.U16();
                e.endTick = in.U32();
                if (in.Failed()) return false;
                event = e;
                return true;
            }
            case EventKind::CastFx: {
                CastFx fx;
                fx.slot = in.U8();
                fx.spell = in.U8();
                fx.originQ = ReadIvec3(in);
                fx.hitQ = ReadIvec3(in);
                fx.face = in.U8();
                fx.tick = in.U32();
                if (in.Failed() || fx.spell > kMaxSpellId || fx.face > 5) return false;
                event = fx;
                return true;
            }
            case EventKind::ObjectModel: {
                ObjectModel m;
                m.netId = in.U32();
                m.offset.x = in.I16();
                m.offset.y = in.I16();
                m.offset.z = in.I16();
                m.motion = in.U8();
                if (in.Failed() || m.motion > 2) return false;
                if (!ReadVolumeRuns(in, m.volume, maskVoxelBudget)) return false;
                event = std::move(m);
                return true;
            }
        }
        return false;
    }

    size_t EventBytes(const Event& event) {
        ByteWriter w;
        WriteEvent(w, event);
        return w.Size();
    }

    // --- RegionDesc -------------------------------------------------------------------------------

    namespace {
        // The material ids in a fixed order (wire format).
        template <typename F>
        void ForEachMaterialId(voxel::WorldMaterials& m, F&& f) {
            for (uint8_t* id : {&m.grass, &m.grassDry, &m.dirt, &m.stone, &m.stoneDark, &m.sand, &m.snow, &m.water, &m.wood, &m.leaves,
                                &m.leavesAutumn, &m.planks, &m.brick, &m.iron, &m.gold, &m.crystal, &m.lava, &m.torch, &m.glass, &m.bedrock})
                f(*id);
        }
    }

    void WriteRegion(ByteWriter& out, const sim::RegionDesc& region) {
        out.U32(static_cast<uint32_t>(region.templateId));
        out.U32(region.seed);
        out.I32(region.playMinChunk.x);
        out.I32(region.playMinChunk.y);
        out.I32(region.playMaxChunk.x);
        out.I32(region.playMaxChunk.y);
        out.F32(region.heightRange.x);
        out.F32(region.heightRange.y);
        out.U8(static_cast<uint8_t>(region.generator));
        out.I32(region.flatGroundVoxels);
        out.U64(region.landHash);
        out.U64(region.landSourceHash);
        const voxel::TerrainSettings& t = region.terrain;
        out.U32(t.seed);
        out.F32(t.voxelSize);
        voxel::WorldMaterials ids = t.materials;
        ForEachMaterialId(ids, [&out](uint8_t& id) { out.U8(id); });
        for (const float f : {t.seaLevel, t.baseHeight, t.continentHeight, t.hillHeight, t.mountainHeight, t.horizontalScale, t.snowLine})
            out.F32(f);
        out.U8(t.caves ? 1 : 0);
        out.F32(t.caveMaxDepth);
        out.U8(t.trees ? 1 : 0);
        out.F32(t.treeSpacing);
        out.F32(t.treeDensity);
        out.U8(t.skipClassifiedRows ? 1 : 0);
    }

    bool ReadRegion(ByteReader& in, sim::RegionDesc& region) {
        region = sim::RegionDesc{};
        region.templateId = static_cast<sim::RegionTemplateId>(in.U32());
        region.seed = in.U32();
        region.playMinChunk.x = in.I32();
        region.playMinChunk.y = in.I32();
        region.playMaxChunk.x = in.I32();
        region.playMaxChunk.y = in.I32();
        region.heightRange.x = in.F32();
        region.heightRange.y = in.F32();
        region.generator = static_cast<sim::RegionGenerator>(in.U8());
        region.flatGroundVoxels = in.I32();
        region.landHash = in.U64();
        region.landSourceHash = in.U64();
        voxel::TerrainSettings& t = region.terrain;
        t.seed = in.U32();
        t.voxelSize = in.F32();
        ForEachMaterialId(t.materials, [&in](uint8_t& id) { id = in.U8(); });
        for (float* f : {&t.seaLevel, &t.baseHeight, &t.continentHeight, &t.hillHeight, &t.mountainHeight, &t.horizontalScale, &t.snowLine})
            *f = in.F32();
        t.caves = in.U8() != 0;
        t.caveMaxDepth = in.F32();
        t.trees = in.U8() != 0;
        t.treeSpacing = in.F32();
        t.treeDensity = in.F32();
        t.skipClassifiedRows = in.U8() != 0;
        return !in.Failed();
    }

    void WritePlayerTuning(ByteWriter& out, const sim::PlayerTuning& t) {
        for (const float f : {t.walkSpeed, t.runSpeed, t.jumpSpeed, t.height, t.radius, t.eyeHeight, t.maxSlopeDegrees, t.stepHeight})
            out.F32(f);
        out.U8(t.castCooldownTicks);
        for (const float f : {t.castRadius, t.castPower, t.castRange}) out.F32(f);
        out.U16(t.pushDecayGround);
        out.U16(t.pushDecayAir);
    }

    bool ReadPlayerTuning(ByteReader& in, sim::PlayerTuning& t) {
        t = sim::PlayerTuning{};
        for (float* f : {&t.walkSpeed, &t.runSpeed, &t.jumpSpeed, &t.height, &t.radius, &t.eyeHeight, &t.maxSlopeDegrees, &t.stepHeight})
            *f = in.F32();
        t.castCooldownTicks = in.U8();
        for (float* f : {&t.castRadius, &t.castPower, &t.castRange}) *f = in.F32();
        t.pushDecayGround = in.U16();
        t.pushDecayAir = in.U16();
        return !in.Failed();
    }
}
