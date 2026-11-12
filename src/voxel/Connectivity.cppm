module;

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

export module vortex.voxel:connectivity;

import :volume;
import :world;
import :material;

export namespace vortex::voxel {

    /// @brief Connected component labelling of a volume (6-connectivity: faces only).
    struct ComponentLabels {
        glm::ivec3 size{0};
        std::vector<uint32_t> labels;          ///< Per voxel (same indexing as VoxelVolume): 0 = empty, 1..count.
        uint32_t count = 0;
        std::vector<uint32_t> voxelCounts;     ///< [count + 1], entry 0 unused.
        std::vector<glm::ivec3> boundsMin;     ///< [count + 1], inclusive.
        std::vector<glm::ivec3> boundsMax;     ///< [count + 1], exclusive.
        /// [count + 1], entry 0 unused: index (VoxelVolume::Index order) of the component's first
        /// voxel in scan order - the seed it was labelled from.
        std::vector<uint32_t> firstIndex;
    };

    /**
     * @brief Labels the connected components of the solid voxels of a volume.
     * @details Linear-time flood fill over the dense array (no hashing), a 32^3 object takes
     * microseconds. This is what decides whether a damaged object breaks apart.
     */
    ComponentLabels LabelComponents(const VoxelVolume& volume);

    /// @brief One piece of a volume that fell apart.
    struct VolumePiece {
        VoxelVolume volume;            ///< Cropped to the piece's bounds.
        glm::ivec3 offset{0};          ///< Position of the piece's minimum corner inside the source volume.
        uint32_t voxelCount = 0;
    };

    /// @brief A voxel that was too small a fragment to become an object (turn it into a particle).
    struct LooseVoxel {
        glm::ivec3 position{0};        ///< In the source volume.
        uint8_t material = 0;
    };

    struct SplitResult {
        std::vector<VolumePiece> pieces;     ///< Sorted by voxel count, largest first.
        std::vector<LooseVoxel> loose;       ///< Voxels of components smaller than minVoxels.
    };

    /**
     * @brief Splits a volume into its connected components.
     * @param minVoxels Components with fewer voxels are not returned as pieces but as `loose` voxels.
     */
    SplitResult SplitComponents(const VoxelVolume& volume, uint32_t minVoxels = 1);

    /// @brief A chunk of terrain that lost its connection to the rest of the world.
    struct FloatingIsland {
        glm::ivec3 origin{0};          ///< World voxel coordinate of the volume's minimum corner.
        VoxelVolume volume;
        uint32_t voxelCount = 0;
    };

    /// @brief A component of an island box that touches the box's face (ExtractFloatingIslands
    /// counts it as attached): a candidate for the face flood (ProveFaceComponents).
    struct FaceComponent {
        uint32_t label = 0;              ///< In IslandBoxScan::labels.
        glm::ivec3 firstVoxel{0};        ///< World voxel: the component's first voxel in box scan order.
        uint32_t inBoxVoxels = 0;
    };

    /// @brief What ExtractFloatingIslands saw in its box besides the islands (optional out-parameter).
    struct IslandBoxScan {
        /// Labels of the box's solid (not ignored) voxels; empty when the search was refused or
        /// the box held nothing solid.
        ComponentLabels labels;
        /// Components touching the box face with at most `maxIslandVoxels` voxels inside the box,
        /// in label order: the face-flood candidates. Larger face components are attached for good.
        std::vector<FaceComponent> faceComponents;
        uint32_t faceOversize = 0;       ///< Face components over `maxIslandVoxels` in the box (never flooded).
        /// Components not touching the face with more than `maxIslandVoxels` voxels: floating, but
        /// too large to detach. They stay in the world (counted, docs/design/shred.md stage 1c).
        uint32_t oversizeIslands = 0;
        uint64_t boxVoxels = 0;          ///< Voxels of the box that were gathered (work, not time).
        /// The box that was labelled: the region asked for, without the rows below the bottom of
        /// a full-height window (the world's ground). `labels` and the face are of this box.
        glm::ivec3 boxMin{0}, boxMax{0};
    };

    /**
     * @brief Finds terrain that is no longer attached to anything after an edit, and optionally
     * removes it from the world so it can be turned into falling dynamic objects.
     * @details Only the box [regionMin, regionMax) is examined (keep it modest, e.g. the edit's
     * bounds grown by a few metres). A component counts as attached if it touches the boundary
     * of the box - i.e. it continues outside the examined region - so the search can never
     * rip out a hillside just because the box was small (ProveFaceComponents then proves such
     * components by a bounded flood). Voxels whose material has any of `ignoreFlags` (liquids by
     * default) neither support nor belong to islands. In a world with a fixed height range, chunks
     * above the build ceiling are air (nothing can exist there), not "not loaded", and the box ends
     * at the window's bottom row (below it is the world's ground, never loaded): a component
     * touching that row touches the box face (`IslandBoxScan::boxMin`); a box entirely below the
     * window is searched and holds nothing.
     * @param maxIslandVoxels Larger floating components are left in place.
     *
     * The result depends only on the voxels of the box: components are found by a single-threaded
     * scan in VoxelVolume::Index order and returned in the order of their first voxel, so two
     * worlds with equal voxels in the box give equal islands in equal order. The box is gathered
     * chunk by chunk (one lookup per chunk, empty 4^3 blocks skipped, a Packed chunk decoded once).
     * @param removeFromWorld Clear the island voxels in the world (World::SetVoxel).
     * @param searched If not null: set to true when the box was examined, false when the search
     * was refused (box empty or larger than 192 voxels per axis, or a chunk of it not loaded) -
     * an empty result then proves nothing.
     * @param scan If not null: the labels, the face components and the counters above.
     */
    std::vector<FloatingIsland> ExtractFloatingIslands(World& world, const MaterialLibrary* materials,
                                                       const glm::ivec3& regionMin, const glm::ivec3& regionMax,
                                                       uint32_t maxIslandVoxels, bool removeFromWorld = true,
                                                       uint32_t ignoreFlags = MaterialFlag_Liquid, bool* searched = nullptr,
                                                       IslandBoxScan* scan = nullptr);

    /// Half extent of a blast's flood box in voxels (docs/design/shred.md, stage 1): the box is
    /// floor(centre / 16) +- this, a pure function of the integer sphere. 192 per axis, the
    /// island region limit. Not rescaled with the voxel size (24 m at 0.125 m, 19.2 m at 0.1 m).
    inline constexpr int32_t kFloodHalfExtent = 96;

    /// @brief Bounds of a face flood. Everything is counted in voxels, never in time.
    struct FloodLimits {
        glm::ivec3 floodMin{0}, floodMax{0};   ///< The flood box [min, max), at most 192 per axis.
        uint32_t maxVoxels = 0;                ///< A flood that visits more gives up (attached).
        uint32_t maxVisitsPerSearch = 0;       ///< Work budget of the whole call, in voxel visits.
        /// What a chunk of the flood box that is not loaded means. true (offline): the whole call
        /// stops with `complete == false` (the caller defers the search). false (session server,
        /// whose window holds every flood box by construction): the candidate is attached and
        /// `attachedByUnloaded` counts an error.
        bool deferOnUnloaded = true;
    };

    /// @brief Result of ProveFaceComponents.
    struct FloodResult {
        /// Closed floods (islands, any size: the caller splits pieces from crumbs), in candidate order.
        std::vector<FloatingIsland> islands;
        /// false: a flood met a chunk of the flood box that is not loaded and deferOnUnloaded was
        /// set; `islands` must not be used, `unloadedChunk` names the chunk.
        bool complete = true;
        glm::ivec3 unloadedChunk{0};
        uint32_t candidates = 0;           ///< Face components given.
        uint32_t proven = 0;               ///< Floods that closed (= islands.size()).
        uint32_t merged = 0;               ///< Candidates already inside an earlier closed flood.
        uint32_t attachedByLimit = 0;      ///< Over maxVoxels, at the flood box face, or below the window.
        uint32_t attachedByKnown = 0;      ///< Reached a voxel known to be attached.
        uint32_t attachedByBudget = 0;     ///< The work budget was spent (this and every later candidate).
        uint32_t attachedByUnloaded = 0;   ///< deferOnUnloaded false only: met a chunk that is not loaded.
        uint32_t chunksDecoded = 0;        ///< Packed chunks decoded (again, after an eviction).
        uint32_t visits = 0;               ///< Voxels visited by all floods.
    };

    /**
     * @brief Proves face components of an island box floating by an outward flood through the
     * world, inside a fixed flood box (docs/design/shred.md, stage 1).
     * @details Candidates are processed by in-box voxel count ascending, ties by label. Each
     * flood is breadth-first (FIFO) from the candidate's first voxel, 6-connected, neighbours
     * -x +x -y +y -z +z, solid = id != 0 and not `ignoreFlags`. Inside the island box voxels
     * are judged by `boxLabels`: a face component that is not a candidate (or one given up on)
     * is attached. A flood gives up (attached) when it visits more than `maxVoxels`, visits a
     * voxel on the flood box face (it would leave the box) or below the bottom of a fixed-height
     * window, reaches a voxel an earlier flood gave up on, or when the visit budget is spent.
     * Rows above the build ceiling are air. A closed flood is an island; a candidate whose first
     * voxel an earlier closed flood visited is skipped. Integer-only and in a fixed order: equal
     * voxels in the flood box give equal results, whatever is loaded outside it.
     * Reads go through World::GetChunk (one lookup per chunk change); a Packed chunk is decoded
     * once into a cache of 16 chunks. The 2-bit visit state over the flood box (1.7 MiB) is
     * allocated once per thread. Nothing in the world is changed.
     */
    FloodResult ProveFaceComponents(const World& world, const MaterialLibrary* materials, std::span<const FaceComponent> candidates,
                                    const glm::ivec3& boxMin, const glm::ivec3& boxMax, const ComponentLabels& boxLabels,
                                    uint32_t ignoreFlags, const FloodLimits& limits);
}
