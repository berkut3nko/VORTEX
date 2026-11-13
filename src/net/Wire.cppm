module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

export module vortex.net:wire;

import vortex.voxel;
import vortex.sim;

export namespace vortex::net {

    /// @brief Appends little-endian integers, raw float bits and LEB128 numbers to a byte string.
    class ByteWriter {
    public:
        void U8(uint8_t v) { m_Bytes.push_back(v); }
        void U16(uint16_t v);
        void U32(uint32_t v);
        void U64(uint64_t v);
        void I16(int16_t v) { U16(static_cast<uint16_t>(v)); }
        void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
        void F32(float v);                       ///< The raw bits: decoded bit for bit.
        void F64(double v);
        void VarU(uint64_t v);                   ///< LEB128.
        void Bytes(std::span<const uint8_t> bytes) { m_Bytes.insert(m_Bytes.end(), bytes.begin(), bytes.end()); }

        size_t Size() const { return m_Bytes.size(); }
        const std::vector<uint8_t>& Data() const { return m_Bytes; }
        std::vector<uint8_t> Take() { return std::move(m_Bytes); }

    private:
        std::vector<uint8_t> m_Bytes;
    };

    /**
     * @brief Reads what ByteWriter wrote. Input from the network: a read past the end (or a
     * malformed LEB128 number) sets Failed() and returns 0 from then on; callers check once at the
     * end and must never let a value read from a failed reader size an allocation unchecked.
     */
    class ByteReader {
    public:
        explicit ByteReader(std::span<const uint8_t> bytes) : m_Bytes(bytes) {}
        uint8_t U8();
        uint16_t U16();
        uint32_t U32();
        uint64_t U64();
        int16_t I16() { return static_cast<int16_t>(U16()); }
        int32_t I32() { return static_cast<int32_t>(U32()); }
        float F32();
        double F64();
        uint64_t VarU();
        std::span<const uint8_t> Bytes(size_t count);

        bool Failed() const { return m_Failed; }
        void Fail() { m_Failed = true; }
        size_t Remaining() const { return m_Failed ? 0 : m_Bytes.size() - m_Pos; }
        bool AtEnd() const { return !m_Failed && m_Pos == m_Bytes.size(); }

    private:
        std::span<const uint8_t> m_Bytes;
        size_t m_Pos = 0;
        bool m_Failed = false;
    };

    /// Mask dimensions a decoder accepts per axis, and in all (a Detach of the largest island box
    /// plus its crumb margin is at most about 230 voxels per axis). kMaxMaskVoxels is also the most
    /// one EventBatch may hold in all (the server splits batches there): 16 MiB of bits.
    inline constexpr int32_t kMaxMaskAxis = 1024;
    inline constexpr uint64_t kMaxMaskVoxels = 1ull << 27;

    /// @brief A mask as alternating runs of clear / set bits in VoxelMask::Index order, LEB128,
    /// starting with a (possibly empty) clear run; self-delimiting given the mask size.
    void WriteMaskRuns(ByteWriter& out, const voxel::VoxelMask& mask);
    /// @brief The inverse; false (mask left empty) when the runs do not add up to `size` exactly.
    /// With `voxelBudget`, also false when the mask holds more voxels than are left in it (checked
    /// before the mask is allocated); the budget is reduced by the mask's voxels.
    bool ReadMaskRuns(ByteReader& in, const glm::ivec3& size, voxel::VoxelMask& mask, uint64_t* voxelBudget = nullptr);

    /// @brief Kinds of events in the opSeq stream. 1-6 are sim::WorldOpKind; wire format.
    enum class EventKind : uint8_t {
        EntitySpawn = 32,   ///< A server entity appears (stage 3: lights only; stage 6 adds objects).
    };
    /// @brief What an EntitySpawn creates. Wire format.
    enum class EntityKind : uint8_t {
        Light = 1,
    };

    /// @brief `EntitySpawn` of kind Light (docs/design/networking.md "Region start-up": the
    /// template's lights). Not hashed; a client keeps NetId -> entity.
    struct EntitySpawn {
        uint32_t netId = 0;
        EntityKind kind = EntityKind::Light;
        sim::TemplateLight light;
        friend bool operator==(const EntitySpawn&, const EntitySpawn&) = default;
    };

    /// @brief One event of the opSeq stream: a world operation or an entity event.
    using Event = std::variant<sim::WorldOp, EntitySpawn>;

    /// Detach velocity on the wire: int16 per axis in 1/256 m/s (+-128 m/s; presentation only,
    /// ApplyOp never reads it).
    inline constexpr float kVelocityQuanta = 256.0f;
    /// @brief The velocity a client gets for `v` (what the server's Detach velocity becomes).
    glm::vec3 QuantizeVelocity(const glm::vec3& v);

    /**
     * @brief Encodes one event (docs/design/networking.md "World operations": carve / swap 17 B,
     * fill 16 B, box 26 B, Detach 29 B + mask runs, ClearMask 19 B + mask runs, light 50 B).
     */
    void WriteEvent(ByteWriter& out, const Event& event);
    /// @brief Decodes one event; false for an unknown kind, a malformed or oversized mask (or one
    /// over `maskVoxelBudget`, see ReadMaskRuns), or a read past the end.
    bool ReadEvent(ByteReader& in, Event& event, uint64_t* maskVoxelBudget = nullptr);
    /// @brief Bytes WriteEvent writes for `event`.
    size_t EventBytes(const Event& event);

    /// @brief RegionDesc on the wire: integers, raw float bits (the voxel size and every terrain tunable).
    void WriteRegion(ByteWriter& out, const sim::RegionDesc& region);
    /// @brief Decodes a RegionDesc. Does not validate it (sim::IsValid does).
    bool ReadRegion(ByteReader& in, sim::RegionDesc& region);
}
