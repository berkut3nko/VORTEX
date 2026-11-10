module;

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

export module vortex.voxel:world;

import vortex.core;
import :chunk;
import :store;
import :material;
import :generator;
import :region;
import :trace;

export namespace vortex::voxel {

    struct WorldConfig {
        float voxelSize = 0.1f;                  ///< Edge length of a voxel in metres.
        glm::ivec3 windowSize{32, 16, 32};       ///< Loaded region in chunks; each a power of two.
        std::filesystem::path saveDirectory;     ///< Where modified chunks are stored ("" = memory only).
        uint32_t maxJobsInFlight = 0;            ///< Generation jobs running at once (0 = 2 per worker).
        uint32_t maxIntegrationsPerUpdate = 64;  ///< Finished chunks installed per Update().
    };

    enum class CellState : uint8_t { Unloaded, Loading, Empty, Uniform, Dense };

    /// @brief One voxel changed by an edit operation.
    struct VoxelEdit {
        glm::ivec3 voxel{0};
        uint8_t oldId = 0;
        uint8_t newId = 0;
    };

    /// @brief Result of World::Raycast.
    struct WorldRayHit {
        bool hit = false;
        float distance = 0.0f;
        glm::dvec3 position{0.0};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        glm::ivec3 voxel{0};          ///< The solid voxel that was hit.
        uint8_t material = 0;
    };

    struct WorldStats {
        uint32_t denseChunks = 0;
        uint32_t uniformChunks = 0;
        uint32_t emptyChunks = 0;
        uint32_t pendingChunks = 0;   ///< Not yet generated.
        uint32_t jobsInFlight = 0;
    };

    /**
     * @brief The streaming voxel world.
     *
     * @details The world is unbounded; at any time a box of chunks around a focus point (the
     * "window") is loaded. Chunks entering the window are loaded from the save or generated on
     * worker threads; chunks leaving it are saved if they were modified, and freed.
     *
     * Storage is sparse: chunks that are all air or all one material need no memory, only
     * chunks containing a surface occupy a slot in the ChunkStore.
     *
     * The window is a toroidal array: a chunk coordinate maps to `coord & (size - 1)`, so
     * moving the window never moves data. The same array, in the same encoding, is what the
     * shaders traverse (see shaders/shared/trace.glsl) - `Grid()` is uploaded verbatim.
     *
     * Threading: the public interface is main-thread only.
     */
    class World {
    public:
        World(const WorldConfig& config, ChunkStore& store, JobSystem& jobs,
              std::shared_ptr<const IWorldGenerator> generator, const MaterialLibrary* materials = nullptr);
        ~World();
        World(const World&) = delete;
        World& operator=(const World&) = delete;

        const WorldConfig& Config() const { return m_Config; }
        float VoxelSize() const { return m_Config.voxelSize; }

        // --- streaming -------------------------------------------------------------------
        /// @brief Recentres the window on `focus` (world metres) and advances loading. Call every frame.
        void Update(const glm::dvec3& focus);
        /// @brief Blocks until every chunk of the window around `focus` is loaded (tools, tests, loading screens).
        void LoadBlocking(const glm::dvec3& focus);
        /// @brief Saves every modified chunk currently loaded and writes the region files.
        void Save();
        WorldStats Stats() const;

        // --- window (shared with the renderer and the tracer) ----------------------------
        const glm::ivec3& WindowSize() const { return m_Config.windowSize; }
        const glm::ivec3& WindowMinChunk() const { return m_WindowMin; }
        /// @brief World position (metres) of the window's minimum corner: the origin of "render space".
        glm::dvec3 WindowOrigin() const;
        std::span<const uint32_t> Grid() const { return m_Grid; }
        /// @brief Changes whenever Grid() or WindowMinChunk() change.
        uint64_t GridRevision() const { return m_GridRevision; }
        /// @brief Scene view for CPU ray queries in render space (rays relative to WindowOrigin()).
        TraceScene MakeTraceScene() const;

        // --- coordinates -----------------------------------------------------------------
        glm::ivec3 WorldToVoxel(const glm::dvec3& p) const;
        glm::dvec3 VoxelToWorld(const glm::ivec3& v) const;      ///< Minimum corner of the voxel.
        glm::dvec3 VoxelCenter(const glm::ivec3& v) const;

        // --- queries ---------------------------------------------------------------------
        CellState StateOf(const glm::ivec3& chunk) const;
        bool IsChunkLoaded(const glm::ivec3& chunk) const;
        /// @brief Material id of a voxel; 0 for air and for chunks that are not loaded.
        uint8_t GetVoxel(const glm::ivec3& v) const;
        bool IsSolid(const glm::ivec3& v) const { return GetVoxel(v) != 0; }
        bool IsSolidAt(const glm::dvec3& worldPos) const { return GetVoxel(WorldToVoxel(worldPos)) != 0; }
        /// @brief Dense chunk data, or nullptr if the chunk is empty / uniform / not loaded.
        const Chunk* GetChunk(const glm::ivec3& chunk, uint8_t* uniformId = nullptr) const;
        WorldRayHit Raycast(const glm::dvec3& origin, const glm::vec3& direction, float maxDistance) const;

        // --- editing ---------------------------------------------------------------------
        /// @brief Sets one voxel. Returns false if its chunk is not loaded (or the store is full).
        bool SetVoxel(const glm::ivec3& v, uint8_t id);
        /// @brief Fills the voxel box [min, max). Returns the number of voxels changed.
        uint32_t FillBox(const glm::ivec3& min, const glm::ivec3& max, uint8_t id, std::vector<VoxelEdit>* edits = nullptr);
        /// @brief Fills a sphere (centre and radius in voxels). Returns the number of voxels changed.
        uint32_t FillSphere(const glm::vec3& centerVoxel, float radiusVoxels, uint8_t id, std::vector<VoxelEdit>* edits = nullptr);
        /**
         * @brief Removes material inside a sphere, respecting material hardness.
         * @param power Voxels whose material hardness exceeds `power` (or that are flagged
         * indestructible) survive. Requires a MaterialLibrary; without one everything is removed.
         */
        uint32_t CarveSphere(const glm::vec3& centerVoxel, float radiusVoxels, float power, std::vector<VoxelEdit>* removed = nullptr);

        /// @brief Chunks whose solid content changed (loaded, unloaded or edited) since the last call.
        std::vector<glm::ivec3> TakeChangedChunks();

    private:
        struct Cell {
            glm::ivec3 coord{0};
            uint32_t slot = ChunkStore::kInvalidSlot;
            uint32_t generation = 0;        // bumped whenever the cell is re-targeted: stale jobs are ignored
            CellState state = CellState::Unloaded;
            uint8_t uniformId = 0;
            bool modified = false;          // differs from generator output: must be saved
            bool changeQueued = false;
            bool assigned = false;          // coord is valid
        };
        struct Shared;                      // state shared with worker jobs

        uint32_t CellIndex(const glm::ivec3& chunk) const;
        Cell* FindCell(const glm::ivec3& chunk);
        const Cell* FindCell(const glm::ivec3& chunk) const;
        void Recenter(const glm::ivec3& newMin, const glm::ivec3& focusChunk);
        void UnloadCell(uint32_t index);
        void PersistCell(Cell& cell);
        void SetGrid(uint32_t index, const Cell& cell);
        void QueueChange(Cell& cell);
        bool Materialize(uint32_t index);
        void IntegrateResults(uint32_t budget);
        void SubmitJobs();
        static void LoadJob(const std::shared_ptr<Shared>& shared, uint32_t cellIndex, glm::ivec3 coord, uint32_t generation);
        template <typename Fn> uint32_t EditRegion(const glm::ivec3& min, const glm::ivec3& max, Fn&& decide, std::vector<VoxelEdit>* edits);

        WorldConfig m_Config;
        ChunkStore& m_Store;
        JobSystem& m_Jobs;
        const MaterialLibrary* m_Materials;
        std::shared_ptr<Shared> m_Shared;

        std::vector<Cell> m_Cells;
        std::vector<uint32_t> m_Grid;
        uint64_t m_GridRevision = 1;
        glm::ivec3 m_WindowMin{0};
        bool m_HasWindow = false;

        std::vector<uint32_t> m_LoadOrder;   // cell indices still to load, nearest first
        size_t m_LoadCursor = 0;
        uint32_t m_MaxJobs = 8;

        std::vector<glm::ivec3> m_ChangedChunks;
    };
}
