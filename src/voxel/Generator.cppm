module;

#include <glm/glm.hpp>

#include <climits>
#include <cstdint>
#include <memory>

export module vortex.voxel:generator;

import :chunk;
import :material;

export namespace vortex::voxel {

    /// @brief What a generated (or stored) chunk contains.
    enum class ChunkKind : uint8_t {
        Empty,     ///< Nothing but air: no storage.
        Uniform,   ///< Every voxel is the same solid material: no storage.
        Dense      ///< Arbitrary content: occupies a chunk slot.
    };

    struct ChunkContent {
        ChunkKind kind = ChunkKind::Empty;
        uint8_t uniformId = 0;
    };

    /// @brief Height range (metres) that holds everything a generator can produce. Below `min`
    /// every voxel is solid, above `max` every voxel is air. `max <= min` means unknown.
    struct HeightBounds {
        float min = 0.0f;
        float max = 0.0f;
        bool Known() const { return max > min; }
    };

    /**
     * @brief What a column of chunks (fixed chunk x and z, every chunk row y) contains, decided
     * from the column's height tile without generating a chunk (docs/design/view-distance.md).
     * @details Rows are chunk y coordinates. A row `>= emptyFrom` is Empty, a row `< solidBelow` is
     * Uniform `solidId`; only the band between needs Generate. `surfaceFrom` is the lowest row a
     * ray coming from the sky through this column can reach (the row of the lowest surface voxel,
     * minus one); band rows below it are buried (caves). Generate must agree exactly: for a row
     * outside the band it returns the same Empty / Uniform result.
     */
    struct ColumnClass {
        bool known = false;                 ///< false: nothing is known, every row needs Generate
        int32_t emptyFrom = INT32_MAX;
        int32_t solidBelow = INT32_MIN;
        uint8_t solidId = 0;
        int32_t surfaceFrom = INT32_MIN;
    };

    /**
     * @brief Procedural source of the world.
     * @details Generate() is called from worker threads, for any chunk, in any order, possibly
     * several times for the same chunk over the lifetime of a world. It therefore has to be a
     * pure function of (seed, coordinate): no shared mutable state, no dependence on neighbours
     * having been generated.
     */
    class IWorldGenerator {
    public:
        virtual ~IWorldGenerator() = default;

        /**
         * @brief Produces the content of chunk `coord`.
         * @param chunk Zero-initialised scratch chunk. For ChunkKind::Dense fill `chunk.ids`
         * (index with Chunk::VoxelIndex); the caller rebuilds the occupancy pyramid.
         * For Empty / Uniform results the chunk may be left untouched.
         */
        virtual ChunkContent Generate(const glm::ivec3& coord, Chunk& chunk) const = 0;

        /// @brief Conservative vertical extent of the generated world (metres). The default is
        /// unknown, which keeps a World in the camera-centred window of WorldConfig::windowSize.
        virtual HeightBounds VerticalBounds() const { return {}; }

        /// @brief Classifies the chunk column (cx, cz). Thread safe, like Generate. The default
        /// knows nothing (every row is generated).
        virtual ColumnClass ClassifyColumn(int cx, int cz) const {
            (void)cx;
            (void)cz;
            return {};
        }
    };

    /// @brief Tunables of the built-in terrain. All distances are in metres.
    struct TerrainSettings {
        uint32_t seed = 1337;
        float voxelSize = kDefaultVoxelSize;
        WorldMaterials materials;

        float seaLevel = 0.0f;
        float baseHeight = 3.0f;          ///< Average ground level above the sea.
        float continentHeight = 10.0f;    ///< Very low frequency swell.
        float hillHeight = 9.0f;
        float mountainHeight = 55.0f;
        float horizontalScale = 1.0f;     ///< >1 stretches every feature.
        float snowLine = 38.0f;
        bool caves = true;
        float caveMaxDepth = 30.0f;       ///< Caves only exist this far below the surface.
        bool trees = true;
        float treeSpacing = 7.0f;         ///< Size of the jittered grid trees are placed on.
        float treeDensity = 0.55f;        ///< Probability that a grid cell grows a tree.
        /// Generate returns rows outside ClassifyColumn's band without running the voxel loop.
        /// false (tests only): every row runs it, so a test can check that the rows the column
        /// pass skips really are all air / all dark stone. Changes no voxel.
        bool skipClassifiedRows = true;
    };

    /**
     * @brief Default world generator: rolling hills, ridged mountains, beaches, a sea, caves
     * and trees. Meant as a solid starting point and as a reference for writing game-specific
     * generators.
     */
    std::shared_ptr<IWorldGenerator> CreateTerrainGenerator(const TerrainSettings& settings);

    /**
     * @brief Surface height in metres of the built-in terrain at a world position in metres.
     * @details Exactly the float the generator computes before it is turned into voxels: the
     * determinism tests pin its bit pattern, and a server can use it to place things on the ground
     * without loading chunks. Trees and caves are not included. Clamped to TerrainSurfaceBounds.
     */
    float TerrainHeight(const TerrainSettings& settings, float x, float z);

    /**
     * @brief True when every tunable is finite and inside the range the generator supports.
     * @details Outside it float-to-int conversions in the generator can overflow (undefined
     * behaviour, so two builds may disagree) or loops become unbounded. Settings that come from a
     * file or from the network must pass this before CreateTerrainGenerator. Chunk coordinates
     * must also stay inside +-2^25 (voxel coordinates are 32-bit).
     */
    bool IsValid(const TerrainSettings& settings);

    /// @brief Range of TerrainHeight for these settings (the sum of the noise amplitudes; the
    /// height is clamped to it). Caves, trees and the sea are not included.
    HeightBounds TerrainSurfaceBounds(const TerrainSettings& settings);

    /// @brief Generator producing an infinite flat slab; useful for tests and tech demos.
    std::shared_ptr<IWorldGenerator> CreateFlatGenerator(int groundVoxelHeight, uint8_t surfaceId, uint8_t groundId);
}
