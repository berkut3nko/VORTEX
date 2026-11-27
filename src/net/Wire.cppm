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
        EntityWake = 36,    ///< A replicated object at rest started to move (review of stage 6).
        MechanismMove = 37, ///< [mech-1] a mechanism took a new segment (a use, a link, a jam, a release).
        ZoneStart = 38,     ///< [mag-2] a zone that acts on players from its start tick (the lead ahead).
        ZoneCancel = 39,    ///< [mag-2] a zone's truncation: it ends at endTick at the latest.
        CastFx = 40,        ///< [mag-2] a spell was cast: presentation only (never in sim or hashes).
        EntityHold = 41,    ///< [mech-1] a player picked a replicated body up.
        EntityRelease = 42, ///< [mech-1] a held body was let go: its pose and velocity.
    };
    /// @brief What an EntitySpawn creates. Wire format.
    enum class EntityKind : uint8_t {
        Light = 1,
        Object = 2,         ///< A voxel object with a body: a crate, a piece (netId 0: client rubble).
        Mechanism = 3,      ///< [mech-1] an object that is a mechanism: the object's bytes, then its desc and track.
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
        /// [mech-1] kObjectHeld: a player holds it (7 B after the runs; never together with kObjectResting).
        uint8_t holder = 0;
        uint16_t relYaw = 0;
        uint32_t holdSince = 0;
    };
    inline constexpr uint8_t kObjectResting = 1;
    inline constexpr uint8_t kObjectHeld = 2;

    /// @brief [mech-1] What a client needs to pose a mechanism itself: its desc, spawn and segment ring (oldest first,
    /// 1..8). Desc 63 B, state 24 B, then 11 B per segment, after the object's bytes.
    struct MechanismState {
        scene::MechanismDesc desc;
        glm::ivec3 spawnVoxel{0};
        uint8_t quarterTurns = 0;
        glm::ivec3 modelOffset{0};
        uint32_t linkedNetId = 0;
        std::vector<scene::MechanismSegment> segments;
        friend bool operator==(const MechanismState&, const MechanismState&) = default;
    };

    /// @brief `EntitySpawn`: a template light (stage 3), or an object (stage 6; `netId` 0 = a
    /// fragment every client makes into its own rubble). Not hashed; a client keeps NetId -> entity.
    struct EntitySpawn {
        uint32_t netId = 0;
        EntityKind kind = EntityKind::Light;
        sim::TemplateLight light;
        ObjectSpawn object;
        MechanismState mechanism;            ///< kind == Mechanism ([mech-1])
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

    /// @brief `EntityWake{netId, tick}` (review of stage 6): an object that rested woke on the server
    /// after the physics of `tick`. A client's copy leaves Layer::Dynamic at the same point of the
    /// stream as the server's body (what blocks a player is a function of the opSeq, as for a rest)
    /// and holds still until its samples come; samples of the rest's tick or older stay refused.
    struct EntityWake {
        uint32_t netId = 0;
        uint32_t tick = 0;
        friend bool operator==(const EntityWake&, const EntityWake&) = default;
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

    /// @brief `MechanismMove{netId, segment}` ([mech-1], 16 B): the mechanism's newest segment.
    struct MechanismMove {
        uint32_t netId = 0;
        scene::MechanismSegment segment;
        friend bool operator==(const MechanismMove&, const MechanismMove&) = default;
    };

    /// @brief `EntityHold{netId, holder, relYaw, tick}` ([mech-1], 12 B): a player picked the body up at `tick`.
    struct EntityHold {
        uint32_t netId = 0;
        uint8_t holder = 0;
        uint16_t relYaw = 0;
        uint32_t tick = 0;
        friend bool operator==(const EntityHold&, const EntityHold&) = default;
    };

    /// @brief `EntityRelease{netId, tick, reason, pose, velocity}` ([mech-1], 44 B): the hold ended (sim::HoldEnd); the
    /// velocity travels as 3 x int16 in 1/256 m/s.
    struct EntityRelease {
        uint32_t netId = 0;
        uint32_t tick = 0;
        uint8_t reason = 0;
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f};
        friend bool operator==(const EntityRelease&, const EntityRelease&) = default;
    };

    /// @brief `ZoneStart` ([mag-2], 30 / 42 / 40 / 32 B for a sphere / capsule / box / cylinder): zoneId u16, kind u8,
    /// teamMask u8, startTick u32, duration u16, strength i16 x 2, then the shape as a MapShape writes it.
    struct ZoneStartEvent {
        sim::Zone zone;
        friend bool operator==(const ZoneStartEvent&, const ZoneStartEvent&) = default;
    };
    /// @brief `ZoneCancel{zoneId, endTick}` ([mag-2], 7 B): the zone acts on no tick from `endTick` on.
    struct ZoneCancelEvent {
        uint16_t zoneId = 0;
        uint32_t endTick = 0;
        friend bool operator==(const ZoneCancelEvent&, const ZoneCancelEvent&) = default;
    };
    /// @brief `CastFx` ([mag-2], 34 B + 12 B per point): a spell's projectile, its impact and its drawn shape for the
    /// clients to draw. `slot` is the caster's slot + 1 (0: a server script), `originQ` and `hitQ` in q (1/16 voxel),
    /// `face` axis * 2 + (sign < 0), `tick` T_run; [mag-8] `flightTicks` (the effects land at tick + flightTicks, at most
    /// kMaxFlightTicks) and the points of a shape drawn along a gesture (count u8, i32 x 3 q each, at most 16).
    struct CastFx {
        uint8_t slot = 0;
        uint8_t spell = 0;
        glm::ivec3 originQ{0}, hitQ{0};
        uint8_t face = 0;
        uint32_t tick = 0;
        uint8_t flightTicks = 0;
        std::vector<glm::ivec3> pointsQ;
        static constexpr uint8_t kMaxFlightTicks = 60;
        static constexpr size_t kMaxPoints = 16;
        friend bool operator==(const CastFx&, const CastFx&) = default;
    };

    /// @brief One event of the opSeq stream: a world operation, an entity event or a zone event.
    using Event = std::variant<sim::WorldOp, EntitySpawn, EntityDespawn, EntityRest, ObjectModel, EntityWake, MechanismMove, EntityHold,
                               EntityRelease, ZoneStartEvent, ZoneCancelEvent, CastFx>;

    /// Decoder limits of the magic events and ops (magic.md "Wire values").
    inline constexpr uint16_t kMaxZoneDuration = 600;
    inline constexpr int32_t kMaxCapsuleLengthQ = 8192;     ///< 64 m at 0.125 m
    inline constexpr uint16_t kMaxCapsuleRadiusQ = 1024;
    /// Zone kinds a decoder accepts (1-9: Impulse .. Rune; the later stages give the reserved ones behaviour).
    inline constexpr uint8_t kMaxZoneKind = 9;
    /// Spell ids a decoder accepts in a CastFx (0-15 the matrix, 16 and 17 the drills).
    inline constexpr uint8_t kMaxSpellId = 17;
    /// @brief A shape as MapShape and ZoneStart write it: kind u8, then sphere centre i32 x 3 q and radius u16 q,
    /// capsule A and B i32 x 3 q and radius u16 q, box min and max i32 x 3 voxels, cylinder centre i32 x 3 q, radius u16
    /// q and half height u16 q.
    void WriteShape(ByteWriter& out, const voxel::ShapeQ& shape);
    /// @brief False for a kind over 3, a sphere or cylinder radius over sim::kMaxCastRadiusQ, a capsule radius over
    /// kMaxCapsuleRadiusQ or longer than kMaxCapsuleLengthQ, a box corner beyond kMaxSyncChunkCoordinate chunks, or a
    /// read past the end.
    bool ReadShape(ByteReader& in, voxel::ShapeQ& shape);
    /// @brief A zone as ZoneStart writes it after its kind byte (29 / 41 / 39 / 31 B by shape).
    void WriteZone(ByteWriter& out, const sim::Zone& zone);
    /// @brief False for a zone kind of 0 or over kMaxZoneKind, a duration of 0 or over kMaxZoneDuration, a shape ReadShape
    /// refuses, or a read past the end.
    bool ReadZone(ByteReader& in, sim::Zone& zone);

    /// @brief The event of a sim::HoldChange for the body `netId`: EntityHold or EntityRelease ([mech-1]).
    Event HoldEvent(const sim::HoldChange& change, uint32_t netId);

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
     * 6: object spawn 60 B, despawn 6 B, rest 37 B, model 12 B, each object or model + its runs:
     * 6 B of size and (LEB128 run, id) pairs; wake 9 B).
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

    /// @brief sim::PlayerTuning on the wire: raw float bits, the cooldown and the push decays (49 bytes).
    void WritePlayerTuning(ByteWriter& out, const sim::PlayerTuning& tuning);
    /// @brief Decodes a PlayerTuning. Does not validate it (sim::IsValid does).
    bool ReadPlayerTuning(ByteReader& in, sim::PlayerTuning& tuning);
}
