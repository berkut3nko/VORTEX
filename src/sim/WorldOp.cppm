module;

#include <glm/glm.hpp>

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
    };

    /**
     * @brief One replicated change of the terrain (docs/design/networking.md, "World operations").
     * @details Integer parameters only, so applying the same operations in the same order to two
     * worlds with equal voxels gives equal voxels on any machine (ApplyOp). A blast is one
     * CarveSphere followed by one Detach per terrain island that broke off (sim::Blast).
     *
     * One struct for every kind; only the fields the kind lists are meaningful, the others keep
     * their defaults (so `==` compares operations exactly). Build them with the factories.
     */
    struct WorldOp {
        WorldOpKind kind = WorldOpKind::CarveSphere;
        voxel::SphereQ sphere;                 ///< Carve / Fill / Swap: centre and radius in 1/16 voxel.
        uint16_t power = 0;                    ///< Carve: in 1/16 hardness units (voxel::QuantizePower).
        uint8_t id = 0;                        ///< Fill, FillBox: material written; Swap: the replacement.
        uint8_t from = 0;                      ///< Swap: the material replaced.
        glm::ivec3 min{0};                     ///< FillBox: minimum voxel; Detach: world voxel of mask (0, 0, 0).
        glm::ivec3 max{0};                     ///< FillBox: maximum voxel, exclusive.
        /// Detach: the server entity that carries the piece; 0 = nobody does, every client makes
        /// its own rubble (all islands until networking stage 6).
        uint32_t netId = 0;
        voxel::VoxelMask mask;                 ///< Detach: voxels cleared, relative to `min`.
        /// Detach: initial velocity of the piece in m/s (the blast's push). Not used by ApplyOp.
        glm::vec3 velocity{0.0f};

        static WorldOp Carve(const voxel::SphereQ& sphere, uint16_t power);
        static WorldOp Fill(const voxel::SphereQ& sphere, uint8_t id);
        static WorldOp Swap(const voxel::SphereQ& sphere, uint8_t from, uint8_t to);
        static WorldOp Box(const glm::ivec3& min, const glm::ivec3& max, uint8_t id);
        static WorldOp Detach(const glm::ivec3& origin, voxel::VoxelMask mask, const glm::vec3& velocity, uint32_t netId = 0);

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
     * World's edit functions. For a Detach these are the ids of the piece (BuildDetachedPiece).
     */
    OpResult ApplyOp(voxel::World& world, const WorldOp& op, std::vector<voxel::VoxelEdit>* edits = nullptr);

    /**
     * @brief The volume of a detached piece, made from the voxels its Detach removed: size =
     * `op.mask.Size()`, each removed voxel at `voxel - op.min` with its old id.
     * @details A client builds the piece from its own world (worlds are equal at that point of
     * the operation stream), so the ids never travel. Voxels that were not removed (a chunk that
     * was skipped or unsynced) are air in the piece, so no voxel exists twice.
     */
    voxel::VoxelVolume BuildDetachedPiece(const WorldOp& op, std::span<const voxel::VoxelEdit> removed);
}
