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
        float voxelSize = kDefaultVoxelSize;     ///< Edge length of a voxel in metres.
        /// Loaded region in chunks. With `fullHeight` (and known bounds) y is replaced by the
        /// rows of the vertical range.
        glm::ivec3 windowSize{32, 16, 32};
        std::filesystem::path saveDirectory;     ///< Where modified chunks are stored ("" = memory only).
        /// Generation jobs queued or running at once (0 = 8 per worker). New jobs are submitted
        /// once per Update(), so this also caps chunks per frame: many results (sky rows, column
        /// classifications) take far less than a frame. More would also delay collider cooking, which shares the queue (8 per worker: up to about 8 ms in the release build).
        uint32_t maxJobsInFlight = 0;
        /// Dense results installed per Update(); Empty, Uniform and column results are free.
        uint32_t maxIntegrationsPerUpdate = 64;

        // --- full-height window (docs/design/view-distance.md, stage 1) ---------------------
        /// The window covers a fixed range of rows (`heightRange`, or the generator's
        /// VerticalBounds plus `buildHeadroom`, rounded outwards to `heightSnap`) and follows the
        /// focus in x and z only: climbing or flying never unloads anything. Without known
        /// bounds the window stays camera-centred (windowSize.y).
        bool fullHeight = false;
        glm::vec2 heightRange{0.0f};             ///< Explicit [min, max) metres (a session's region); empty = from the generator.
        float buildHeadroom = 32.0f;             ///< Metres of buildable sky above the generator's bounds.
        float heightSnap = 16.0f;                ///< The range is rounded outwards to multiples of this (metres).
        /// Dense chunks that no ray can reach (geometric rule) and farther than `nearRadius` from
        /// the focus keep only their RLE bytes (CellState::Packed): no slot, drawn as rock.
        /// Needs `fullHeight` (the rule uses the column pass).
        bool packHidden = false;
        float nearRadius = 16.0f;                ///< Metres around the focus where everything is resident.
        /// While more columns of the window than this lack their surface, cave rows (outside the
        /// near radius) wait. 0: every surface first (measured best, view-distance.md "as built").
        uint32_t surfaceFirstBacklog = 0;
        /// Packed chunks made resident per Update() (near the focus, newly visible ones); also the
        /// number of resident buried chunks packed again per Update().
        uint32_t maxDecodesPerUpdate = 16;
        /// A buried chunk made resident near the focus is packed again once it is this many
        /// metres farther than `nearRadius` (and still hidden, unedited, with no edited face neighbour).
        float repackMargin = 8.0f;
    };

    enum class CellState : uint8_t { Unloaded, Loading, Empty, Uniform, Dense, Packed };

    /// What LoadBlocking waits for.
    enum class LoadScope : uint8_t {
        Everything,   ///< every chunk of the window
        Surface       ///< every column classified, every surface row and every chunk near the focus
    };

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
    /// never edited (1.3 x 10^8 m at 0.125 m voxels; keeps every integer step in range). A quantised
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
        /// The edit would have written solid voxels above the build ceiling (World::BuildCeiling):
        /// it was refused whole, nothing changed. Decided from integers and the ceiling only, so
        /// every machine with the same height range refuses the same edits; not part of Complete().
        bool outOfBounds = false;
        bool Complete() const { return skipped.empty() && unsynced.empty(); }
    };

    /// @brief A chunk whose solid content changed, as World::TakeChunkChanges reports it.
    struct ChunkChange {
        glm::ivec3 coord{0};
        /// Bounding box, in world voxel coordinates [editMin, editMax), of the voxels edits changed
        /// in this chunk since the last take. Empty (editMin == editMax) when the chunk was only
        /// loaded or unloaded. Physics uses it to know which part of an old collider is stale.
        glm::ivec3 editMin{0}, editMax{0};
        bool Edited() const { return editMin.x < editMax.x && editMin.y < editMax.y && editMin.z < editMax.z; }
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
        uint32_t packedChunks = 0;
        uint32_t pendingChunks = 0;   ///< Not yet generated.
        uint32_t jobsInFlight = 0;
        /// RLE bytes held: Packed chunks, plus the copies resident buried chunks keep so that
        /// they can be packed again without encoding.
        uint64_t packedBytes = 0;
        /// Columns inside 0.85 x the view radius whose surface rows are not all loaded (last Update).
        uint32_t unfilledColumns = 0;
        uint32_t columnsPending = 0;  ///< Columns of the window not classified yet.
        /// Cells that held a chunk (or were loading one) and were re-targeted while their column
        /// was within ViewRadius() of the focus horizontally: the hill bug. Counted by distance,
        /// whatever moved the window. Cumulative.
        uint32_t unloadsInsideRadius = 0;
        uint32_t recenters = 0;       ///< Window moves (cumulative).
        uint32_t retargetedColumns = 0;   ///< Columns re-targeted by window moves (cumulative).
        float lastRecenterMs = 0.0f;
        float maxRecenterMs = 0.0f;   ///< Slowest window move of at most one chunk in x and z.
        uint32_t decodes = 0;         ///< Packed chunks made resident (cumulative).
        double decodeMs = 0.0;        ///< Time spent decoding them (cumulative).
        uint32_t repacks = 0;         ///< Resident buried chunks packed again (cumulative).
        uint32_t reveals = 0;         ///< Packed chunks found visible after a neighbour column was classified (cumulative).
        /// Dense load results that found the ChunkStore full and were installed Packed (drawn as
        /// rock) instead: terrain is not shown as it is. Must stay 0. Cumulative.
        uint32_t storeFullLoads = 0;
        /// Decodes (near the focus, newly visible, next to an edit) refused because the store was
        /// full. Must stay 0. Cumulative.
        uint32_t storeFullDecodes = 0;
        /// Chunks the generator produced on worker threads (saved chunks that were loaded are not
        /// counted), and how many of them were Dense. Includes results the window had moved away
        /// from before they arrived: this is generation work, not what is shown. Cumulative.
        uint32_t generatedChunks = 0;
        uint32_t generatedDense = 0;
        /// Worker time spent inside IWorldGenerator::Generate for them (summed over threads):
        /// generatedChunks / generateMs is the generator's own speed, whatever the frame rate.
        double generateMs = 0.0;
        /// Region files of the save directory refused because they were written at another voxel
        /// size or are unreadable (RegionStore): their edits are not shown and the files are never
        /// overwritten.
        uint32_t refusedRegions = 0;
        /// Of those, the regions this session edited: Save() cannot write their edits.
        uint32_t unwrittenRegions = 0;
    };

    /// @brief What World::Save wrote.
    struct WorldSaveResult {
        uint32_t files = 0;              ///< Region files written.
        /// Regions with edits that were not written because their file is refused (another voxel
        /// size, or unreadable): those edits are lost when the world closes. 0 = everything saved.
        uint32_t unwrittenRegions = 0;
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
        /// @brief Blocks until the window around `focus` is loaded (tools, tests, loading screens).
        void LoadBlocking(const glm::dvec3& focus, LoadScope scope = LoadScope::Everything);
        /// @brief Saves every modified chunk currently loaded and writes the region files.
        WorldSaveResult Save();
        /// @brief Counts every cell (main thread, about 0.1 ms for 126 000 cells).
        WorldStats Stats() const;
        /// @brief Cheap (per column) test of what LoadBlocking waits for, for the current window.
        bool IsFilled(LoadScope scope) const;
        /// @brief Columns inside 0.85 x the view radius whose surface is not loaded (last Update).
        uint32_t UnfilledColumns() const { return m_Counters.unfilledColumns; }

        // --- window (shared with the renderer and the tracer) ----------------------------
        const glm::ivec3& WindowSize() const { return m_Config.windowSize; }
        const glm::ivec3& WindowMinChunk() const { return m_WindowMin; }
        /// @brief World position (metres) of the window's minimum corner: the origin of "render space".
        glm::dvec3 WindowOrigin() const;
        /// @brief One entry per cell (see trace.glsl for the encoding).
        std::span<const uint32_t> Grid() const { return std::span<const uint32_t>(m_Grid).first(m_CellCount); }
        /// @brief What the shaders read: Grid() (column-major: (x + z * size.x) * size.y + y, each
        /// toroidal), then one column-top word per column (x + z * size.x: window rows >= the value
        /// are air in that column), then the content top (the largest column top), then one region
        /// top per 4 x 4 columns of the window (window-relative, rx + rz * ceil(size.x / 4): the
        /// largest column top of the region; rebuilt when the window moves).
        std::span<const uint32_t> GridWords() const { return m_Grid; }
        /// @brief Changes whenever GridWords() or WindowMinChunk() change.
        uint64_t GridRevision() const { return m_GridRevision; }
        /// @brief Unique per World object of the process (a new World at the same address differs).
        uint64_t InstanceId() const { return m_InstanceId; }
        /// @brief For a cell whose grid word is dense (slot + 1): the word it had before it became
        /// dense (0, VX_GRID_PENDING, or uniform / packed). What a renderer shows while the slot
        /// is not on the GPU yet. Undefined for other cells.
        uint32_t GridFallback(uint32_t cellIndex) const { return m_Fallback[cellIndex]; }
        /// @brief Words per page of GridPageRevisions().
        static constexpr uint32_t kGridPageWords = 1024;
        /// @brief Per 4 KiB page of GridWords(): the GridRevision() at which it last changed.
        std::span<const uint64_t> GridPageRevisions() const { return m_GridPages; }
        /// @brief True if the window has a fixed vertical range (WorldConfig::fullHeight with known bounds).
        bool FullHeight() const { return m_FullHeight; }
        /// @brief World height (metres) above which nothing may be built: the top of the fixed
        /// range, +infinity without one.
        double BuildCeiling() const;
        /// @brief Horizontal distance (metres) from the focus within which every column is inside
        /// the window, whatever the focus' position inside its chunk.
        double ViewRadius() const;
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
        /// @brief Dense chunk data, or nullptr if the chunk is empty / uniform / packed / not
        /// loaded. `uniformId` is set for Uniform chunks only (0 otherwise, Packed included).
        const Chunk* GetChunk(const glm::ivec3& chunk, uint8_t* uniformId = nullptr) const;
        /// @brief Copies the content of any loaded chunk into `out` (a Packed one is decoded,
        /// without making it resident). False if the chunk is not loaded.
        bool ReadChunk(const glm::ivec3& chunk, Chunk& out) const;
        /// @brief The RLE bytes of a Packed chunk (EncodeChunk format), empty otherwise.
        std::span<const uint8_t> PackedData(const glm::ivec3& chunk) const;
        /// @brief Makes every Packed chunk of the box [minChunk, maxChunk] (inclusive) resident.
        /// Not a content change. Returns the number that could not be (store full).
        uint32_t EnsureResident(const glm::ivec3& minChunk, const glm::ivec3& maxChunk);
        /// @brief Loads the chunks of the box [minChunk, maxChunk] (inclusive) that lie in the
        /// window before any other chunk except those near the focus (an edit site whose
        /// surroundings must be complete: a blast's island search). At most 16 boxes are kept
        /// (the oldest goes); a box is dropped once every chunk of it inside the window is loaded.
        void RequestLoad(const glm::ivec3& minChunk, const glm::ivec3& maxChunk);
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
        /// @brief CarveSphereQ restricted to one chunk: the same decision for every voxel of the
        /// sphere inside `chunk`, in the same (z, y, x) order, nothing outside it. Carving every
        /// chunk of the sphere this way in (y, z, x) order equals CarveSphereQ. Used to replay a
        /// carve on exactly the chunks it skipped (sim::RetryIslandSearches).
        uint32_t CarveSphereQInChunk(const SphereQ& sphere, uint16_t power, const glm::ivec3& chunk,
                                     std::vector<VoxelEdit>* removed = nullptr, EditReport* report = nullptr);
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

        /// @brief Chunks whose solid content changed (loaded, unloaded or edited) since the last
        /// call, each once, with the box of the voxels edits changed in it.
        std::vector<ChunkChange> TakeChunkChanges();
        /// @brief TakeChunkChanges, coordinates only (the same list: either call empties it).
        std::vector<glm::ivec3> TakeChangedChunks();

    private:
        struct Cell {
            glm::ivec3 coord{0};
            uint32_t slot = ChunkStore::kInvalidSlot;
            uint32_t generation = 0;        // bumped whenever the cell is re-targeted: stale jobs are ignored
            CellState state = CellState::Unloaded;
            uint8_t uniformId = 0;          // Uniform: the id; Packed: the id its grid word shows
            uint8_t packedId = 0;           // Dense with kept RLE bytes: the id it shows when packed again
            bool modified = false;          // differs from generator output: must be saved
            bool changeQueued = false;      // m_ChangedChunks[changeIndex] is this chunk's entry
            uint32_t changeIndex = 0;
            bool assigned = false;          // coord is valid
            bool fromSave = false;          // loaded from the RegionStore (an edit of an earlier visit)
        };
        /// One column of cells (fixed chunk x, z): classification and loading state.
        struct Column {
            glm::ivec2 coord{0};            // chunk x, z
            uint32_t generation = 0;        // bumped whenever the column is re-targeted
            enum class Class : uint8_t { None, Classifying, Known } classState = Class::None;
            bool assigned = false;
            ColumnClass cls;                // valid when Known (cls.known false: every row is band)
            uint64_t savedRows[2] = {0, 0}; // window rows outside the band with a saved chunk (window-relative)
            uint64_t surfaceRows[2] = {0, 0};   // band rows still to submit, surface band
            uint64_t caveRows[2] = {0, 0};      // band rows still to submit, buried
            uint16_t surfaceLeft = 0;       // surface band cells not loaded yet
            uint16_t top = 0;               // window rows >= top are air (mirrored in the grid words)
            uint16_t bucket = 0;            // horizontal distance from the focus in whole chunks
        };
        struct Shared;                      // state shared with worker jobs

        uint32_t CellIndex(const glm::ivec3& chunk) const;
        uint32_t ColumnIndex(int cx, int cz) const;
        Cell* FindCell(const glm::ivec3& chunk);
        const Cell* FindCell(const glm::ivec3& chunk) const;
        Column* FindColumn(int cx, int cz);
        const Column* FindColumn(int cx, int cz) const;
        glm::ivec3 WindowMinFor(const glm::ivec3& focusChunk) const;
        void Recenter(const glm::ivec3& newMin);
        void RetargetColumn(uint32_t columnIndex, const glm::ivec2& coord);
        void RetargetCell(uint32_t cellIndex, const glm::ivec3& coord);
        void Rebucket();
        void UnloadCell(uint32_t index);
        void FreeGarbage();
        void PersistCell(Cell& cell);
        void SetGrid(uint32_t index, const Cell& cell);
        void WriteGrid(uint32_t index, uint32_t entry);
        void UpdateColumnTop(uint32_t columnIndex, int row, bool nonEmpty);
        void RefreshContentTop();
        void RebuildRegionTops();
        uint32_t RegionOfColumn(uint32_t columnIndex) const;
        void ApplyColumnClass(uint32_t columnIndex);
        void PlaceCell(uint32_t cellIndex, Column& column, int relRow);
        bool IsBandRow(const Column& column, int row) const;
        bool IsSurfaceRow(const Column& column, int row) const;
        bool NeighboursKnown(const Column& column, bool submitMissing);
        /// The geometric rule. `unknownVisible`: a neighbour column that is not classified yet
        /// makes the chunk visible (true, the safe answer) or is ignored (false: it will re-check
        /// the chunk itself once it is classified).
        bool Visible(const Cell& cell, bool unknownVisible = true) const;
        /// A face neighbour is an edit (modified, or loaded from a save): a ray reaches this
        /// chunk through it.
        bool FaceNeighbourEdited(const glm::ivec3& chunk) const;
        double DistanceToFocus(const glm::ivec3& chunk) const;
        double HorizontalDistanceToFocus(int cx, int cz) const;
        double NearSlack() const;
        void RecheckNeighbours(uint32_t columnIndex);
        void ProcessReveals();
        void Repack();
        void DropKeptBytes(uint32_t index);
        /// Reports the cell's chunk as changed (once until the next take); its entry, valid until
        /// the next call.
        ChunkChange& QueueChange(Cell& cell);
        bool Materialize(uint32_t index);
        bool Unpack(uint32_t index);
        void PromoteNear();
        void PromoteFaceNeighbours(const glm::ivec3& chunk);
        void IntegrateResults(uint32_t budget);
        void SubmitJobs();
        bool SubmitRequestedBoxes();
        bool SubmitColumnJob(uint32_t columnIndex);
        bool SubmitCellJob(uint32_t cellIndex);
        bool SubmitColumnRows(uint32_t columnIndex, bool caves, bool nearOnly);
        bool LoadComplete(LoadScope scope) const;
        static void LoadJob(const std::shared_ptr<Shared>& shared, uint32_t cellIndex, glm::ivec3 coord, uint32_t generation, bool pack);
        static void ColumnJob(const std::shared_ptr<Shared>& shared, uint32_t columnIndex, glm::ivec2 coord, uint32_t generation,
                              int rowMin, int rows);
        /// Every row of voxels may change (the default row filter of EditRegion).
        struct AnyRow {
            bool operator()(const glm::ivec3&, int) const { return true; }
        };
        /// `rows(first voxel, length)` false skips a row of voxels along +x before any id is read:
        /// a sparse mask (ClearMask) then costs a few byte tests per row of its box.
        template <typename Fn, typename Touches, typename Rows = AnyRow>
        uint32_t EditRegion(const glm::ivec3& min, const glm::ivec3& max, Fn&& decide, Touches&& touches,
                            std::vector<VoxelEdit>* edits, EditReport* report, Rows rows = {});

        WorldConfig m_Config;
        ChunkStore& m_Store;
        JobSystem& m_Jobs;
        const MaterialLibrary* m_Materials;
        std::shared_ptr<Shared> m_Shared;

        bool m_FullHeight = false;
        bool m_Pack = false;
        uint32_t m_CellCount = 0;
        uint32_t m_ColumnCount = 0;
        std::vector<Cell> m_Cells;
        std::vector<std::vector<uint8_t>> m_Packed;   // per cell: RLE bytes of a Packed chunk
        std::vector<Column> m_Columns;
        std::vector<uint32_t> m_Grid;                 // cells, column tops, content top
        std::vector<uint64_t> m_GridPages;
        uint64_t m_GridRevision = 1;
        bool m_ContentTopStale = false;
        bool m_RegionTopsStale = false;
        uint32_t m_RegionsX = 0, m_RegionCount = 0;    // 4 x 4 column regions (window-relative)
        std::vector<uint32_t> m_RegionScratch;
        glm::ivec3 m_WindowMin{0};
        glm::ivec3 m_FocusChunk{0};
        glm::dvec3 m_Focus{0.0};
        bool m_HasWindow = false;
        bool m_NearPromotePending = false;
        glm::dvec3 m_NearScanFocus{0.0};               // focus of the last PromoteNear scan
        uint32_t m_DecodeBudget = 0;                   // decodes left in this Update
        uint64_t m_InstanceId = 0;
        std::vector<uint32_t> m_Fallback;              // per cell: grid word before it became dense
        std::vector<uint32_t> m_Resident;              // Dense cells that keep RLE bytes (re-pack candidates)
        size_t m_ResidentCursor = 0;
        std::vector<uint32_t> m_Reveal;                // Packed cells found visible: decode them
        struct Box {
            glm::ivec3 min{0}, max{0};
        };
        std::vector<Box> m_Requested;                  // RequestLoad boxes, oldest first

        std::vector<uint32_t> m_BucketOrder;           // column indices by horizontal distance from the focus
        std::vector<uint32_t> m_BucketStart, m_BucketCursor;
        std::vector<uint16_t> m_BucketOfD2;            // floor(sqrt(d^2)) for d^2 in chunks^2
        uint32_t m_ColumnsWithWork = 0;                // columns not classified or with rows to submit
        uint32_t m_MaxJobs = 8;

        WorldStats m_Counters;                         // the cumulative / last-Update fields of Stats()
        std::vector<ChunkChange> m_ChangedChunks;
        std::unique_ptr<Chunk> m_Scratch;              // decode target for edits of Packed chunks
        struct Garbage {
            std::vector<std::unique_ptr<Chunk>> chunks;
            std::vector<std::vector<uint8_t>> bytes;
        };
        Garbage m_Garbage;                             // memory of unloaded chunks, released on a worker
    };
}
