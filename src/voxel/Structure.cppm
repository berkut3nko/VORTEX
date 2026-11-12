module;

#include <glm/glm.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

export module vortex.voxel:structure;

import :chunk;
import :volume;
import :world;
import :material;
import :connectivity;

export namespace vortex::voxel {

    // --- the cell graph of a terrain chunk (docs/design/shred.md stage 4, collapse.md) -------------

    inline constexpr int kCellSize = 8;                   ///< Voxels per cell edge: ChunkOccupancy::cellMask granularity.
    inline constexpr int kCellsPerChunkAxis = kChunkSize / kCellSize;
    inline constexpr int kCellsPerChunk = kCellsPerChunkAxis * kCellsPerChunkAxis * kCellsPerChunkAxis;
    /// collapse.md's SupportCell held at most this many nodes per cell (more merged into node 7 and
    /// over-connected). Here every component is its own node; a cell with more is "over-full",
    /// counted (`ChunkGraph::overfullCells`, `GraphSearchResult::overfullCells`) and exact.
    inline constexpr uint32_t kMaxCellNodes = 8;

    /// Faces of a cell, in the fixed neighbour order of every traversal (-x +x -y +y -z +z).
    enum CellFace : uint8_t { kFaceNegX, kFacePosX, kFaceNegY, kFacePosY, kFaceNegZ, kFacePosZ };

    /// Bond classes of the load check (docs/design/shred.md Part 4b), indices of
    /// Material::strength and GraphNode::weak: a node resting on the one below (its -y face) loads
    /// the bond in compression, one hanging from the one above (+y) in tension, x and z in shear.
    enum StrengthClass : uint8_t { kCompression = 0, kShear = 1, kTension = 2 };
    /// The class of a bond crossed from a node through its face `face`.
    constexpr uint8_t StrengthClassOf(uint8_t face) { return face == kFaceNegY ? kCompression : face == kFacePosY ? kTension : kShear; }
    inline constexpr uint8_t kNoCut = 255;          ///< GraphNode::cutFaces of a node that is never split.
    inline constexpr uint8_t kCutAnchorBit = 0x80;  ///< GraphNode::cutPlane: the node holds an anchor voxel.

    /**
     * @brief One node: a 6-connected component of solid (not ignored) voxels inside one 8^3 cell.
     * @details Face masks hold the node's voxels on each face of its cell, 8 x 8 bits: an x face
     * bit y + 8z, a y face bit x + 8z, a z face bit x + 8y, so the masks of two cells that share a
     * face line up: two nodes are linked exactly when `a.face[f] & b.face[opposite f]` is not 0,
     * and its popcount is their bond (voxel faces shared). Voxel count, mass, bonds, the weakest
     * material per bond class and the narrowest horizontal plane are what the load check reads
     * (docs/design/shred.md Part 4b: "a tower on a 4-voxel neck must not hold").
     */
    struct GraphNode {
        uint64_t face[6] = {};
        /// Material-weighted mass: the sum of GraphMaterials::densityQ (kg/m^3, integer) over its
        /// voxels; kilograms = mass x voxel size^3 (foliage weighs 0).
        uint32_t mass = 0;
        uint16_t voxels = 0;          ///< 1..512.
        uint16_t firstVoxel = 0;      ///< Cell-local x + 8y + 64z of its first voxel in scan order (z, then y, then x).
        uint16_t firstLink = 0;       ///< Its links inside the chunk: ChunkGraph::links[firstLink, firstLink + linkCount).
        uint16_t linkCount = 0;
        /// Voxel faces of the node across its narrowest horizontal plane, where the load check
        /// splits it into a lower and an upper half (1..63), or kNoCut: not constricted there, or
        /// a half would not be 6-connected (shred.md Part 4b, "The narrowest plane").
        uint8_t cutFaces = kNoCut;
        /// Bits 0-2: the plane, between cell-local layers y = k and k + 1 (with cutFaces != kNoCut).
        /// kCutAnchorBit: a voxel of the node is an anchor material (GraphMaterials::anchor).
        uint8_t cutPlane = 0;
        uint8_t cell = 0;             ///< cx + 4 cy + 16 cz.
        uint8_t boxMin[3] = {};       ///< Cell-local bounds of its voxels, inclusive.
        uint8_t boxMax[3] = {};
        /// Per StrengthClass the id of its weakest material (lowest strength, ties to the smaller
        /// id), foliage left out unless the node holds nothing else: a bond is as strong as the
        /// weaker material on either side.
        uint8_t weak[3] = {};
    };
    static_assert(sizeof(GraphNode) == 72);

    /// @brief A link between two nodes of the same chunk (links across a chunk face are computed
    /// from the face masks when a traversal needs them: BondVoxels).
    struct GraphLink {
        uint16_t node = 0;            ///< The other node, in the same ChunkGraph.
        uint8_t face = 0;             ///< CellFace of this node it crosses.
        uint8_t reserved = 0;
        uint16_t bond = 0;            ///< Voxel faces shared, 1..64.
    };

    /**
     * @brief The nodes of one terrain chunk: its 64 cells, every component of every cell.
     * @details Nodes are ordered by cell index, inside a cell by first voxel, so rebuilding an
     * unchanged chunk gives the same graph (TerrainGraphCache relies on it, `Structure.RefreshEqualsRebuild`).
     * Links of a node are ordered by face, then by the other node. A pure function of the chunk's
     * ids and the material tables.
     */
    struct ChunkGraph {
        /// The chunk it was built for. TerrainGraphCache shares one graph between every Uniform
        /// chunk of one id (and every empty chunk): there it is (0, 0, 0).
        glm::ivec3 chunk{0};
        std::vector<GraphNode> nodes;
        std::vector<GraphLink> links;
        std::array<uint16_t, kCellsPerChunk + 1> cellFirst{};   ///< Nodes of cell c: [cellFirst[c], cellFirst[c + 1]).
        uint32_t overfullCells = 0;                   ///< Cells with more than kMaxCellNodes nodes.
        uint32_t solidVoxels = 0;
    };

    /// Voxel faces two nodes share across `face` of `a` (0: not linked).
    inline uint32_t BondVoxels(const GraphNode& a, const GraphNode& b, uint8_t face) {
        return static_cast<uint32_t>(std::popcount(a.face[face] & b.face[face ^ 1u]));
    }

    /// @brief What a graph is built from per material id: whether it is ignored (liquids: neither
    /// support nor part of a piece, like the island search), its integer density, its bond
    /// strengths and whether it is an anchor (never fails, always supports).
    struct GraphMaterials {
        std::array<bool, 256> ignored{};
        /// Indestructible and not ignored (bedrock): a node holding one is an anchor of the load check.
        std::array<bool, 256> anchor{};
        /// MaterialFlag_Foliage (leaves): weightless, and left out of a node's weakest material
        /// (GraphNode::weak) when the node holds any other solid id: a leaf touching a wall does not
        /// rate the wall (shred.md "Stage 4b review").
        std::array<bool, 256> foliage{};
        /// kg/m^3, rounded, saturated at 65535; 0 for air, ignored ids and foliage (weightless for
        /// the load check: a canopy never overloads its trunk).
        std::array<uint16_t, 256> densityQ{};
        /// Material::strength per StrengthClass, kg/m^2.
        std::array<std::array<uint32_t, 256>, 3> strength{};
        bool operator==(const GraphMaterials&) const = default;
    };
    /// @brief The tables for `materials` (kWorldPalette) and `ignoreFlags`; without a library
    /// nothing is ignored, every density is 1000 and every strength stone's (Material's default).
    GraphMaterials MakeGraphMaterials(const MaterialLibrary* materials, uint32_t ignoreFlags);

    /// @brief Builds the graph of a chunk from its voxels (`chunk`), or of a Uniform chunk of
    /// `uniformId` when `chunk` is null (0 or an ignored id: no nodes). The vectors hold no
    /// spare capacity afterwards (ChunkGraphBytes is what the graph keeps).
    void BuildChunkGraph(ChunkGraph& out, const glm::ivec3& coord, const Chunk* chunk, uint8_t uniformId, const GraphMaterials& materials);

    /// @brief The voxels of one node: 8 z slices of its cell, bit x + 8y (re-labelled from the
    /// chunk's voxels, the same rule BuildChunkGraph used).
    std::array<uint64_t, 8> NodeVoxels(const Chunk* chunk, uint8_t uniformId, const GraphNode& node, const GraphMaterials& materials);

    /// @brief Heap bytes a graph holds (the struct and its vectors' capacity).
    size_t ChunkGraphBytes(const ChunkGraph& graph);

    /// @brief What a TerrainGraphCache did. Depends on what was cached, loaded and evicted before:
    /// logged only, never an input to a decision or a work budget (docs/design/networking.md R5).
    struct TerrainGraphStats {
        uint64_t built = 0;           ///< Graphs built (a Dense / Packed chunk seen for the first time or changed since).
        uint64_t shared = 0;          ///< Lookups of Uniform / empty chunks answered with the graph shared per id.
        uint64_t reused = 0;          ///< Lookups answered from the cache.
        uint64_t evicted = 0;         ///< Entries dropped: least recently used, or stale / unloaded when over the cap.
        uint64_t decoded = 0;         ///< Packed chunks decoded to build a graph.
        uint64_t bytes = 0;           ///< Heap bytes held now: graphs, shared graphs and map entries (est. 64 B each).
        uint64_t peakBytes = 0;
        double buildMs = 0.0;         ///< Wall clock of the builds: logged only.
    };

    /**
     * @brief Chunk graphs of a World, built lazily (the first search that needs a chunk) and kept
     * per chunk while World::ChunkRevision does not change: an edit, a load or an unload makes the
     * next lookup rebuild that chunk only. Main thread, like the World.
     * @details Which chunks are cached never changes a result: a graph is a pure function of the
     * chunk's voxels (`Structure.RefreshEqualsRebuild`, `Structure.EvictionNeverChangesAGraph`).
     * Dense and Packed chunks own their graph; every Uniform chunk of one id shares one graph,
     * built once per material table (an empty chunk or a uniform liquid shares the empty one). At
     * most `maxChunks` entries are kept: when a pass starts over the cap, entries whose chunk
     * changed or unloaded go first, then the least recently used.
     */
    class TerrainGraphCache {
    public:
        explicit TerrainGraphCache(size_t maxChunks = 4096);
        ~TerrainGraphCache();
        TerrainGraphCache(const TerrainGraphCache&) = delete;
        TerrainGraphCache& operator=(const TerrainGraphCache&) = delete;

        /// @brief Starts a lookup pass (one island search): the use stamp for eviction. Drops every
        /// graph if `world` or `materials` differ from the last pass.
        void BeginPass(const World& world, const GraphMaterials& materials);
        /// @brief The current graph of a loaded chunk, or nullptr if it is not loaded. Valid until
        /// the next Get of another pass or Clear.
        const ChunkGraph* Get(const World& world, const glm::ivec3& chunk);
        void Clear();
        size_t Size() const { return m_Entries.size(); }
        const TerrainGraphStats& Stats() const { return m_Stats; }
        const GraphMaterials& Materials() const { return m_Materials; }
        /// @brief Tests only: keeps `graph` as the chunk's graph at its current revision, as if it
        /// had been built from it (a stale graph: what an edit path that missed
        /// World::ChunkRevision would leave). Call after BeginPass.
        void InjectForTesting(const World& world, const glm::ivec3& chunk, ChunkGraph graph);

    private:
        struct Entry {
            std::unique_ptr<ChunkGraph> own;          ///< Dense / Packed chunks; null for a shared graph.
            const ChunkGraph* graph = nullptr;        ///< `own`, or the graph shared by its uniform id.
            glm::ivec3 chunk{0};
            uint64_t revision = 0;                    ///< World::ChunkRevision it was built from.
            uint64_t lastUse = 0;
        };
        void Evict(const World& world);
        const ChunkGraph* Shared(uint8_t uniformId);
        void SetOwn(Entry& entry, std::unique_ptr<ChunkGraph> graph);
        void AddBytes(int64_t delta);
        std::unordered_map<uint64_t, Entry> m_Entries;
        std::array<std::unique_ptr<ChunkGraph>, 256> m_Shared;   ///< Per uniform id ([0]: empty), built lazily.
        size_t m_MaxChunks;
        uint64_t m_Pass = 0;
        uint64_t m_WorldId = 0;
        GraphMaterials m_Materials;
        std::unique_ptr<Chunk> m_Decoded;
        TerrainGraphStats m_Stats;
    };

    // --- the load check (docs/design/shred.md Part 4b) ------------------------------------------------

    /// Strengths are kg/m^2 and masses kg/m^3 x voxels ("MU": kg = MU x vs^3); one voxel face of
    /// a material carries strength x vs^2 kg = strength x 1 000 000 / vsMicro MU.
    uint32_t VoxelSizeMicro(float voxelSize);       ///< llround(voxelSize x 1e6): 125 000 at 0.125 m.

    /// @brief The limits of the load check of one search (GraphSearchLimits::neck).
    struct NeckLimits {
        /// Per StrengthClass and material id: what one voxel face carries, MU (MakeNeckLimits).
        std::array<std::array<uint64_t, 256>, 3> capFace{};
        uint32_t maxNetworkNodes = 8192;      ///< Judged nodes (halves counted) plus loads; above: skipped.
        uint32_t maxArcPairs = 65535;         ///< Arc pairs of the network; above: skipped.
        uint32_t maxExteriorVisits = 16384;   ///< Node visits of the exterior floods; then every flood is an anchor.
        uint32_t maxFlowWork = 262144;        ///< Arc scans of the maximum preflow; spent: nothing released.
        /// Released parts and voxels per check (shred.md "Stage 4b review"): parts are taken in piece
        /// order while both fit; the first that does not and every later one stay (counted in
        /// `neckDeferredParts`; still overloaded, a later cast near them releases them).
        uint32_t maxReleasedVoxels = 60000;
        uint32_t maxReleasedParts = 64;
        /// Tests only: a non-zero seed permutes the arc order of every network node and the first
        /// queue of the preflow (the released set must not change: `Neck.ReleasedSetIndependentOfArcOrder`).
        uint32_t shuffleForTesting = 0;
    };
    /// @brief capFace[k][id] = strength[k][id] x 1 000 000 / VoxelSizeMicro(voxelSize), floor;
    /// the budgets at their defaults.
    NeckLimits MakeNeckLimits(const GraphMaterials& materials, float voxelSize);

    /// @brief One part the load check released (or found overloaded and left, `oversize`).
    struct NeckCut {
        uint64_t load = 0;        ///< MU: the mass of the part (exterior loads hanging on it included).
        uint64_t cut = 0;         ///< MU: what the bonds out of the part carry (load > cut, strictly).
        uint32_t faces = 0;       ///< Voxel faces of those bonds.
        uint32_t voxels = 0;
        bool insideCell = false;  ///< Some of the cut runs through the narrowest plane of a split node.
    };

    /// @brief An abstract load network (tests; the search builds its own the same way): node
    /// supplies in MU, arc pairs with a capacity each way, arcs to the sink (`to == kNeckSink`).
    struct NeckNetwork {
        static constexpr uint32_t kNeckSink = 0xFFFFFFFFu;
        static constexpr uint64_t kInfinite = ~uint64_t{0};
        struct Arc {
            uint32_t from = 0, to = 0;
            uint64_t cap = 0;         ///< from -> to, MU (kInfinite: never saturates).
            uint64_t capBack = 0;     ///< to -> from (ignored for an arc to the sink).
        };
        std::vector<uint64_t> supply;
        std::vector<Arc> arcs;
        uint32_t shuffleSeed = 0;     ///< As NeckLimits::shuffleForTesting.
    };
    struct NeckFlowResult {
        bool solved = false;              ///< false: maxWork spent (nothing may be released).
        std::vector<uint8_t> released;    ///< Per node: 1 = in the largest strictly overloaded set.
        uint64_t work = 0;                ///< Arc scans.
    };
    /// @brief The load check's rule on a network: a maximum preflow on the perturbed capacities
    /// (c x 2^17 + 1, supplies x 2^17), released = the nodes that cannot reach the sink in its
    /// residual graph: the largest set A with mass(A) > cap(A -> out), unique whatever the order.
    NeckFlowResult SolveNeckNetwork(const NeckNetwork& network, uint32_t maxWork);

    // --- the island search on the graph -------------------------------------------------------------

    struct GraphSearchLimits {
        glm::ivec3 boxMin{0}, boxMax{0};       ///< The island box [min, max) (sim::IslandBox), at most 192 per axis.
        glm::ivec3 floodMin{0}, floodMax{0};   ///< The flood box [min, max) (sim::FloodBox): holds the island box, at most 192 per axis.
        uint32_t maxIslandVoxels = 0;          ///< A component with more voxels is never an island.
        /// Work budget of the call in node visits (docs/design/shred.md R5): once spent, the flood
        /// running and every later one stay attached (`attachedByBudget`).
        uint32_t maxNodeVisits = 0;
        /// A chunk a flood needs that is not loaded: true (offline) the face islands are not
        /// returned (`complete` false, the caller waits for `unloadedChunk`); false (session
        /// server) that flood is attached and `attachedByUnloaded` counts an error.
        bool deferOnUnloaded = true;
        /// The load check (docs/design/shred.md Part 4b) after the island floods, or nullptr: off.
        const NeckLimits* neck = nullptr;
    };

    struct GraphSearchResult {
        /// false: a chunk of the island box was not loaded; nothing was searched (as ExtractFloatingIslands).
        bool searched = false;
        /// false (offline): a flood met a chunk that is not loaded; `faceIslands` is empty,
        /// `boxIslands` is valid.
        bool complete = true;
        glm::ivec3 unloadedChunk{0};
        /// Islands inside the island box that do not touch its face, in the order of their first
        /// voxel in box scan order: what ExtractFloatingIslands returns.
        std::vector<FloatingIsland> boxIslands;
        /// Islands that cross the island box face, in the order the face flood proves them
        /// (smallest in-box component first, ties by its first voxel): what ProveFaceComponents returns.
        std::vector<FloatingIsland> faceIslands;
        glm::ivec3 labelledMin{0}, labelledMax{0};   ///< The island box without rows below a full-height window.

        uint32_t seedNodes = 0;            ///< Nodes whose bounds meet the island box.
        uint32_t boxComponents = 0;        ///< Their components over links between them.
        uint32_t floods = 0;               ///< Box components flooded through the flood box.
        uint32_t merged = 0;               ///< Box components already inside an earlier island.
        uint32_t attachedByLimit = 0;      ///< A flood passed maxIslandVoxels.
        uint32_t attachedByFace = 0;       ///< A flood reached a node on the flood box face (or the window's bottom row).
        uint32_t attachedByKnown = 0;      ///< A flood reached a node an earlier one gave up on.
        uint32_t attachedByBudget = 0;
        uint32_t attachedByUnloaded = 0;
        uint32_t faceOversize = 0;         ///< Box components over maxIslandVoxels that reach the box face: attached, not flooded.
        uint32_t oversizeIslands = 0;      ///< Floating inside the box but over maxIslandVoxels: they stay (counted).
        uint32_t outsideBox = 0;           ///< Closed floods with no voxel inside the island box (never islands of this blast).
        uint32_t nodeVisits = 0;           ///< Nodes enqueued by the floods (the budget's unit).
        /// Chunk graphs (with nodes) the search used: a function of the voxels in the flood box,
        /// whatever was cached. The upper bound of the graphs a cold search builds.
        uint32_t chunkGraphs = 0;
        /// Cells with more than kMaxCellNodes nodes in those graphs (summed per search: a cell
        /// seen by two searches counts twice).
        uint32_t overfullCells = 0;
        uint32_t relabelledNodes = 0;      ///< Nodes of islands re-labelled per voxel.
        /// Packed chunks decoded to re-label island voxels. Which chunks are Packed depends on
        /// streaming: logged only, never a budget input.
        uint32_t chunksDecoded = 0;
        /// Island nodes whose voxels, re-labelled now, differ from their graph's node in count or
        /// face masks (a stale graph: an edit path missed World::ChunkRevision): an error, logged,
        /// summed over both runs. The search then drops the cache and runs once more on fresh
        /// graphs (`staleGraphRetries`; the other counters are the second run's). If that still
        /// differs, no island is returned (everything stays attached). Only island nodes are
        /// checked: a stale attached neighbour is not seen (the revision rule is the guard).
        uint32_t relabelMismatch = 0;
        uint32_t staleGraphRetries = 0;
        /// Graphs (re)built for this search. Depends on what the cache held (earlier searches,
        /// eviction, loading order): logged only, never a budget input. `chunkGraphs` is the
        /// count that follows from the voxels.
        uint64_t graphsBuilt = 0;
        /// Wall clock, logged only: seeds and box components (graph builds included), floods, re-labelling.
        double seedMs = 0.0, floodMs = 0.0, relabelMs = 0.0;

        // The load check (GraphSearchLimits::neck). Every counter follows from the voxels of the
        // flood box: equal cold or warm, with any number of workers.
        /// Parts released by the check, in piece order (smallest network index first), and what
        /// each held (`neckCuts`, same order). They may reach past the island box.
        std::vector<FloatingIsland> neckPieces;
        std::vector<NeckCut> neckCuts;
        bool neckRan = false;              ///< The check ran (searched, complete, `neck` given).
        uint32_t neckNodes = 0;            ///< Network nodes: judged nodes (a split node counts twice) and loads.
        uint32_t neckSplit = 0;            ///< Judged nodes split at their narrowest plane.
        uint32_t neckLoads = 0;            ///< Closed exterior floods: loads hanging on judged nodes.
        uint32_t neckAnchors = 0;          ///< Exterior floods that ended as anchors.
        uint32_t neckArcs = 0;             ///< Arc pairs.
        uint32_t neckExteriorVisits = 0;
        uint32_t neckFlowWork = 0;         ///< Arc scans of the preflow.
        uint32_t neckSplitRelabels = 0;    ///< Split nodes re-labelled per voxel for their halves' mass.
        uint32_t neckVoxels = 0;           ///< Voxels released.
        uint32_t neckOversize = 0;         ///< Overloaded parts over maxIslandVoxels: they stay (counted).
        /// Overloaded parts left for a later cast by NeckLimits::maxReleasedVoxels / maxReleasedParts.
        uint32_t neckDeferredParts = 0;
        /// Exterior floods that ended on entering a chunk that is solid through (32^3 solid voxels,
        /// whatever its storage): ground, an anchor without reading further (shred.md "Stage 4b review").
        uint32_t neckSolidChunks = 0;
        uint32_t neckSkipped = 0;          ///< Network over maxNetworkNodes or maxArcPairs: nothing judged.
        uint32_t neckExteriorBudget = 0;   ///< Exterior floods ended (or never run) by maxExteriorVisits: anchors.
        uint32_t neckFlowBudget = 0;       ///< maxFlowWork spent: nothing released.
        uint32_t neckUnloaded = 0;         ///< Server: an exterior flood met a chunk that is not loaded (an anchor; an error).
        /// Offline: the check met a chunk outside the world window (it cannot load before the
        /// window moves) / a chunk of the window still loading. Either is taken as support for this
        /// check, which can only release less than the full answer (never something that holds),
        /// and the check asks to run again once `neckWaitChunk` (the first one met) has loaded
        /// (`neckWaits`; sim::Blast queues a late search). The islands are not held back.
        uint32_t neckBeyondWindow = 0;
        uint32_t neckLoading = 0;
        bool neckWaits = false;
        glm::ivec3 neckWaitChunk{0};
        /// Wall clock, logged only: the check without the chunk graphs it built (`neckBuildMs`).
        double neckMs = 0.0, neckBuildMs = 0.0;
    };

    /**
     * @brief The island search of a blast on the cell graph (docs/design/shred.md stage 4): the same
     * islands, in the same order, as ExtractFloatingIslands over the island box followed by
     * ProveFaceComponents inside the flood box, except where a work budget runs out and, when a
     * chunk of the flood box is not loaded, in whether the face islands wait (`complete`) or why
     * a flood counts as attached: the two searches flood in another order, so one may meet the
     * unloaded chunk before the other closes or reaches the face.
     * @details Seeds are the nodes whose bounds meet the island box (rows below a full-height
     * window cut off, rows above the build ceiling air); their components over links between seeds
     * ("box components") are flooded smallest first (node-weighted voxel count, ties by first
     * seed), breadth-first over nodes, 6 faces in fixed order, inside the flood box: a flood that
     * reaches a node with a voxel on the flood box face (or the window's bottom row) is attached,
     * one that passes maxIslandVoxels voxels too; a closed flood is an island. A box component
     * entirely inside the box interior is closed without a flood. Island nodes are re-labelled per
     * voxel to build the island volumes. Integer-only, a fixed order: equal voxels in the flood
     * box give equal results, whatever was cached or loaded outside it. Changes nothing in the world.
     * With `limits.neck` the load check runs after the floods (docs/design/shred.md Part 4b): the
     * nodes of the island box that are not islands are judged, what hangs on them inside the
     * flood box is a load, what reaches the flood box face (or an anchor material) supports; the
     * largest strictly overloaded set is returned as `neckPieces` (capped per call:
     * NeckLimits::maxReleasedParts / maxReleasedVoxels). An exterior flood that enters a chunk solid
     * through is ground (`neckSolidChunks`). A chunk the check needs that is not loaded is support:
     * a server counts an error (`neckUnloaded`); offline (`deferOnUnloaded`) it is counted
     * (`neckLoading`, `neckBeyondWindow`) and `neckWaits` asks the caller to run the search again
     * once `neckWaitChunk` has loaded. Unlike a face flood it never holds back the islands.
     */
    GraphSearchResult SearchIslandsOnGraph(const World& world, TerrainGraphCache& cache, const GraphMaterials& materials,
                                           const GraphSearchLimits& limits);
}
