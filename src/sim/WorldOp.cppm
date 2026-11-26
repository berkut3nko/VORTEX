module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

export module vortex.sim:worldop;

import vortex.voxel;

export namespace vortex::sim {

    /// @brief The kinds of WorldOp. Values are part of the wire format (stage 3): never renumber.
    enum class WorldOpKind : uint8_t {
        CarveSphere = 1,   ///< `sphere`, `power`
        FillSphere = 2,    ///< `sphere`, `id`
        SwapSphere = 3,    ///< `sphere`, `from` -> `id`
        FillBox = 4,       ///< voxel box [`min`, `max`), `id`
        Detach = 5,        ///< terrain island: `min` = origin, `mask`, `velocity`, `netId`
        /// Terrain crumbs (islands under scene::MinPieceVoxels(world voxel size)): `min` = origin, `mask`. The
        /// voxels are cleared like a Detach's; whoever draws shows them as particles, nobody
        /// makes a piece or a body of them.
        ClearMask = 6,
        MapShape = 7,      ///< `map`, `shape`: id = map[old] for every voxel of the shape (magic.md "Apply")
        /// `map`, `min` = origin, `mask`: id = map[old] for every masked voxel. `axis` is the run order of the
        /// mask's encoding only, never which voxels change.
        MapMask = 8,
        /// `shape` (a capsule), `power`: every voxel of the capsule that the power may carve becomes air
        /// (Pierce's bore, magic.md "Pierce (Solid): a bore that pays for every voxel it adds").
        CarveCapsule = 9,
    };

    /**
     * @brief One replicated change of the terrain (docs/design/networking.md, "World operations").
     * @details Integer parameters only, so applying the same operations in the same order to two
     * worlds with equal voxels gives equal voxels on any machine (ApplyOp). A blast is one
     * CarveSphere, one Detach per terrain island of at least scene::MinPieceVoxels(world voxel size) that broke
     * off, then at most one ClearMask for all smaller islands together (sim::Blast).
     *
     * One struct for every kind; only the fields the kind lists are meaningful, the others keep
     * their defaults (so `==` compares operations exactly). Build them with the factories.
     */
    struct WorldOp {
        WorldOpKind kind = WorldOpKind::CarveSphere;
        voxel::SphereQ sphere;                 ///< Carve / Fill / Swap: centre and radius in 1/16 voxel.
        uint16_t power = 0;                    ///< Carve, CarveCapsule: in 1/16 hardness units (voxel::QuantizePower).
        uint8_t id = 0;                        ///< Fill, FillBox: material written; Swap: the replacement.
        uint8_t from = 0;                      ///< Swap: the material replaced.
        glm::ivec3 min{0};                     ///< FillBox: minimum voxel; Detach, ClearMask: world voxel of mask (0, 0, 0).
        glm::ivec3 max{0};                     ///< FillBox: maximum voxel, exclusive.
        /// Detach: the server entity that carries the piece; 0 = nobody does, every client makes
        /// its own rubble (all islands until networking stage 6).
        uint32_t netId = 0;
        voxel::VoxelMask mask;                 ///< Detach, ClearMask: voxels cleared, relative to `min`.
        /// Detach: initial velocity of the piece in m/s (the blast's push). Not used by ApplyOp.
        glm::vec3 velocity{0.0f};
        uint8_t map = 0;                       ///< MapShape, MapMask: a voxel::TurningMapId (1..15).
        voxel::ShapeQ shape;                   ///< MapShape: the shape; CarveCapsule: its capsule.
        uint8_t axis = 0;                      ///< MapMask: the run axis of its mask's encoding (0 x, 1 y, 2 z).

        static WorldOp Carve(const voxel::SphereQ& sphere, uint16_t power);
        static WorldOp Fill(const voxel::SphereQ& sphere, uint8_t id);
        static WorldOp Swap(const voxel::SphereQ& sphere, uint8_t from, uint8_t to);
        static WorldOp Box(const glm::ivec3& min, const glm::ivec3& max, uint8_t id);
        static WorldOp Detach(const glm::ivec3& origin, voxel::VoxelMask mask, const glm::vec3& velocity, uint32_t netId = 0);
        /// @brief A ClearMask: the voxels `origin + p` of every set bit `p` become air.
        static WorldOp Clear(const glm::ivec3& origin, voxel::VoxelMask mask);
        /// @brief A MapShape: every voxel of `shape` takes `map`'s new id.
        static WorldOp Map(uint8_t map, const voxel::ShapeQ& shape);
        /// @brief A MapMask: the voxels `origin + p` of every set bit `p` take `map`'s new id; `axis` is
        /// voxel::ShortestRunAxis(mask).
        static WorldOp MapMasked(uint8_t map, const glm::ivec3& origin, voxel::VoxelMask mask);
        /// @brief A CarveCapsule: the voxels of `capsule` (kind voxel::kShapeCapsule) that `power` may carve.
        static WorldOp Bore(const voxel::ShapeQ& capsule, uint16_t power);

        /// @brief Voxel box [min, max) the operation can change (empty for an empty sphere / box).
        void Bounds(glm::ivec3& outMin, glm::ivec3& outMax) const;

        friend bool operator==(const WorldOp&, const WorldOp&) = default;
    };

    /// @brief What ApplyOp did.
    struct OpResult {
        uint32_t changed = 0;                  ///< Voxels whose id changed.
        /// Chunks the operation touches but could not change, see voxel::EditReport: `skipped` =
        /// not loaded (outside the window, Unloaded, Loading), `unsynced` = no free store slot.
        /// Either way that chunk now differs from a world where the operation applied: it must
        /// be replayed or re-sent before it is trusted (networking stage 7).
        voxel::EditReport report;
        bool Complete() const { return report.Complete(); }
    };

    /**
     * @brief Applies one operation to a world: the same World::*Q call on server and clients.
     * @param edits If not null, every changed voxel is appended (old and new id) in the order of
     * World's edit functions. For a Detach these are the ids of the piece (BuildDetachedPiece),
     * for a ClearMask the crumbs' voxels (particles).
     */
    /// @param clip If not null, only the chunks inside it are visited (voxel::ChunkClip: the build
    /// ceiling is still decided on the whole operation; a client applies an operation to the chunks
    /// that have a base below it, networking stage 7a).
    OpResult ApplyOp(voxel::World& world, const WorldOp& op, std::vector<voxel::VoxelEdit>* edits = nullptr,
                     const voxel::ChunkClip* clip = nullptr);

    /// @brief Bytes the operation takes as an event on the wire, its kind byte included (kinds 1-6 as
    /// net::WriteEvent writes them; MapShape, MapMask and CarveCapsule in the layout of magic.md "Wire values").
    size_t OpWireBytes(const WorldOp& op);

    /// @brief The chunk box [minChunk, maxChunk] (inclusive) of `op.Bounds()`; false when the
    /// operation's box is empty.
    bool OpChunkRange(const WorldOp& op, glm::ivec3& minChunk, glm::ivec3& maxChunk);
    /// @brief True if the operation can change a voxel of `chunk` (the sphere, box and mask tests
    /// World's edits use to decide whether they touch a chunk: exact, except MapShape and CarveCapsule,
    /// which touch every chunk of their box). A chunk it does not touch is never changed by it, whatever its content.
    bool OpTouchesChunk(const WorldOp& op, const glm::ivec3& chunk);

    /**
     * @brief The volume of a detached piece, made from the voxels its Detach removed: size =
     * `op.mask.Size()`, each removed voxel at `voxel - op.min` with its old id.
     * @details A client builds the piece from its own world (worlds are equal at that point of
     * the operation stream), so the ids never travel. Voxels that were not removed (a chunk that
     * was skipped or unsynced) are air in the piece, so no voxel exists twice.
     */
    voxel::VoxelVolume BuildDetachedPiece(const WorldOp& op, std::span<const voxel::VoxelEdit> removed);
}
