module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

export module vortex.sim:blast;

import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import :simulation;
import :worldop;

export namespace vortex::sim {

    /// @brief A blast as a game asks for it: world metres and material hardness (quantised by Blast).
    struct BlastDesc {
        glm::dvec3 center{0.0};   ///< World metres.
        float radius = 1.0f;      ///< Metres.
        float power = 1.0f;       ///< Terrain voxels harder than this survive (Material::hardness units).
    };

    /// @brief One operation of a blast as it was applied to the blasting simulation's world.
    struct AppliedOp {
        WorldOp op;
        OpResult result;
        /// Voxels the operation changed, in ApplyOp order: the carved voxels (particles), the
        /// voxels of a detached piece (BuildDetachedPiece) or the crumbs of the ClearMask
        /// (particles). Presentation data, never sent.
        std::vector<voxel::VoxelEdit> edits;
    };

    /// @brief Wall-clock cost of the simulation half of a blast, milliseconds (docs/design/shred.md
    /// stage 0). Logged only: nothing is ever decided from a time.
    struct BlastTimes {
        double carveMs = 0.0;      ///< The CarveSphere (or its replay in a late search).
        /// The island search: the graph search (seeds, floods, re-labelling; graph builds included),
        /// or with the voxel search the island box gathered and labelled (ExtractFloatingIslands).
        double searchMs = 0.0;
        double graphBuildMs = 0.0; ///< Graph search: chunk graphs (re)built, part of searchMs.
        double graphFloodMs = 0.0; ///< Graph search: the floods over nodes, part of searchMs.
        double relabelMs = 0.0;    ///< Graph search: island nodes re-labelled per voxel and ordered, part of searchMs.
        double neckMs = 0.0;       ///< Graph search: the load check (shred.md Part 4b) without its graph builds, part of searchMs.
        double neckBuildMs = 0.0;  ///< Graph search: chunk graphs the load check built (also in graphBuildMs).
        double floodMs = 0.0;      ///< Voxel search: the face flood (voxel::ProveFaceComponents). Graph search: 0.
        double opsMs = 0.0;        ///< Detach / ClearMask built and applied.
        double objectsMs = 0.0;    ///< Scene::Damage and the push.
        /// SimulationConfig::checkIslandSearch only: the other search (not in the total; nothing
        /// it found was applied).
        double referenceMs = 0.0;
    };

    /// @brief What the island search of a blast saw (docs/design/shred.md stage 1c: nothing
    /// silent). Counts of work and decisions; equal on every machine for equal voxels (and equal
    /// loaded chunks in the flood box), except `chunksDecoded` and `graphsBuilt`, which depend on
    /// streaming and on what the graph cache held: those two are logged only and never an input
    /// to a decision or a work budget (docs/design/networking.md R5).
    struct IslandSearchStats {
        /// The search that ran: the cell graph (SimulationConfig::islandSearch Graph, the default)
        /// or the voxel search. Counters marked "voxel" stay 0 for the graph and the other way round.
        bool graph = false;
        uint64_t boxVoxels = 0;          ///< Voxel: island box voxels gathered.
        uint32_t faceCandidates = 0;     ///< Voxel: face components of at most kMaxIslandVoxels in the box: flooded.
        uint32_t facePieces = 0;         ///< Proven by the flood and detached (a Detach each).
        uint32_t faceCrumbs = 0;         ///< Proven by the flood, under the piece size: in the ClearMask.
        /// Face components over kMaxIslandVoxels in the box: attached, not flooded (graph: box
        /// components over it by whole-node voxel count).
        uint32_t faceOversize = 0;
        /// Floating components inside the box over kMaxIslandVoxels: they stay in the air.
        uint32_t oversizeIslands = 0;
        uint32_t merged = 0;             ///< Candidates (graph: box components) that were part of an earlier proven island.
        /// voxel::FloodResult / voxel::GraphSearchResult counters, summed. Voxel: limit = over the
        /// size limit, at the flood box face or below the window; graph: over the size limit only
        /// (the face is `attachedByFace`).
        uint32_t attachedByLimit = 0;
        uint32_t attachedByKnown = 0;
        uint32_t attachedByBudget = 0;
        /// A flood met a chunk that is not loaded on a session server: an error (its window holds
        /// every flood box by construction), logged; the candidate stayed attached.
        uint32_t attachedByUnloaded = 0;
        uint32_t chunksDecoded = 0;      ///< Packed chunks decoded: depends on streaming, logged only.
        uint32_t floodVisits = 0;        ///< Voxel: voxels visited by the face flood.
        // Graph search (voxel::GraphSearchResult):
        uint32_t seedNodes = 0;          ///< Nodes whose bounds meet the island box.
        uint32_t boxComponents = 0;      ///< Their components (over links between them).
        uint32_t graphFloods = 0;        ///< Box components flooded through the flood box.
        uint32_t attachedByFace = 0;     ///< A flood reached the flood box face (or the window's bottom row).
        uint32_t nodeVisits = 0;         ///< Nodes the floods visited (kGraphNodeVisitBudget's unit).
        /// Chunk graphs used (with nodes): follows from the voxels of the flood box, whatever was
        /// cached; what a work budget charges for graph builds (the cold worst case).
        uint32_t chunkGraphs = 0;
        /// Graphs (re)built for this search, the rest came from the cache. Depends on cache
        /// history (earlier blasts, eviction, loading order): logged only, never a budget input.
        uint32_t graphsBuilt = 0;
        /// Cells with more than voxel::kMaxCellNodes nodes in those graphs (collapse.md's merge
        /// would have over-connected them; here they are exact). Summed per graph used.
        uint32_t overfullCells = 0;
        uint32_t relabelledNodes = 0;    ///< Island nodes re-labelled per voxel.
        uint32_t outsideBox = 0;         ///< Closed floods without a voxel in the island box (not this blast's).
        /// SimulationConfig::checkIslandSearch: the other search found other islands (logged).
        uint32_t referenceMismatch = 0;
        /// A chunk of the island box was not loaded on a session server: an error (its window holds
        /// every island box by construction), logged; nothing was searched and nothing waits.
        uint32_t islandBoxUnloaded = 0;
        /// Offline: a flood met a chunk of the flood box that is not loaded. The face pieces were
        /// not emitted, the search waits for that chunk (RetryIslandSearches).
        bool floodDeferred = false;
        // The load check (docs/design/shred.md Part 4b; graph search with SimulationConfig::neckRule),
        // voxel::GraphSearchResult's counters. All follow from the voxels of the flood box.
        bool neckRan = false;            ///< The check ran in (one of) the search(es) of this result.
        uint32_t neckNodes = 0;          ///< Network nodes (judged nodes, halves counted, and loads).
        uint32_t neckSplit = 0;          ///< Judged nodes split at their narrowest plane.
        uint32_t neckLoads = 0;          ///< Exterior loads hanging on judged nodes.
        uint32_t neckAnchors = 0;        ///< Exterior floods that ended as anchors.
        uint32_t neckArcs = 0;           ///< Arc pairs.
        uint32_t neckExteriorVisits = 0;
        uint32_t neckFlowWork = 0;       ///< Arc scans of the preflow.
        uint32_t neckSplitRelabels = 0;
        uint32_t neckPieces = 0;         ///< Released parts of at least the piece size: a Detach each.
        uint32_t neckCrumbs = 0;         ///< Released parts under it: in the ClearMask.
        uint32_t neckVoxels = 0;         ///< Voxels released (pieces and crumbs).
        uint32_t neckOversize = 0;       ///< Overloaded parts over kMaxIslandVoxels: they stay.
        uint32_t neckDeferredParts = 0;  ///< Overloaded parts over kNeckMaxReleasedVoxels / kNeckMaxReleasedParts: a later cast.
        uint32_t neckSolidChunks = 0;    ///< Exterior floods ended as ground on a chunk solid through.
        uint32_t neckSkipped = 0;        ///< Network over kNeckMaxNetworkNodes / kNeckMaxArcPairs: nothing judged.
        uint32_t neckExteriorBudget = 0; ///< Exterior floods turned into anchors by kNeckExteriorVisitBudget.
        uint32_t neckFlowBudget = 0;     ///< kNeckFlowWorkBudget spent: nothing released.
        uint32_t neckUnloaded = 0;       ///< Server: an exterior flood met a chunk that is not loaded (an error).
        uint32_t neckBeyondWindow = 0;   ///< Offline: the check met a chunk outside the window: support for now (logged).
        uint32_t neckLoading = 0;        ///< Offline: the check met a chunk of the window still loading: support for now.
        /// Offline: the check took an unloaded chunk as support (one of the two above), so a late
        /// search was queued that runs again once it has loaded (Simulation::DeferredIslandSearches);
        /// the islands of this search were emitted.
        bool neckDeferred = false;
        /// The released parts: load and cut (MU = kg / vs^3), faces, voxels, in emission order.
        std::vector<voxel::NeckCut> neckCuts;
    };

    /**
     * @brief A replicated object a blast changed or made (SimulationConfig::replication; networking
     * stage 6), for the server to send: an ObjectModel for a damaged one that survives, an
     * EntitySpawn for a piece that became a server body. Despawns are not listed: every destruction
     * reaches the server through its `on_destroy<scene::NetId>` observer.
     */
    struct ObjectEvent {
        enum class Kind : uint8_t {
            Spawn,     ///< a new server body: `model`, `pose`, the velocities (after the push), `motion`
            Model,     ///< the survivor of Scene::Damage: `model`, `offset` (old model voxels), `motion`
        };
        Kind kind = Kind::Spawn;
        uint32_t netId = 0;
        scene::Entity entity = scene::kNoEntity;
        std::shared_ptr<voxel::VoxelModel> model;
        math::Transform pose;
        glm::vec3 linearVelocity{0.0f};
        glm::vec3 angularVelocity{0.0f};
        physics::MotionType motion = physics::MotionType::Dynamic;
        glm::ivec3 offset{0};
    };

    /// @brief A segment a mechanism took: what the server sends as MechanismMove ([mech-1]).
    struct MechanismChange {
        scene::Entity entity = scene::kNoEntity;
        scene::MechanismSegment segment;
    };

    /// @brief A hold or a release: what the server sends as EntityHold / EntityRelease ([mech-1]).
    struct HoldChange {
        scene::Entity entity = scene::kNoEntity;
        bool hold = false;           ///< true: EntityHold; false: EntityRelease
        uint8_t holder = 0;
        uint16_t relYaw = 0;
        uint32_t tick = 0;
        uint8_t reason = 0;          ///< sim::HoldEnd (a release)
        glm::vec3 position{0.0f};    ///< the release pose (shape origin) and velocity (quantised)
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f};
        friend bool operator==(const HoldChange&, const HoldChange&) = default;
    };

    /**
     * @brief Everything sim::Blast did: the replicated operations in their order plus what the
     * presentation (PresentBlast) needs.
     */
    struct BlastResult {
        /// ops[0] is the CarveSphere (none with BlastOptions::noCarve; the Ablate MapShape when
        /// `absorbed`); one Detach (netId 0) per terrain island of at least
        /// scene::MinPieceVoxels(world voxel size) follows: first the islands inside the island
        /// box in the order ExtractFloatingIslands found them, then the face components the flood
        /// proved, in candidate order (in-box count ascending, ties by label), then the parts the
        /// load check released (shred.md Part 4b), in piece order; last, if there were smaller
        /// ones ("crumbs", from all three), one ClearMask over the box of all of them.
        /// Applying the `op`s in this order to a world that was equal before gives the same
        /// voxels (ApplyOp).
        std::vector<AppliedOp> ops;
        /// The island box (blast radius + 24 voxels) was loaded and searched and the face flood
        /// completed. false: a chunk of the island box was not loaded (nothing emitted), or offline
        /// the flood met a chunk of the flood box that is not loaded (`search.floodDeferred`: the
        /// in-box islands were emitted, the face pieces were not). Either way the search is
        /// remembered and repeated by RetryIslandSearches. A server window always has both boxes
        /// loaded; an offline window may not, near its edge or while it streams.
        bool islandsSearched = false;
        /// Made by RetryIslandSearches: the Detach / ClearMask operations of an earlier blast whose
        /// search had to wait, preceded by the replayed CarveSphere when the blast's carve had
        /// skipped chunks that have loaded since (`carveReplayed`; a result may hold only that, while
        /// the search still waits). PresentBlast shows no explosion and no flash for it.
        bool deferred = false;
        /// Deferred only: ops[0] is the blast's carve applied again, to `carveReplayChunks` only
        /// (the chunks it had skipped that have loaded since; World::CarveSphereQInChunk). Such an
        /// op is not a self-contained replay of WorldOp::Carve: it is offline only, never sent.
        bool carveReplayed = false;
        std::vector<glm::ivec3> carveReplayChunks;
        /// This blast's search was deferred into a full list and replaced an older deferred search
        /// (the one farthest from the window's centre): that one is lost (logged).
        uint32_t deferredDropped = 0;
        /// RetryIslandSearches only: a deferred search given up because its flood box left the
        /// window (logged). Such a result has no operations.
        uint32_t deferredGivenUp = 0;
        IslandSearchStats search;
        /// SimulationConfig::checkIslandSearch only: the counters of the other search (the voxel
        /// search when the graph ran), which ran on the same voxels and applied nothing.
        IslandSearchStats reference;
        BlastTimes times;
        uint32_t crumbs = 0;           ///< Islands under scene::MinPieceVoxels(world voxel size), all in the one ClearMask.
        uint32_t objectsHit = 0;       ///< Objects that lost voxels (Scene::Damage removed > 0).
        uint32_t objectVoxels = 0;     ///< Voxels removed from objects.
        /// Object pieces below SceneConfig::replicatedPieceVoxels (none while it is 0), their
        /// velocity already including the blast's push. Local rubble for whoever draws. With
        /// replication, also the object pieces over a piece budget (TakePieceSlot).
        std::vector<scene::DamageFragment> fragments;
        /// Replication only: object changes in the order they happened (ObjectEvent).
        std::vector<ObjectEvent> objectEvents;
        /// Replication only: NetIds of Detach operations whose server body could not be made (out of
        /// bodies, above the ceiling): the server despawns them at once.
        std::vector<uint32_t> lostNetIds;
        /// Segments the blast's mechanism damage made (Freed / Broken), in the order they happened ([mech-1]).
        std::vector<MechanismChange> mechanismChanges;
        std::vector<HoldChange> holdChanges;   ///< releases (Damaged) the blast made, before its object events
        glm::dvec3 center{0.0};        ///< The blast as applied (after quantisation), world metres.
        float radius = 0.0f;           ///< Metres, after quantisation.
        /// BlastAt only: the hit was fresh conjured matter (AbsorbsBlast). ops[0] is the Ablate
        /// MapShape and no carve follows; the island search, the objects and the push ran.
        bool absorbed = false;

        /// @brief Union of the skipped / unsynced chunks of all operations.
        voxel::EditReport Report() const;
        /// @brief Detach operations: islands of at least scene::MinPieceVoxels(world voxel size) (crumbs not counted).
        uint32_t IslandCount() const;
        /// @brief Voxels the ClearMask removed (0 without crumbs).
        uint32_t CrumbVoxels() const;
    };

    /// Islands up to this many voxels are detached (larger floating parts stay in place, counted).
    inline constexpr uint32_t kMaxIslandVoxels = 60000;
    /// The island search box is the blast's bounding box grown by this many voxels per side.
    inline constexpr int32_t kIslandSearchMargin = 24;
    /// Work budget of one blast's face flood, in voxel visits (docs/design/shred.md, R5).
    inline constexpr uint32_t kFloodVisitBudget = 4 * kMaxIslandVoxels;
    /// Work budget of one blast's graph search, in node visits (docs/design/shred.md stage 4, R5).
    /// A flood gives up at kMaxIslandVoxels voxels: 118 nodes of solid cells, about 940 of
    /// one-voxel walls; the budget is spent only by dozens of large separate pieces.
    inline constexpr uint32_t kGraphNodeVisitBudget = 32768;
    /// Budgets of one blast's load check (docs/design/shred.md Part 4b, R5): network nodes and arc
    /// pairs (above either nothing is judged), exterior flood node visits (then every exterior
    /// flood is an anchor) and preflow arc scans (spent: nothing released). All counts.
    inline constexpr uint32_t kNeckMaxNetworkNodes = 8192;
    inline constexpr uint32_t kNeckMaxArcPairs = 65535;
    inline constexpr uint32_t kNeckExteriorVisitBudget = 16384;
    /// Lowered from 262 144 with the session's cast work budget (networking stage 3; shred.md open
    /// question 7): 262 144 arc scans took 5-6 ms of preflow alone, every measured cast needs at
    /// most 13 000. Spent means "released by a later cast" (R7).
    inline constexpr uint32_t kNeckFlowWorkBudget = 65536;
    /// What one check may release (R5, R7): parts in piece order while both fit; the rest stays,
    /// still overloaded, for a later cast (counted). One piece of the largest island size always fits.
    inline constexpr uint32_t kNeckMaxReleasedVoxels = kMaxIslandVoxels;
    inline constexpr uint32_t kNeckMaxReleasedParts = 64;
    /// Island searches waiting at once (Simulation::DeferredIslandSearches). When the list is full,
    /// a new one replaces the one whose box is farthest from the window's centre (ties: the oldest);
    /// the new search itself is always kept (it is the blast the player just made).
    inline constexpr size_t kMaxDeferredIslandSearches = 16;
    /// A session server's window is its play bounds (whole chunks) plus this many chunks on all six
    /// sides, so the flood box of every cast centred inside the play bounds is loaded
    /// (docs/design/networking.md, "Server world without a camera").
    inline constexpr int32_t kServerWindowMarginChunks = 3;
    static_assert(kServerWindowMarginChunks * voxel::kChunkSize >= voxel::kFloodHalfExtent);
    /// The largest cast radius a session server runs (1/16 voxel): its island box (radius / 16 +
    /// kIslandSearchMargin voxels per side) and its carve then stay inside the margin for any centre
    /// in the play bounds. 72 voxels = 9 m at 0.125 m.
    inline constexpr uint16_t kMaxCastRadiusQ =
        static_cast<uint16_t>((kServerWindowMarginChunks * voxel::kChunkSize - kIslandSearchMargin) * voxel::kQuantaPerVoxel);
    static_assert(kMaxCastRadiusQ / voxel::kQuantaPerVoxel + kIslandSearchMargin <= kServerWindowMarginChunks * voxel::kChunkSize);

    // --- the cast work budget (docs/design/networking.md "Time and ticks", shred.md R5) ----------
    //  A session server runs queued casts while the tick has work left. Work is counted, never
    //  timed: only counters that follow from the voxels of the flood box and from the operations'
    //  integers are charged, so a replayed server puts every cast on the same tick. Never charged:
    //  `graphsBuilt` and `chunksDecoded` (they depend on the graph cache and on streaming) and the
    //  object half of a blast (it depends on Jolt's float poses). A work unit is about 0.1 us on the
    //  reference machine (the UHD 620 laptop, -O2; calibrated once, networking.md "Stage 3 as built").

    /// Work a session server may spend on casts per tick (about 6 ms of the 16.7 ms tick).
    inline constexpr uint64_t kCastWorkPerTick = 60000;
    /// One chunk graph used by a search, at the price of building it cold (`IslandSearchStats::
    /// chunkGraphs` whatever the cache held). Calibrated: 20 us per graph on a quiet CPU (in a
    /// game that also streams and renders, 70-120 us: shred.md "Stage 4 as built").
    inline constexpr uint64_t kColdChunkGraphWork = 200;
    /// One node visit of the graph floods (est.: too few visits in the calibration to fit).
    inline constexpr uint64_t kNodeVisitWork = 2;
    /// The load check's counted work (shred.md "Work bound per cast": exterior visits + arc pairs +
    /// flow work / 8 + 64 x split relabels + released voxels / 64) is multiplied by this
    /// (calibrated: 0.18 us per unit of that formula).
    inline constexpr uint64_t kNeckWorkScale = 2;
    /// Every operation, plus its box: carve / fill / swap / box voxels / 16, mask voxels / 128.
    inline constexpr uint64_t kOpWork = 100;

    /// @brief The work of one cast, by part (all integers from counters and operations).
    struct CastWork {
        uint64_t search = 0;     ///< Graph node visits, chunk graphs at the cold price; voxel search: box voxels / 8 + flood visits.
        uint64_t neck = 0;       ///< The load check.
        uint64_t ops = 0;        ///< The operations the cast applied.
        uint64_t Total() const { return search + neck + ops; }
    };
    /// @brief The work of one operation (kOpWork plus its box, from its integers only).
    uint64_t OpWork(const WorldOp& op);
    /// @brief The work of a blast as `sim::Blast` returned it (the object half is not counted).
    CastWork CastWorkOf(const BlastResult& blast);

    /// @brief The flood box of a blast: floor(centre / 16) +- voxel::kFloodHalfExtent voxels, [min, max).
    void FloodBox(const voxel::SphereQ& sphere, glm::ivec3& min, glm::ivec3& max);
    /// @brief The island box of a blast: floor(centre / 16) +- (radius / 16 + kIslandSearchMargin), [min, max).
    void IslandBox(const voxel::SphereQ& sphere, glm::ivec3& min, glm::ivec3& max);

    /**
     * @brief The push of a blast at `position`, as a velocity change (m/s): away from the centre,
     * 6 m/s x a linear falloff that reaches 0 at three radii; straight up at the centre. Bodies
     * get it as an impulse (x mass), detached pieces and fragments as their initial velocity.
     */
    glm::vec3 BlastPush(const glm::vec3& center, float radius, const glm::vec3& position);

    /**
     * @brief The simulation half of a blast (server, or an offline game): carve, detach terrain
     * islands, damage objects, push bodies. Spawns no particles and no light: that is
     * presentation (PresentBlast), made from the returned result.
     * @details In order:
     *  1. `WorldOp::Carve(sphere, power)` is applied (voxel::World::CarveSphereQ).
     *  2. The island search over the island box `floor(centre) +- (radius / 16 + 24)` voxels and
     *     the flood box (`floor(centre) +- 96` voxels), without removing anything: by default on
     *     the cell graph cached per terrain chunk (voxel::SearchIslandsOnGraph, budget
     *     kGraphNodeVisitBudget node visits; docs/design/shred.md stage 4), or with
     *     SimulationConfig::islandSearch Voxel the stage-1 search it reproduces: ExtractFloatingIslands
     *     over the island box, then components touching its face with at most kMaxIslandVoxels
     *     voxels in the box proven by voxel::ProveFaceComponents inside the flood box (budget
     *     kFloodVisitBudget visits). Both give the same islands in the same order unless a budget
     *     runs out or (a chunk of the flood box not loaded) one defers or attaches where the other
     *     closes first. Each island of at least
     *     scene::MinPieceVoxels(world voxel size) becomes a `WorldOp::Detach` (mask of its voxels,
     *     netId 0, velocity = BlastPush at its minimum corner) that is applied at once with
     *     ApplyOp - the same call a client makes: in-box islands first, then proven ones, then
     *     (SimulationConfig::neckRule, graph search) the parts the load check releases: the
     *     largest set of judged nodes whose weight no distribution fits the strength of what
     *     holds it (docs/design/shred.md Part 4b). The
     *     smaller islands (crumbs) become one `WorldOp::Clear` whose mask is the union of their
     *     voxels over the smallest box holding them all, applied last. No entity is created for
     *     an island: rubble and particles are local. Offline, a search that meets a chunk that
     *     is not loaded (island box, or a flood in the flood box) asks the world to load it and
     *     is repeated by RetryIslandSearches; on a session server (SimulationConfig::sessionServer)
     *     a flood that meets one stays attached and is counted as an error. The load check takes
     *     such a chunk as support (it can only release less); offline the islands still go out
     *     and the search is repeated once that chunk has loaded (IslandSearchStats::neckDeferred).
     *  3. Scene::Damage for every body whose bounds overlap the blast's box (a snapshot taken
     *     first, so pieces made here are not hit twice); pieces below the replicated threshold
     *     come back as fragments.
     *  4. Every rigid body within three radii gets mass x BlastPush; bodies within two radii are
     *     woken (PhysicsWorld::ActivateInBox), so what rested on removed ground falls.
     *  With SimulationConfig::replication (a session server, networking stage 6): a Detach of at
     *  least SceneConfig::replicatedPieceVoxels that holds no conjured id (voxel::MaterialFlag_Conjured:
     *  such a piece never asks for a slot) and whose TakePieceSlot says so carries a new NetId, and
     *  after step 4 its server body is made from the voxels it removed (velocity = its push, as a
     *  client's rubble); an object piece of step 3 gets a NetId (an ObjectEvent::Spawn) or, over a
     *  budget, becomes a fragment; a damaged replicated object that survives is an
     *  ObjectEvent::Model, one that breaks apart is despawned Broken.
     * Steps 1-2 are integer-exact (equal worlds give equal operations); steps 3-4 are server
     * results (float poses, Jolt) and are never hashed.
     * Call it between the streaming and the physics phase of a tick (Application::OnFixedUpdate)
     * or from Application::OnUpdate; never inside a registry view.
     */
    BlastResult Blast(Simulation& simulation, const voxel::SphereQ& sphere, uint16_t power);

    /// @brief How a site-based blast runs.
    struct BlastOptions {
        /// Skip step 1 (the site's carve): no carve in `ops`, nothing to replay. The island search
        /// and the load check still run, and the objects and the push when the site's radius > 0.
        bool noCarve = false;
    };
    /// @brief The site of `Blast(simulation, sphere, power)`: its carve, IslandBox, FloodBox and the
    /// sphere's centre and radius in metres.
    BlastSite SphereSite(const voxel::World& world, const voxel::SphereQ& sphere, uint16_t power);
    /// @brief A site check over one chunk (docs/design/magic.md "Expiry and structure"): no carve,
    /// island box [32 c - 24, 32 c + 56), flood box 32 c + 16 +- kFloodHalfExtent, radius 0.
    BlastSite ChunkSite(const voxel::World& world, const glm::ivec3& chunk);
    /**
     * @brief Blast steps 1-5 on a site. `Blast(simulation, sphere, power)` is
     * `Blast(simulation, SphereSite(world, sphere, power))`: the same operations bit for bit.
     * Steps 3 and 4 (objects and push) reach only as far as `site.radius`.
     */
    BlastResult Blast(Simulation& simulation, const BlastSite& site, const BlastOptions& options = {});

    /// @brief The absorb rule (magic.md "The absorb rule, ablative"): the hit voxel `hitMaterial`
    /// is conjured and harder than the blast (`hardness * 16 > power`, the compare of CarveSphereQ).
    bool AbsorbsBlast(const voxel::MaterialLibrary& materials, uint8_t hitMaterial, uint16_t power);
    /**
     * @brief A blast that obeys the absorb rule. `hitMaterial` is the world id the cast ray hit (0:
     * a body or unknown). Not absorbed: `Blast(simulation, sphere, power)`. Absorbed: ops[0] is
     * `MapShape(Map_Ablate, sphere at the centre, r 0.25 m)` (knit and slag become air), then the
     * sphere's site runs with `noCarve`: its islands, objects and push; `absorbed` is set.
     */
    BlastResult BlastAt(Simulation& simulation, const voxel::SphereQ& sphere, uint16_t power, uint8_t hitMaterial);

    /**
     * @brief Runs the island search (step 2 of Blast) of earlier blasts that had to wait
     * (`islandsSearched` false: an offline window streaming its cave rows, or a blast near the
     * window's edge).
     * @details Blast asked the world to load what was missing (voxel::World::RequestLoad) and
     * remembered the search (Simulation::DeferredIslandSearches). A search runs again once its
     * island box is loaded and, if a flood was waiting, the chunk it met. When the blast's own
     * carve had skipped chunks that were not loaded, the carve is applied to each of them, and only
     * to them, in the first call after it has loaded (before the search, which waits for them);
     * it is ops[0] of that call's result (`carveReplayed`, `carveReplayChunks`). The
     * operations are applied and returned as one BlastResult with `deferred` set (show it with
     * PresentBlast). A search is given up only once its flood box has left the window (a result
     * with `deferredGivenUp`, logged). Never needed on a server (its window always has the boxes
     * loaded); the operations are later operations of their own, not part of the blast's.
     * Call once per frame between the streaming and the physics phase (Engine::Run does).
     */
    std::vector<BlastResult> RetryIslandSearches(Simulation& simulation);
    /// @brief Quantises (voxel::QuantizeSphere with the world's voxel size, voxel::QuantizePower)
    /// and runs the blast above.
    BlastResult Blast(Simulation& simulation, const BlastDesc& desc);

    /**
     * @brief The presentation half of a blast, for whoever draws (Engine::PresentBlast offline;
     * a client from stage 3, given operations it applied itself). Never on a server.
     * @details Into the simulation's particle pool and scene: per CarveSphere debris particles
     * (a sample, at most 799 bursts), per Detach with netId 0 local rubble (`VoxelObjectDesc::debris`, made
     * from the voxels it removed), per ClearMask particles only - every
     * `SceneConfig::debrisParticleStride`-th crumb voxel, at most 400 bursts, pushed with
     * BlastPush at the voxel - never an entity or a body; rubble for the object fragments, the
     * explosion burst and a fading flash. Changes no voxel and nothing that is hashed.
     */
    void PresentBlast(Simulation& simulation, const BlastResult& blast);
}
