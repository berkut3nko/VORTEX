module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

export module vortex.net:wire;

import vortex.voxel;
import vortex.scene;
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
        EntitySpawn = 32,   ///< A server entity appears: a light (stage 3) or an object (stage 6).
        EntityDespawn = 33, ///< A replicated object is gone (stage 6).
        EntityRest = 34,    ///< A replicated object came to rest: its pose (stage 6).
        ObjectModel = 35,   ///< A replicated object's model changed (Scene::Damage; stage 6).
    };
    /// @brief What an EntitySpawn creates. Wire format.
    enum class EntityKind : uint8_t {
        Light = 1,
        Object = 2,         ///< A voxel object with a body: a crate, a piece (netId 0: client rubble).
    };

    /// @brief Most voxels of one object model a decoder accepts per axis (as masks).
    inline constexpr int32_t kMaxModelAxis = 1024;

    /// @brief A replicated voxel object as it appears (EntitySpawn of kind Object). The model's
    /// palette is the world palette (voxel::kWorldPalette): the only one both sides share.
    struct ObjectSpawn {
        glm::vec3 position{0.0f};            ///< World metres of the model origin (the body's shape origin).
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 linearVelocity{0.0f};
        glm::vec3 angularVelocity{0.0f};
        uint8_t motion = 2;                  ///< physics::MotionType of the server body
        uint8_t flags = 0;                   ///< kObjectResting: it rests already (no EntityRest follows until it moved)
        voxel::VoxelVolume volume;
    };
    inline constexpr uint8_t kObjectResting = 1;

    /// @brief `EntitySpawn`: a template light (stage 3), or an object (stage 6; `netId` 0 = a
    /// fragment every client makes into its own rubble). Not hashed; a client keeps NetId -> entity.
    struct EntitySpawn {
        uint32_t netId = 0;
        EntityKind kind = EntityKind::Light;
        sim::TemplateLight light;
        ObjectSpawn object;
        friend bool operator==(const EntitySpawn& a, const EntitySpawn& b);
    };

    /// @brief `EntityDespawn{netId, reason}` (stage 6): made by the server's on_destroy observer.
    struct EntityDespawn {
        uint32_t netId = 0;
        scene::DespawnReason reason = scene::DespawnReason::Destroyed;
        friend bool operator==(const EntityDespawn&, const EntityDespawn&) = default;
    };

    /// @brief `EntityRest{netId, tick, pose}` (stage 6): the object fell asleep on the server at
    /// `tick`; a client puts its copy there (Layer::Dynamic: players collide with it) and drops
    /// snapshot samples of `tick` or older. The rest ends with a newer sample.
    struct EntityRest {
        uint32_t netId = 0;
        uint32_t tick = 0;
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        friend bool operator==(const EntityRest&, const EntityRest&) = default;
    };

    /// @brief `ObjectModel{netId, offset, motion, model}` (stage 6): the survivor of a damage. Its
    /// origin moved by `offset` voxels of the old model (rotated with it); a client re-bases its
    /// copy and its samples.
    struct ObjectModel {
        uint32_t netId = 0;
        glm::ivec3 offset{0};
        uint8_t motion = 2;
        voxel::VoxelVolume volume;
        friend bool operator==(const ObjectModel& a, const ObjectModel& b);
    };

    /// @brief One event of the opSeq stream: a world operation or an entity event.
    using Event = std::variant<sim::WorldOp, EntitySpawn, EntityDespawn, EntityRest, ObjectModel>;

    /// @brief A volume's ids as (run length, id) pairs in VoxelVolume::Index order after its size
    /// (3 x u16); self-delimiting.
    void WriteVolumeRuns(ByteWriter& out, const voxel::VoxelVolume& volume);
    /// @brief The inverse; false for a size over kMaxModelAxis, over `voxelBudget` (reduced by the
    /// volume, checked before allocating), empty runs, or runs that do not add up.
    bool ReadVolumeRuns(ByteReader& in, voxel::VoxelVolume& volume, uint64_t* voxelBudget = nullptr);
    /// @brief Hash of a volume's size and ids (the model hash of the entity check).
    uint64_t VolumeHash(const voxel::VoxelVolume& volume);

    /// Detach velocity on the wire: int16 per axis in 1/256 m/s (+-128 m/s; presentation only,
    /// ApplyOp never reads it).
    inline constexpr float kVelocityQuanta = 256.0f;
    /// @brief The velocity a client gets for `v` (what the server's Detach velocity becomes).
    glm::vec3 QuantizeVelocity(const glm::vec3& v);

    /**
     * @brief Encodes one event (docs/design/networking.md "World operations": carve / swap 17 B,
     * fill 16 B, box 26 B, Detach 29 B + mask runs, ClearMask 19 B + mask runs, light 50 B; stage
     * 6: object spawn 64 B + model runs, despawn 6 B, rest 37 B, model 19 B + model runs).
     */
    void WriteEvent(ByteWriter& out, const Event& event);
    /// @brief Decodes one event; false for an unknown kind, a malformed or oversized mask or model
    /// (or one over `maskVoxelBudget`, see ReadMaskRuns: masks and models share it), a non-finite
    /// pose, or a read past the end.
    bool ReadEvent(ByteReader& in, Event& event, uint64_t* maskVoxelBudget = nullptr);
    /// @brief Bytes WriteEvent writes for `event`.
    size_t EventBytes(const Event& event);

    /// @brief RegionDesc on the wire: integers, raw float bits (the voxel size and every terrain tunable).
    void WriteRegion(ByteWriter& out, const sim::RegionDesc& region);
    /// @brief Decodes a RegionDesc. Does not validate it (sim::IsValid does).
    bool ReadRegion(ByteReader& in, sim::RegionDesc& region);

    /// @brief sim::PlayerTuning on the wire: raw float bits and the cooldown (45 bytes).
    void WritePlayerTuning(ByteWriter& out, const sim::PlayerTuning& tuning);
    /// @brief Decodes a PlayerTuning. Does not validate it (sim::IsValid does).
    bool ReadPlayerTuning(ByteReader& in, sim::PlayerTuning& tuning);
}
