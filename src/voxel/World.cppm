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
import :volume;

export namespace vortex::voxel {

    struct WorldConfig {
        float voxelSize = 0.1f;                  ///< Edge length of a voxel in metres.
        glm::ivec3 windowSize{32, 16, 32};       ///< Loaded region in chunks.
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

    // --- quantised edits (docs/design/networking.md, "World operations") ----------------------
    //  Operations that two machines must apply with identical results take integer parameters
    //  only. Positions and radii are in q = 1/16 voxel, power in 1/16 hardness units; the sphere
    //  test is done in 64-bit integers. No float takes part in deciding which voxels change.

    inline constexpr int32_t kQuantaPerVoxel = 16;          ///< q per voxel.
    inline constexpr int32_t kPowerQuantaPerHardness = 16;  ///< Power units per Material::hardness unit.
    /// Voxel coordinates an edit may reach: [-kMaxEditVoxel, kMaxEditVoxel). Voxels beyond are
    /// never edited (10^8 m at 0.1 m voxels; keeps every integer step in range). A quantised
    /// sphere centre is an int32 in q and reaches +-2^27 voxels only.
    inline constexpr int32_t kMaxEditVoxel = 1 << 30;

    /**
     * @brief A sphere in quantised units: centre in q (1/16 voxel, world voxel space), radius in q.
     * @details A voxel v belongs to the sphere when, per axis, d = 16 * v + 8 - centre (the voxel
     * centre minus the sphere centre, in q) satisfies dot(d, d) <= radius^2, computed in int64.
     * Radius 0 contains a voxel only if its centre is exactly the sphere centre.
     */
    struct SphereQ {
        glm::ivec3 center{0};
        uint16_t radius = 0;
        friend bool operator==(const SphereQ&, const SphereQ&) = default;
    };

    /**
     * @brief Quantises a sphere given in voxel units (centre may be fractional).
     * @details Each value is multiplied by 16 and rounded to the nearest integer, halves away from
     * zero (std::llround); the centre is clamped to the int32 range (+-2^27 voxels), the radius to
     * [0, 65535] q (4095.9 voxels). A non-finite input gives radius 0 at the clamped / zero centre.
     * Deterministic: the same doubles give the same integers on every IEEE-754 machine.
     */
    SphereQ QuantizeSphere(const glm::dvec3& centerVoxels, double radiusVoxels);
    /// @brief Hardness units -> power in 1/16 units, rounded to nearest, clamped to [0, 65535].
    /// NaN gives 0. A voxel survives a carve of power P when `hardness * 16 > P` (see CarveSphereQ).
    uint16_t QuantizePower(float power);
    /// @brief The sphere test of SphereQ for one voxel (int64, no overflow for any int voxel).
    bool InSphereQ(const SphereQ& sphere, const glm::ivec3& voxel);
    /**
     * @brief Voxel box [min, max) holding every voxel of the sphere: per axis the voxels with
     * |16 v + 8 - centre| <= radius (at most one voxel larger per side than the tight bounds),
     * clipped to +-kMaxEditVoxel. Empty when no voxel centre is within the radius on some axis.
     */
    void SphereVoxelBounds(const SphereQ& sphere, glm::ivec3& min, glm::ivec3& max);

    /**
     * @brief Chunks an edit could not apply, appended by the edit functions of World.
     * @details A chunk is listed only if the operation would touch at least one of its voxels
     * (exact for spheres, boxes and masks), in the order the edit visits chunks (y, z, x).
     *  - `skipped`: the chunk is not loaded (outside the window, Unloaded or still Loading). The
     *    operation changed nothing there; a client must get it later (replay or chunk data).
     *  - `unsynced`: the chunk is loaded but stored as Empty / Uniform and the ChunkStore had no
     *    free slot to make it editable. Nothing in that chunk changed (all-or-nothing per chunk).
     */
    struct EditReport {
        std::vector<glm::ivec3> skipped;
        std::vector<glm::ivec3> unsynced;
        bool Complete() const { return skipped.empty() && unsynced.empty(); }
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
     * The window is a toroidal array: a chunk coordinate maps to `coord mod size`, so
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
        //  Every edit visits chunks in (y, z, x) order and voxels in (z, y, x) order inside a
        //  chunk, decides per voxel from (voxel, old id) only, and appends to `edits` in that
        //  order. Chunks it cannot apply to are listed in `report` (see EditReport); without a
        //  report they are skipped silently. The integer (Q) functions are what replicated
        //  operations use; the float ones quantise (QuantizeSphere / QuantizePower) and forward.

        /// @brief Sets one voxel. Returns false if its chunk is not loaded (or the store is full).
        bool SetVoxel(const glm::ivec3& v, uint8_t id);
        /// @brief Fills the voxel box [min, max). Returns the number of voxels changed.
        uint32_t FillBox(const glm::ivec3& min, const glm::ivec3& max, uint8_t id, std::vector<VoxelEdit>* edits = nullptr,
                         EditReport* report = nullptr);
        /// @brief Sets every voxel of the sphere to `id`. Returns the number of voxels changed.
        uint32_t FillSphereQ(const SphereQ& sphere, uint8_t id, std::vector<VoxelEdit>* edits = nullptr, EditReport* report = nullptr);
        /**
         * @brief Removes material inside a sphere, respecting material hardness.
         * @details A solid voxel is removed unless its material is flagged indestructible or
         * `hardness * 16 > power` (float multiply by 16 is exact, so this equals
         * `hardness > power / 16` with no rounding). Without a MaterialLibrary everything goes.
         */
        uint32_t CarveSphereQ(const SphereQ& sphere, uint16_t power, std::vector<VoxelEdit>* removed = nullptr,
                              EditReport* report = nullptr);
        /// @brief Replaces material `from` by `to` inside the sphere (other ids untouched; `from`
        /// may be 0 to fill only air). `from == to` changes nothing.
        uint32_t SwapSphereQ(const SphereQ& sphere, uint8_t from, uint8_t to, std::vector<VoxelEdit>* edits = nullptr,
                             EditReport* report = nullptr);
        /// @brief Sets to air every voxel `origin + p` whose mask bit p is set (a terrain island
        /// that broke off). Returns the number of voxels changed (masked voxels already air do not count).
        uint32_t ClearMask(const glm::ivec3& origin, const VoxelMask& mask, std::vector<VoxelEdit>* removed = nullptr,
                           EditReport* report = nullptr);

        /// @brief FillSphereQ(QuantizeSphere(centre, radius), ...): centre and radius in voxels.
        uint32_t FillSphere(const glm::vec3& centerVoxel, float radiusVoxels, uint8_t id, std::vector<VoxelEdit>* edits = nullptr);
        /// @brief CarveSphereQ(QuantizeSphere(centre, radius), QuantizePower(power), ...): voxels whose
        /// hardness exceeds `power` (or flagged indestructible) survive.
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
        template <typename Fn, typename Touches>
        uint32_t EditRegion(const glm::ivec3& min, const glm::ivec3& max, Fn&& decide, Touches&& touches,
                            std::vector<VoxelEdit>* edits, EditReport* report);

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
