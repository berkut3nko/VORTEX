module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.scene;

namespace vortex::sim {

    voxel::EditReport BlastResult::Report() const {
        voxel::EditReport out;
        for (const AppliedOp& applied : ops) {
            out.skipped.insert(out.skipped.end(), applied.result.report.skipped.begin(), applied.result.report.skipped.end());
            out.unsynced.insert(out.unsynced.end(), applied.result.report.unsynced.begin(), applied.result.report.unsynced.end());
        }
        return out;
    }

    uint32_t BlastResult::IslandCount() const {
        uint32_t n = 0;
        for (const AppliedOp& applied : ops) n += applied.op.kind == WorldOpKind::Detach ? 1u : 0u;
        return n;
    }

    uint32_t BlastResult::CrumbVoxels() const {
        uint32_t n = 0;
        for (const AppliedOp& applied : ops) n += applied.op.kind == WorldOpKind::ClearMask ? applied.result.changed : 0u;
        return n;
    }

    glm::vec3 BlastPush(const glm::vec3& center, float radius, const glm::vec3& position) {
        const glm::vec3 d = position - center;
        const float dist = glm::length(d);
        const float reach = radius * 3.0f;
        if (!(dist <= reach) || !(reach > 0.0f)) return glm::vec3(0.0f);
        const float falloff = 1.0f - dist / reach;
        return (dist > 1e-3f ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f)) * (6.0f * falloff);
    }

    uint64_t OpWork(const WorldOp& op) {
        glm::ivec3 mn, mx;
        op.Bounds(mn, mx);
        const glm::ivec3 size = glm::max(mx - mn, glm::ivec3(0));
        const uint64_t voxels = static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.y) * static_cast<uint64_t>(size.z);
        const bool mask = op.kind == WorldOpKind::Detach || op.kind == WorldOpKind::ClearMask;
        return kOpWork + (mask ? voxels / 128 : voxels / 16);
    }

    CastWork CastWorkOf(const BlastResult& blast) {
        const IslandSearchStats& s = blast.search;
        CastWork w;
        w.search = static_cast<uint64_t>(s.nodeVisits) * kNodeVisitWork + static_cast<uint64_t>(s.chunkGraphs) * kColdChunkGraphWork +
                   s.boxVoxels / 8 + s.floodVisits;
        w.neck = kNeckWorkScale * (static_cast<uint64_t>(s.neckExteriorVisits) + s.neckArcs + s.neckFlowWork / 8 +
                                   64ull * s.neckSplitRelabels + s.neckVoxels / 64);
        for (const AppliedOp& applied : blast.ops) w.ops += OpWork(applied.op);
        return w;
    }

    void IslandBox(const voxel::SphereQ& sphere, glm::ivec3& min, glm::ivec3& max) {
        const glm::ivec3 c = math::FloorDiv(sphere.center, voxel::kQuantaPerVoxel);
        const glm::ivec3 half(static_cast<int32_t>(sphere.radius / voxel::kQuantaPerVoxel) + kIslandSearchMargin);
        min = c - half;
        max = c + half;
    }

    void FloodBox(const voxel::SphereQ& sphere, glm::ivec3& min, glm::ivec3& max) {
        const glm::ivec3 c = math::FloorDiv(sphere.center, voxel::kQuantaPerVoxel);
        min = c - glm::ivec3(voxel::kFloodHalfExtent);
        max = c + glm::ivec3(voxel::kFloodHalfExtent);
    }

    namespace {
        using Clock = std::chrono::steady_clock;
        double MsSince(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

        /// Refused: a session server's island box was not loaded (an error, nothing waits).
        /// NeckWaits (offline only): everything was emitted, but the load check took a chunk that
        /// is not loaded as support; the search runs again once it has loaded.
        enum class SearchOutcome { Done, BoxNotLoaded, FloodWaits, Refused, NeckWaits };

        /// Every chunk of the voxel box [min, max) is loaded, or lies above the build ceiling or
        /// below the bottom row of a fixed-height window (nothing exists above, the world's ground
        /// is below; neither is ever loaded): ExtractFloatingIslands' rule.
        bool BoxLoaded(const voxel::World& world, const glm::ivec3& min, const glm::ivec3& max) {
            const glm::ivec3 lo = voxel::VoxelToChunk(min), hi = voxel::VoxelToChunk(max - glm::ivec3(1));
            const int bottomRow = world.WindowMinChunk().y;
            const int ceilingRow = bottomRow + world.WindowSize().y;
            for (int cz = lo.z; cz <= hi.z; ++cz)
                for (int cy = lo.y; cy <= hi.y; ++cy) {
                    if (world.FullHeight() && (cy >= ceilingRow || cy < bottomRow)) continue;
                    for (int cx = lo.x; cx <= hi.x; ++cx) {
                        if (!world.IsChunkLoaded(glm::ivec3(cx, cy, cz))) return false;
                    }
                }
            return true;
        }

        /// The voxel box [min, max) shares at least one chunk column with the world window.
        bool BoxTouchesWindow(const voxel::World& world, const glm::ivec3& min, const glm::ivec3& max) {
            const glm::ivec3 lo = voxel::VoxelToChunk(min), hi = voxel::VoxelToChunk(max - glm::ivec3(1));
            const glm::ivec3 wlo = world.WindowMinChunk(), whi = wlo + world.WindowSize() - glm::ivec3(1);
            return glm::all(glm::lessThanEqual(lo, whi)) && glm::all(glm::lessThanEqual(wlo, hi));
        }

        /// What one island search found, before anything is applied.
        struct Found {
            bool searched = false;            ///< false: a chunk of the island box is not loaded
            bool complete = true;             ///< false (offline): a flood met `waitChunk`, not loaded
            glm::ivec3 waitChunk{0};
            std::vector<voxel::FloatingIsland> boxIslands;     ///< in-box islands, scan order
            std::vector<voxel::FloatingIsland> faceIslands;    ///< proven ones, candidate order
            std::vector<voxel::FloatingIsland> neckPieces;     ///< released by the load check, piece order
            std::vector<voxel::NeckCut> neckCuts;
            bool neckWaits = false;                            ///< offline: the check took an unloaded chunk as support
            glm::ivec3 neckWaitChunk{0};
            IslandSearchStats stats;
            double searchMs = 0.0, floodMs = 0.0, graphBuildMs = 0.0, graphFloodMs = 0.0, relabelMs = 0.0, neckMs = 0.0, neckBuildMs = 0.0;
        };

        /// The stage-1 search: ExtractFloatingIslands over the island box, then the face flood.
        Found VoxelSearch(Simulation& simulation, const voxel::SphereQ& sphere) {
            voxel::World& world = simulation.World();
            Found out;
            glm::ivec3 boxMin, boxMax;
            IslandBox(sphere, boxMin, boxMax);
            Clock::time_point start = Clock::now();
            voxel::IslandBoxScan scan;
            out.boxIslands = voxel::ExtractFloatingIslands(world, &simulation.Materials(), boxMin, boxMax, kMaxIslandVoxels, false,
                                                           voxel::MaterialFlag_Liquid, &out.searched, &scan);
            out.searchMs = MsSince(start);
            if (!out.searched) return out;
            IslandSearchStats& stats = out.stats;
            stats.boxVoxels = scan.boxVoxels;
            stats.faceOversize = scan.faceOversize;
            stats.oversizeIslands = scan.oversizeIslands;
            stats.faceCandidates = static_cast<uint32_t>(scan.faceComponents.size());
            // Face components: attached inside the box, maybe floating outside it.
            start = Clock::now();
            if (!scan.faceComponents.empty()) {
                voxel::FloodLimits limits;
                FloodBox(sphere, limits.floodMin, limits.floodMax);
                limits.maxVoxels = kMaxIslandVoxels;
                limits.maxVisitsPerSearch = kFloodVisitBudget;
                limits.deferOnUnloaded = !simulation.Config().sessionServer;
                // The labelled box: the island box without rows below a full-height window.
                voxel::FloodResult flood = voxel::ProveFaceComponents(world, &simulation.Materials(), scan.faceComponents, scan.boxMin,
                                                                      scan.boxMax, scan.labels, voxel::MaterialFlag_Liquid, limits);
                stats.merged = flood.merged;
                stats.attachedByLimit = flood.attachedByLimit;
                stats.attachedByKnown = flood.attachedByKnown;
                stats.attachedByBudget = flood.attachedByBudget;
                stats.attachedByUnloaded = flood.attachedByUnloaded;
                stats.chunksDecoded = flood.chunksDecoded;
                stats.floodVisits = flood.visits;
                out.complete = flood.complete;
                out.waitChunk = flood.unloadedChunk;
                if (flood.complete) out.faceIslands = std::move(flood.islands);
            }
            out.floodMs = MsSince(start);
            return out;
        }

        /// Stage 4: the same islands from the cell graph cached per terrain chunk.
        Found GraphSearch(Simulation& simulation, const voxel::SphereQ& sphere) {
            Found out;
            voxel::GraphSearchLimits limits;
            IslandBox(sphere, limits.boxMin, limits.boxMax);
            FloodBox(sphere, limits.floodMin, limits.floodMax);
            limits.maxIslandVoxels = kMaxIslandVoxels;
            limits.maxNodeVisits = kGraphNodeVisitBudget;
            limits.deferOnUnloaded = !simulation.Config().sessionServer;
            voxel::TerrainGraphCache& cache = simulation.TerrainGraphs();
            const voxel::GraphMaterials materials = voxel::MakeGraphMaterials(&simulation.Materials(), voxel::MaterialFlag_Liquid);
            // The load check (shred.md Part 4b): capacities for the session's voxel size (the
            // float both sides hold bit for bit), the four work budgets.
            voxel::NeckLimits neck;
            if (simulation.Config().neckRule && simulation.Config().islandSearch == IslandSearchKind::Graph) {
                neck = voxel::MakeNeckLimits(materials, simulation.World().VoxelSize());
                neck.maxNetworkNodes = kNeckMaxNetworkNodes;
                neck.maxArcPairs = kNeckMaxArcPairs;
                neck.maxExteriorVisits = kNeckExteriorVisitBudget;
                neck.maxFlowWork = kNeckFlowWorkBudget;
                neck.maxReleasedVoxels = kNeckMaxReleasedVoxels;
                neck.maxReleasedParts = kNeckMaxReleasedParts;
                limits.neck = &neck;
            }
            const double buildBefore = cache.Stats().buildMs;
            const Clock::time_point start = Clock::now();
            voxel::GraphSearchResult r = voxel::SearchIslandsOnGraph(simulation.World(), cache, materials, limits);
            out.searchMs = MsSince(start);
            out.graphBuildMs = cache.Stats().buildMs - buildBefore;
            out.graphFloodMs = r.floodMs;
            out.relabelMs = r.relabelMs;
            out.neckMs = r.neckMs;
            out.neckBuildMs = r.neckBuildMs;
            out.neckWaits = r.neckWaits;
            out.neckWaitChunk = r.neckWaitChunk;
            out.searched = r.searched;
            out.complete = r.complete;
            out.waitChunk = r.unloadedChunk;
            out.boxIslands = std::move(r.boxIslands);
            out.faceIslands = std::move(r.faceIslands);
            out.neckPieces = std::move(r.neckPieces);
            out.neckCuts = std::move(r.neckCuts);
            IslandSearchStats& stats = out.stats;
            stats.graph = true;
            stats.faceOversize = r.faceOversize;
            stats.oversizeIslands = r.oversizeIslands;
            stats.merged = r.merged;
            stats.attachedByLimit = r.attachedByLimit;
            stats.attachedByKnown = r.attachedByKnown;
            stats.attachedByBudget = r.attachedByBudget;
            stats.attachedByUnloaded = r.attachedByUnloaded;
            stats.chunksDecoded = r.chunksDecoded;
            stats.seedNodes = r.seedNodes;
            stats.boxComponents = r.boxComponents;
            stats.graphFloods = r.floods;
            stats.attachedByFace = r.attachedByFace;
            stats.nodeVisits = r.nodeVisits;
            stats.chunkGraphs = r.chunkGraphs;
            stats.graphsBuilt = static_cast<uint32_t>(r.graphsBuilt);
            stats.overfullCells = r.overfullCells;
            stats.relabelledNodes = r.relabelledNodes;
            stats.outsideBox = r.outsideBox;
            stats.neckRan = r.neckRan;
            stats.neckNodes = r.neckNodes;
            stats.neckSplit = r.neckSplit;
            stats.neckLoads = r.neckLoads;
            stats.neckAnchors = r.neckAnchors;
            stats.neckArcs = r.neckArcs;
            stats.neckExteriorVisits = r.neckExteriorVisits;
            stats.neckFlowWork = r.neckFlowWork;
            stats.neckSplitRelabels = r.neckSplitRelabels;
            stats.neckVoxels = r.neckVoxels;
            stats.neckOversize = r.neckOversize;
            stats.neckSkipped = r.neckSkipped;
            stats.neckExteriorBudget = r.neckExteriorBudget;
            stats.neckFlowBudget = r.neckFlowBudget;
            stats.neckUnloaded = r.neckUnloaded;
            stats.neckBeyondWindow = r.neckBeyondWindow;
            stats.neckLoading = r.neckLoading;
            stats.neckDeferredParts = r.neckDeferredParts;
            stats.neckSolidChunks = r.neckSolidChunks;
            return out;
        }

        bool SameIslands(const std::vector<voxel::FloatingIsland>& a, const std::vector<voxel::FloatingIsland>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].origin != b[i].origin || a[i].voxelCount != b[i].voxelCount || a[i].volume.Size() != b[i].volume.Size() ||
                    a[i].volume.Data() != b[i].volume.Data())
                    return false;
            }
            return true;
        }

        /// SimulationConfig::checkIslandSearch: the two searches on the same voxels must find the
        /// same islands in the same order. Face islands are not compared where a work budget ran
        /// out on either side, nor where either met a chunk of the flood box that is not loaded
        /// (they flood in another order: one may defer while the other closes first).
        bool SameResult(const Found& a, const Found& b) {
            if (a.searched != b.searched) return false;
            if (!a.searched) return true;
            if (!SameIslands(a.boxIslands, b.boxIslands)) return false;
            if (a.stats.attachedByBudget != 0 || b.stats.attachedByBudget != 0) return true;
            if (!a.complete || !b.complete || a.stats.attachedByUnloaded != 0 || b.stats.attachedByUnloaded != 0) return true;
            return SameIslands(a.faceIslands, b.faceIslands);
        }

        /// Step 2 of Blast: islands in the island box of `sphere`, and face components the flood
        /// proves, become Detach operations, crumbs one ClearMask, each applied at once and
        /// appended to `result.ops`. BoxNotLoaded: a chunk of the island box is not loaded,
        /// nothing was done. FloodWaits (offline only): a flood met `waitChunk`, which is not
        /// loaded; the in-box islands were emitted, the face pieces were not. Both boxes are
        /// derived from the integer sphere only.
        SearchOutcome SearchIslands(Simulation& simulation, const voxel::SphereQ& sphere, BlastResult& result, glm::ivec3& waitChunk) {
            voxel::World& world = simulation.World();
            const glm::vec3 center(result.center);
            const float radius = result.radius;
            const bool graph = simulation.Config().islandSearch == IslandSearchKind::Graph;
            Found found = graph ? GraphSearch(simulation, sphere) : VoxelSearch(simulation, sphere);
            result.times.searchMs += found.searchMs;
            result.times.floodMs += found.floodMs;
            result.times.graphBuildMs += found.graphBuildMs;
            result.times.graphFloodMs += found.graphFloodMs;
            result.times.relabelMs += found.relabelMs;
            result.times.neckMs += found.neckMs;
            result.times.neckBuildMs += found.neckBuildMs;
            ++simulation.IslandSearchCheck().run;
            if (simulation.Config().checkIslandSearch) {
                // Nothing has been applied yet: the other search reads the same voxels.
                const Found other = graph ? VoxelSearch(simulation, sphere) : GraphSearch(simulation, sphere);
                result.reference = other.stats;
                result.times.referenceMs += other.searchMs + other.floodMs;
                Simulation::IslandSearchChecks& checks = simulation.IslandSearchCheck();
                ++checks.searches;
                if (!SameResult(found, other)) {
                    ++checks.mismatches;
                    ++found.stats.referenceMismatch;
                    Log::Error("Blast: the graph and the voxel island search differ at ({:.2f}, {:.2f}, {:.2f}) m: searched {} / {}, "
                               "complete {} / {}, {} / {} in-box island(s), {} / {} face island(s)",
                               result.center.x, result.center.y, result.center.z, found.searched, other.searched, found.complete,
                               other.complete, found.boxIslands.size(), other.boxIslands.size(), found.faceIslands.size(),
                               other.faceIslands.size());
                }
            }
            if (!found.searched) {
                if (!simulation.Config().sessionServer) return SearchOutcome::BoxNotLoaded;
                // A session window holds every island box by construction: never wait, say so.
                ++result.search.islandBoxUnloaded;
                Log::Error("Blast: a chunk of the island box of the blast at ({:.2f}, {:.2f}, {:.2f}) m is not loaded; nothing was "
                           "searched (a session window must hold every flood box)",
                           result.center.x, result.center.y, result.center.z);
                return SearchOutcome::Refused;
            }
            IslandSearchStats& stats = result.search;
            {
                const IslandSearchStats& f = found.stats;
                // Sums over the searches of one result (a late result has one).
                stats.graph = f.graph;
                stats.boxVoxels += f.boxVoxels;
                stats.faceCandidates += f.faceCandidates;
                stats.faceOversize += f.faceOversize;
                stats.oversizeIslands += f.oversizeIslands;
                stats.merged += f.merged;
                stats.attachedByLimit += f.attachedByLimit;
                stats.attachedByKnown += f.attachedByKnown;
                stats.attachedByBudget += f.attachedByBudget;
                stats.attachedByUnloaded += f.attachedByUnloaded;
                stats.chunksDecoded += f.chunksDecoded;
                stats.floodVisits += f.floodVisits;
                stats.seedNodes += f.seedNodes;
                stats.boxComponents += f.boxComponents;
                stats.graphFloods += f.graphFloods;
                stats.attachedByFace += f.attachedByFace;
                stats.nodeVisits += f.nodeVisits;
                stats.chunkGraphs += f.chunkGraphs;
                stats.graphsBuilt += f.graphsBuilt;
                stats.overfullCells += f.overfullCells;
                stats.relabelledNodes += f.relabelledNodes;
                stats.outsideBox += f.outsideBox;
                stats.referenceMismatch += f.referenceMismatch;
                stats.neckRan = stats.neckRan || f.neckRan;
                stats.neckNodes += f.neckNodes;
                stats.neckSplit += f.neckSplit;
                stats.neckLoads += f.neckLoads;
                stats.neckAnchors += f.neckAnchors;
                stats.neckArcs += f.neckArcs;
                stats.neckExteriorVisits += f.neckExteriorVisits;
                stats.neckFlowWork += f.neckFlowWork;
                stats.neckSplitRelabels += f.neckSplitRelabels;
                stats.neckVoxels += f.neckVoxels;
                stats.neckOversize += f.neckOversize;
                stats.neckSkipped += f.neckSkipped;
                stats.neckExteriorBudget += f.neckExteriorBudget;
                stats.neckFlowBudget += f.neckFlowBudget;
                stats.neckUnloaded += f.neckUnloaded;
                stats.neckBeyondWindow += f.neckBeyondWindow;
                stats.neckLoading += f.neckLoading;
                stats.neckDeferredParts += f.neckDeferredParts;
                stats.neckSolidChunks += f.neckSolidChunks;
            }
            if (found.stats.neckUnloaded > 0) {
                Log::Error("Blast: the load check met {} chunk(s) that are not loaded inside the flood box of the blast at ({:.2f}, {:.2f}, "
                           "{:.2f}) m; they were taken as support (a session window must hold every flood box)",
                           found.stats.neckUnloaded, result.center.x, result.center.y, result.center.z);
            }
            if (found.stats.neckSkipped + found.stats.neckFlowBudget > 0) {
                Log::Warn("Blast: the load check at ({:.2f}, {:.2f}, {:.2f}) m released nothing: {}", result.center.x, result.center.y,
                          result.center.z, found.stats.neckSkipped ? "its network is over the node or arc limit" : "its flow work budget ran out");
            }
            if (found.stats.attachedByUnloaded > 0) {
                Log::Error("Blast: {} face flood(s) met a chunk that is not loaded inside the flood box of the blast at ({:.2f}, {:.2f}, "
                           "{:.2f}) m; what they would have freed stays (a session window must hold every flood box)",
                           found.stats.attachedByUnloaded, result.center.x, result.center.y, result.center.z);
            }
            if (!found.complete) {
                stats.floodDeferred = true;
                waitChunk = found.waitChunk;
                found.faceIslands.clear();
                found.neckPieces.clear();
                found.neckCuts.clear();
            } else if (found.neckWaits) {
                // Offline: the load check took a chunk that is not loaded as support (it released
                // at most part of the full answer). The islands go out now; the search runs again
                // once that chunk has loaded (a chunk outside the window: once the window moved).
                stats.neckDeferred = true;
                waitChunk = found.neckWaitChunk;
            }
            if (found.stats.neckDeferredParts > 0) {
                Log::Info("Blast: the load check at ({:.2f}, {:.2f}, {:.2f}) m left {} overloaded part(s) for a later cast (at most {} parts / "
                          "{} voxels per cast)",
                          result.center.x, result.center.y, result.center.z, found.stats.neckDeferredParts, kNeckMaxReleasedParts,
                          kNeckMaxReleasedVoxels);
            }
            const std::vector<voxel::FloatingIsland>& islands = found.boxIslands;
            const std::vector<voxel::FloatingIsland>& proven = found.faceIslands;
            const std::vector<voxel::FloatingIsland>& necks = found.neckPieces;
            stats.neckCuts.insert(stats.neckCuts.end(), found.neckCuts.begin(), found.neckCuts.end());
            simulation.IslandSearchCheck().neckReleases += static_cast<uint32_t>(necks.size());

            // Islands under the piece threshold are crumbs: no Detach each, one ClearMask for all.
            // The threshold is the piece volume in voxels of the session's size (shared bit for bit).
            const Clock::time_point start = Clock::now();
            const uint32_t minPiece = scene::MinPieceVoxels(static_cast<double>(world.VoxelSize()));
            glm::ivec3 crumbMin(std::numeric_limits<int32_t>::max()), crumbMax(std::numeric_limits<int32_t>::min());
            uint32_t crumbs = 0;
            // A session server: which pieces of the replicated size become server bodies. The tick's
            // slots go to the largest pieces first, then the lowest, then in emission order (shred.md
            // Part 4 "largest and lowest first"; review of stage 6: a tower the load check released
            // last lost its slot to two smaller in-box pieces). NetIds are handed out in that order;
            // the Detach order below stays the emission order (wire order).
            const std::array<const std::vector<voxel::FloatingIsland>*, 3> lists = {&islands, &proven, &necks};
            std::array<std::vector<uint32_t>, 3> netIds;
            if (simulation.Config().replication.enabled) {
                const uint32_t replicated = std::max(minPiece, simulation.Scene().Config().replicatedPieceVoxels);
                struct Candidate {
                    uint32_t voxels;
                    int32_t bottom;
                    uint32_t order;
                    size_t list, index;
                };
                std::vector<Candidate> candidates;
                uint32_t order = 0;
                for (size_t l = 0; l < lists.size(); ++l) {
                    netIds[l].assign(lists[l]->size(), 0u);
                    for (size_t i = 0; i < lists[l]->size(); ++i, ++order) {
                        const voxel::FloatingIsland& island = (*lists[l])[i];
                        if (island.voxelCount >= replicated) candidates.push_back({island.voxelCount, island.origin.y, order, l, i});
                    }
                }
                std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                    if (a.voxels != b.voxels) return a.voxels > b.voxels;
                    if (a.bottom != b.bottom) return a.bottom < b.bottom;
                    return a.order < b.order;
                });
                for (const Candidate& c : candidates) {
                    if (TakePieceSlot(simulation) != PieceSlot::Replicate) continue;
                    netIds[c.list][c.index] = AllocateNetId(simulation);
                    ++simulation.Replication().pendingPieces;
                }
            }
            // In-box islands in scan order, then proven ones in candidate order, then the parts the
            // load check released in piece order.
            for (size_t l = 0; l < lists.size(); ++l) {
                const std::vector<voxel::FloatingIsland>* list = lists[l];
                const bool face = list == &proven, neck = list == &necks;
                for (size_t i = 0; i < list->size(); ++i) {
                    const voxel::FloatingIsland& island = (*list)[i];
                    if (island.voxelCount < minPiece) {
                        ++crumbs;
                        if (face) ++stats.faceCrumbs;
                        if (neck) ++stats.neckCrumbs;
                        crumbMin = glm::min(crumbMin, island.origin);
                        crumbMax = glm::max(crumbMax, island.origin + island.volume.Size());
                        continue;
                    }
                    if (face) ++stats.facePieces;
                    if (neck) ++stats.neckPieces;
                    AppliedOp detach;
                    const glm::vec3 corner(world.VoxelToWorld(island.origin));
                    detach.op = WorldOp::Detach(island.origin, voxel::VoxelMask::FromVolume(island.volume), BlastPush(center, radius, corner));
                    // A session server: a piece given a slot above becomes a server body (made at the
                    // end of the blast); otherwise client rubble.
                    if (!netIds[l].empty()) detach.op.netId = netIds[l][i];
                    detach.result = ApplyOp(world, detach.op, &detach.edits);
                    result.ops.push_back(std::move(detach));
                }
            }
            if (crumbs > 0) {
                // The box lies inside the island box grown by less than MinPieceVoxels + 7 per side: a
                // proven crumb touches the island box face; a released crumb (load check) touches a
                // judged node, whose cell meets the island box, so its voxels lie at most 7 voxels
                // outside it - or is a load alone hanging on one (review). So it stays under
                // (192 + 2 x (minPiece + 7)) voxels per axis. Bits are set from the islands' own
                // voxels, so the mask does not depend on the order the islands come in.
                voxel::VoxelMask mask(crumbMax - crumbMin);
                for (const std::vector<voxel::FloatingIsland>* list : {&islands, &proven, &necks})
                    for (const voxel::FloatingIsland& island : *list) {
                        if (island.voxelCount >= minPiece) continue;
                        const glm::ivec3 size = island.volume.Size();
                        const glm::ivec3 offset = island.origin - crumbMin;
                        for (int z = 0; z < size.z; ++z)
                            for (int y = 0; y < size.y; ++y)
                                for (int x = 0; x < size.x; ++x) {
                                    if (island.volume.Get(x, y, z) != 0) mask.Set(offset + glm::ivec3(x, y, z), true);
                                }
                    }
                AppliedOp clear;
                clear.op = WorldOp::Clear(crumbMin, std::move(mask));
                clear.result = ApplyOp(world, clear.op, &clear.edits);
                result.ops.push_back(std::move(clear));
                result.crumbs += crumbs;
            }
            result.times.opsMs += MsSince(start);
            if (!found.complete) return SearchOutcome::FloodWaits;
            return found.neckWaits && !simulation.Config().sessionServer ? SearchOutcome::NeckWaits : SearchOutcome::Done;
        }

        /// Squared distance (voxels) from the centre of the island box of `sphere` to the middle of
        /// the world window: what a full list of deferred searches drops first.
        int64_t DistanceToWindow(const voxel::World& world, const voxel::SphereQ& sphere) {
            const glm::ivec3 mid = voxel::ChunkToVoxel(world.WindowMinChunk() + world.WindowSize() / 2);
            const glm::ivec3 c = math::FloorDiv(sphere.center, voxel::kQuantaPerVoxel);
            int64_t sum = 0;
            for (int a = 0; a < 3; ++a) {
                const int64_t d = static_cast<int64_t>(c[a]) - static_cast<int64_t>(mid[a]);
                sum += d * d;
            }
            return sum;
        }

        /// Remembers a search that has to wait; 1 if a full list dropped another one (logged).
        uint32_t Defer(Simulation& simulation, const DeferredIslandSearch& search) {
            std::vector<DeferredIslandSearch>& deferred = simulation.DeferredIslandSearches();
            uint32_t dropped = 0;
            if (deferred.size() >= kMaxDeferredIslandSearches) {
                const voxel::World& world = simulation.World();
                size_t farthest = 0;
                int64_t best = -1;
                for (size_t i = 0; i < deferred.size(); ++i) {
                    const int64_t d = DistanceToWindow(world, deferred[i].sphere);
                    if (d > best) {            // ties keep the oldest
                        best = d;
                        farthest = i;
                    }
                }
                const glm::dvec3 at = glm::dvec3(deferred[farthest].sphere.center) *
                                      (static_cast<double>(world.VoxelSize()) / static_cast<double>(voxel::kQuantaPerVoxel));
                Log::Warn("Blast: {} island searches are already waiting; the one of the blast at ({:.1f}, {:.1f}, {:.1f}) m (farthest "
                          "from the window) is dropped: what it would have freed stays in the air",
                          deferred.size(), at.x, at.y, at.z);
                deferred.erase(deferred.begin() + static_cast<std::ptrdiff_t>(farthest));
                dropped = 1;
            }
            deferred.push_back(search);
            return dropped;
        }

        /// Asks the world for what a waiting search needs: the island box, or the flood box once a
        /// flood met a chunk that is not loaded.
        void RequestFor(voxel::World& world, const voxel::SphereQ& sphere, SearchOutcome outcome) {
            glm::ivec3 min, max;
            if (outcome == SearchOutcome::FloodWaits || outcome == SearchOutcome::NeckWaits) FloodBox(sphere, min, max);
            else IslandBox(sphere, min, max);
            world.RequestLoad(voxel::VoxelToChunk(min), voxel::VoxelToChunk(max - glm::ivec3(1)));
        }

        /// A chunk row that can ever be loaded: any row, or in a fixed-height window only the
        /// rows of its range.
        bool CanLoad(const voxel::World& world, const glm::ivec3& chunk) {
            if (!world.FullHeight()) return true;
            const int bottomRow = world.WindowMinChunk().y;
            return chunk.y >= bottomRow && chunk.y < bottomRow + world.WindowSize().y;
        }

        void SetCenter(const voxel::World& world, const voxel::SphereQ& sphere, BlastResult& result) {
            constexpr double kInvQ = 1.0 / static_cast<double>(voxel::kQuantaPerVoxel);
            const double vs = static_cast<double>(world.VoxelSize());
            result.center = glm::dvec3(sphere.center) * (kInvQ * vs);
            result.radius = static_cast<float>(static_cast<double>(sphere.radius) * kInvQ * vs);
        }
    }

    namespace {
        /// What Scene::Damage left besides the kept entity: its fragments (pushed from `center`) and its new
        /// pieces. Replicated: server bodies within the piece budgets (Damage lists them largest first, so
        /// the slots go to the largest, as for terrain), otherwise fragments; offline the pieces stay bodies.
        void TakeDamagePieces(Simulation& simulation, scene::DamageResult& damage, const glm::vec3& center, float radius, bool replicate,
                              BlastResult& result) {
            scene::Scene& scene = simulation.Scene();
            physics::PhysicsWorld& physics = simulation.Physics();
            for (scene::DamageFragment& fragment : damage.fragments) {
                fragment.linearVelocity += BlastPush(center, radius, fragment.pose.position);
                result.fragments.push_back(std::move(fragment));
            }
            if (!replicate) return;
            for (const scene::Entity piece : damage.pieces) {
                if (TakePieceSlot(simulation) == PieceSlot::Replicate) {
                    ObjectEvent spawn;
                    spawn.kind = ObjectEvent::Kind::Spawn;
                    spawn.netId = Replicate(simulation, piece, true);
                    spawn.entity = piece;
                    result.objectEvents.push_back(std::move(spawn));
                    continue;
                }
                const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(piece);
                const scene::Transform* transform = scene.TryGet<scene::Transform>(piece);
                const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(piece);
                if (voxels && transform && body) {
                    const physics::BodyState state = physics.GetState(body->body);
                    result.fragments.push_back({voxels->model, transform->current,
                                                state.linearVelocity + BlastPush(center, radius, transform->current.position),
                                                state.angularVelocity});
                }
                scene.Destroy(piece);
            }
        }
    }

    BlastResult Blast(Simulation& simulation, const BlastDesc& desc) {
        const double vs = static_cast<double>(simulation.World().VoxelSize());
        return Blast(simulation, voxel::QuantizeSphere(desc.center / vs, static_cast<double>(desc.radius) / vs),
                     voxel::QuantizePower(desc.power));
    }

    BlastResult Blast(Simulation& simulation, const voxel::SphereQ& sphere, uint16_t power) {
        voxel::World& world = simulation.World();
        physics::PhysicsWorld& physics = simulation.Physics();
        scene::Scene& scene = simulation.Scene();

        BlastResult result;
        SetCenter(world, sphere, result);
        const glm::vec3 center(result.center);
        const float radius = result.radius;

        // 1. Carve.
        Clock::time_point start = Clock::now();
        std::vector<glm::ivec3> carvePending;
        {
            AppliedOp carve;
            carve.op = WorldOp::Carve(sphere, power);
            carve.result = ApplyOp(world, carve.op, &carve.edits);
            // Skipped chunks that can still load (not above the ceiling or below the bottom of a
            // fixed-height window), in the carve's own visit order.
            for (const glm::ivec3& chunk : carve.result.report.skipped)
                if (CanLoad(world, chunk)) carvePending.push_back(chunk);
            result.ops.push_back(std::move(carve));
        }
        result.times.carveMs = MsSince(start);

        // 2. Terrain that lost its support. Without the whole island box loaded nothing can be
        //    proven, and offline a flood that meets a chunk that is not loaded proves nothing
        //    either: the world loads what is missing and the search runs later
        //    (RetryIslandSearches), the carve first again if it skipped chunks.
        glm::ivec3 waitChunk(0);
        const SearchOutcome outcome = SearchIslands(simulation, sphere, result, waitChunk);
        result.islandsSearched = outcome == SearchOutcome::Done || outcome == SearchOutcome::NeckWaits;
        if (outcome == SearchOutcome::BoxNotLoaded || outcome == SearchOutcome::FloodWaits || outcome == SearchOutcome::NeckWaits) {
            RequestFor(world, sphere, outcome);
            DeferredIslandSearch search;
            search.sphere = sphere;
            search.power = power;
            search.carvePending = std::move(carvePending);
            search.waitsForChunk = outcome != SearchOutcome::BoxNotLoaded;
            search.waitChunk = waitChunk;
            result.deferredDropped = Defer(simulation, search);
        }

        // 3. Objects caught in the blast. The list is a snapshot: pieces made here are not hit twice.
        start = Clock::now();
        if (radius > 0.0f) {
            std::vector<scene::Entity> caught;
            scene.QueryBodies(math::AABB(center - glm::vec3(radius), center + glm::vec3(radius)), caught);
            const bool replicate = simulation.Config().replication.enabled;
            for (const scene::Entity entity : caught) {
                // A held body ([mech-1], mechanisms.md "One use key: E"): released first when the sphere meets
                // its posed model box, then damaged as any object; untouched when only the boxes' bounds meet.
                if (scene.TryGet<scene::Held>(entity)) {
                    const scene::Transform* transform = scene.TryGet<scene::Transform>(entity);
                    const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
                    const scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity);
                    if (!transform || !voxels || !voxels->model || !rigid) continue;
                    const float vs = world.VoxelSize();
                    const glm::vec3 size = glm::vec3(voxels->model->Volume().Size()) * vs;
                    const glm::vec3 local = glm::conjugate(transform->current.rotation) * (glm::vec3(result.center) - transform->current.position);
                    const glm::vec3 closest = glm::clamp(local, glm::vec3(0.0f), size);
                    if (glm::length(local - closest) > static_cast<float>(result.radius)) continue;
                    MechanismPose pose;
                    pose.position = glm::dvec3(transform->current.position);
                    pose.rotation = transform->current.rotation;
                    HoldChange release;
                    EndHold(simulation, entity, HoldEnd::Damaged, pose, physics.GetState(rigid->body).linearVelocity, nullptr, HoldEnv{},
                            simulation.Tick(), scene.TryGet<scene::NetId>(entity) != nullptr, release);
                    result.holdChanges.push_back(release);
                }
                // A mechanism ([mech-1], mechanisms.md "The damage rule"): only an aimed hit (within
                // kMechanismHitRadius of its model) damages it, by its own rule.
                if (scene.TryGet<scene::Mechanism>(entity)) {
                    if (DistanceToMechanism(simulation, entity, result.center) <= kMechanismHitRadius)
                        DamageMechanism(simulation, entity, result.center, power, result);
                    continue;
                }
                // A replicated object that Damage destroys broke apart (its remains are fragments).
                uint32_t netId = 0;
                if (scene::NetId* id = replicate ? scene.TryGet<scene::NetId>(entity) : nullptr) {
                    netId = id->value;
                    id->despawn = scene::DespawnReason::Broken;
                }
                scene::DamageResult damage = scene.Damage(entity, result.center, radius);
                if (netId != 0 && !damage.destroyed) {
                    if (scene::NetId* id = scene.TryGet<scene::NetId>(entity)) id->despawn = scene::DespawnReason::Destroyed;
                    const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
                    if (damage.removed > 0 && voxels) {
                        ObjectEvent model;
                        model.kind = ObjectEvent::Kind::Model;
                        model.netId = netId;
                        model.entity = entity;
                        model.model = voxels->model;
                        model.offset = damage.keptOffset;
                        model.motion = damage.keptMotion;
                        result.objectEvents.push_back(std::move(model));
                    }
                }
                if (damage.removed == 0) continue;
                ++result.objectsHit;
                result.objectVoxels += damage.removed;
                TakeDamagePieces(simulation, damage, center, radius, replicate, result);
            }
        }

        // 4. Push bodies outwards (the new pieces included) and wake what rested nearby.
        for (const auto [entity, body] : scene.Registry().view<const scene::RigidBody>().each()) {
            if (!physics.IsValid(body.body)) continue;
            if (scene.Registry().any_of<scene::Held, scene::Mechanism>(entity)) continue;   // posed, not pushed
            const glm::vec3 push = BlastPush(center, radius, physics.GetState(body.body).position);
            if (push == glm::vec3(0.0f)) continue;
            physics.AddImpulse(body.body, push * physics.GetMass(body.body));
        }
        physics.ActivateInBox(math::AABB(center - glm::vec3(radius * 2.0f), center + glm::vec3(radius * 2.0f)));

        // 5. Replication: the server bodies of the replicated terrain pieces, made from the voxels
        //    their Detach removed (after the push and the objects, as a client's rubble is), and the
        //    poses and velocities of the new object pieces after the push.
        if (simulation.Config().replication.enabled) {
            for (const AppliedOp& applied : result.ops) {
                if (applied.op.kind != WorldOpKind::Detach || applied.op.netId == 0) continue;
                voxel::VoxelVolume piece = BuildDetachedPiece(applied.op, applied.edits);
                scene::Entity entity = scene::kNoEntity;
                if (!piece.IsEmpty()) {
                    auto model = std::make_shared<voxel::VoxelModel>(std::move(piece), voxel::kWorldPalette, "piece");
                    entity = scene.CreateVoxelObject({.model = std::move(model),
                                                      .position = glm::vec3(world.VoxelToWorld(applied.op.min)),
                                                      .linearVelocity = applied.op.velocity});
                }
                if (entity == scene::kNoEntity) {
                    ++simulation.Replication().stats.bodyFailures;
                    result.lostNetIds.push_back(applied.op.netId);
                    Log::Warn("Blast: the server body of piece {} could not be made; it is despawned", applied.op.netId);
                    continue;
                }
                scene::NetId id;
                id.value = applied.op.netId;
                id.piece = true;
                id.bornTick = simulation.Tick();
                scene.Registry().emplace<scene::NetId>(entity, id);
            }
            simulation.Replication().pendingPieces = 0;
            for (ObjectEvent& e : result.objectEvents) {
                if (e.kind != ObjectEvent::Kind::Spawn) continue;
                const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(e.entity);
                const scene::Transform* transform = scene.TryGet<scene::Transform>(e.entity);
                const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(e.entity);
                if (!voxels || !transform || !body) continue;   // gone again (evicted by a later piece): its despawn follows
                const physics::BodyState state = physics.GetState(body->body);
                e.model = voxels->model;
                e.pose = transform->current;
                e.pose.position = state.position;
                e.pose.rotation = state.rotation;
                e.linearVelocity = physics.IsFrozen(body->body) ? state.keptLinearVelocity : state.linearVelocity;
                e.angularVelocity = state.angularVelocity;
                e.motion = body->motion;
            }
        }
        result.times.objectsMs = MsSince(start);
        return result;
    }

    std::vector<BlastResult> RetryIslandSearches(Simulation& simulation) {
        std::vector<BlastResult> out;
        std::vector<DeferredIslandSearch>& deferred = simulation.DeferredIslandSearches();
        if (deferred.empty()) return out;
        voxel::World& world = simulation.World();
        for (size_t i = 0; i < deferred.size();) {
            DeferredIslandSearch& search = deferred[i];
            ++search.attempts;
            glm::ivec3 islandMin, islandMax, floodMin, floodMax;
            IslandBox(search.sphere, islandMin, islandMax);
            FloodBox(search.sphere, floodMin, floodMax);

            BlastResult result;
            result.deferred = true;
            SetCenter(world, search.sphere, result);

            // Given up only once nothing of the flood box is in the window any more: until then
            // the window may still bring in what the search waits for.
            if (!BoxTouchesWindow(world, floodMin, floodMax)) {
                Log::Warn("Blast: the island search of the blast at ({:.1f}, {:.1f}, {:.1f}) m is given up after {} attempts: its flood "
                          "box left the window before {} loaded",
                          result.center.x, result.center.y, result.center.z, search.attempts,
                          search.waitsForChunk ? "the chunk a flood met" : "the island box");
                result.deferredGivenUp = 1;
                out.push_back(std::move(result));
                deferred.erase(deferred.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            // The blast's carve skipped chunks that were still loading: they arrive with the
            // generator's (or the save's) voxels. The carve is applied to each of them, and only to
            // them, in the first call after it loaded (Engine::Run calls this right after the
            // streaming, before anything else can edit it), so the search sees the crater and an
            // edit made since the blast in a chunk the carve did reach is never carved again.
            if (!search.carvePending.empty()) {
                const Clock::time_point start = Clock::now();
                AppliedOp carve;
                carve.op = WorldOp::Carve(search.sphere, search.power);
                std::vector<glm::ivec3> waiting;
                for (const glm::ivec3& chunk : search.carvePending) {
                    if (!world.IsChunkLoaded(chunk)) {
                        waiting.push_back(chunk);
                        continue;
                    }
                    carve.result.changed += world.CarveSphereQInChunk(search.sphere, search.power, chunk, &carve.edits, &carve.result.report);
                    result.carveReplayChunks.push_back(chunk);
                }
                if (!result.carveReplayChunks.empty()) {
                    result.ops.push_back(std::move(carve));
                    result.carveReplayed = true;
                    result.times.carveMs = MsSince(start);
                }
                search.carvePending = std::move(waiting);
            }

            const bool ready = (!search.waitsForChunk || world.IsChunkLoaded(search.waitChunk)) && BoxLoaded(world, islandMin, islandMax);
            if (!ready) {
                if (!result.ops.empty()) out.push_back(std::move(result));
                ++i;
                continue;
            }

            glm::ivec3 waitChunk(0);
            const SearchOutcome outcome = SearchIslands(simulation, search.sphere, result, waitChunk);
            result.islandsSearched = outcome == SearchOutcome::Done || outcome == SearchOutcome::NeckWaits;
            const bool done = outcome == SearchOutcome::Done || outcome == SearchOutcome::Refused;
            if (!done) {
                search.waitsForChunk = outcome != SearchOutcome::BoxNotLoaded;
                search.waitChunk = waitChunk;
                RequestFor(world, search.sphere, outcome);
            }
            if (!result.ops.empty()) out.push_back(std::move(result));
            if (done) deferred.erase(deferred.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
        return out;
    }

    void PresentBlast(Simulation& simulation, const BlastResult& blast) {
        voxel::World& world = simulation.World();
        voxel::MaterialLibrary& materials = simulation.Materials();
        fx::ParticleSystem& particles = simulation.Particles();
        scene::Scene& scene = simulation.Scene();
        const float vs = world.VoxelSize();
        const glm::dvec3 center = blast.center;
        const float radius = blast.radius;
        // [wp-8] (fx-budget.md "Particle cost"): at most 96 bursts per carving operation, dust on
        // every fourth, so ten blasts fit in the pool.
        constexpr size_t kMaxBursts = 96;
        constexpr size_t kDustEvery = 4;

        for (const AppliedOp& applied : blast.ops) {
            const std::vector<voxel::VoxelEdit>& removed = applied.edits;
            if (applied.op.kind == WorldOpKind::CarveSphere) {
                // Debris for a bounded sample of the removed voxels, in their material's colour.
                const size_t stride = std::max<size_t>(1, (removed.size() + kMaxBursts - 1) / kMaxBursts);
                size_t burst = 0;
                for (size_t i = 0; i < removed.size() && burst < kMaxBursts; i += stride, ++burst) {
                    const glm::dvec3 p = world.VoxelCenter(removed[i].voxel);
                    const glm::vec3 away = glm::normalize(glm::vec3(p - center) + glm::vec3(0.0f, 0.2f, 0.0f)) * 3.0f;
                    fx::SpawnVoxelBreak(particles, p, materials.Get(voxel::kWorldPalette, removed[i].oldId).color, vs, away,
                                        burst % kDustEvery == 0);
                }
            } else if (applied.op.kind == WorldOpKind::Detach && applied.op.netId == 0) {
                // Terrain that broke off, made from the voxels this world just removed. Rubble:
                // small lumps are cleaned up by the scene.
                voxel::VoxelVolume piece = BuildDetachedPiece(applied.op, applied.edits);
                if (piece.IsEmpty()) continue;
                auto model = std::make_shared<voxel::VoxelModel>(std::move(piece), voxel::kWorldPalette, "debris");
                scene.CreateVoxelObject({.model = std::move(model),
                                         .position = glm::vec3(world.VoxelToWorld(applied.op.min)),
                                         .linearVelocity = applied.op.velocity,
                                         .debris = true});
            } else if (applied.op.kind == WorldOpKind::ClearMask) {
                // Crumbs: particles where the voxels were, as Scene::CreateVoxelObject crumbles a
                // piece under minPieceVoxels, but without a model per crumb.
                const size_t stride = std::max<size_t>({1, scene.Config().debrisParticleStride, (removed.size() + kMaxBursts - 1) / kMaxBursts});
                size_t burst = 0;
                for (size_t i = 0; i < removed.size() && burst < kMaxBursts; i += stride, ++burst) {
                    const glm::dvec3 p = world.VoxelCenter(removed[i].voxel);
                    fx::SpawnVoxelBreak(particles, p, materials.Get(voxel::kWorldPalette, removed[i].oldId).color, vs,
                                        BlastPush(glm::vec3(center), radius, glm::vec3(p)), burst % kDustEvery == 0);
                }
            }
        }
        if (blast.deferred) return;     // terrain that fell later: no second explosion, no flash
        fx::SpawnExplosion(particles, center, radius);

        // Object pieces below the replicated threshold (none offline unless it is set).
        for (const scene::DamageFragment& fragment : blast.fragments) {
            scene.CreateVoxelObject({.model = fragment.model,
                                     .position = fragment.pose.position,
                                     .rotation = fragment.pose.rotation,
                                     .linearVelocity = fragment.linearVelocity,
                                     .angularVelocity = fragment.angularVelocity,
                                     .debris = true});
        }

        // A flash of light that fades out quickly and removes itself once it is too dim to see. It is an
        // unshadowed effect light (fx-budget.md "Effect lights"): the composite pass draws it inside an
        // axis box cut by the first wall along each of the six axis rays, so it does not light the far
        // side of a wall. A hit extent reaches half a voxel past the hit face: the composite pass
        // rebuilds each surface point from depth with float error, so a box face on the lit surface
        // would light about half of its pixels. A wall is at least 1 voxel thick, so its far side
        // stays outside the box.
        constexpr float kFlashDecay = 18.0f, kFlashCutoff = 0.05f;
        if (!(radius > 0.0f)) return;
        scene::LightDesc flash;
        flash.position = center;
        flash.color = glm::vec3(1.0f, 0.7f, 0.35f);
        flash.intensity = 40.0f * radius;
        flash.radius = radius * 4.0f;
        flash.decay = kFlashDecay;
        flash.lifetime = std::log(flash.intensity / kFlashCutoff) / kFlashDecay;
        flash.effect = true;
        std::array<float, 6> extent{};
        for (int axis = 0; axis < 3; ++axis) {
            for (int side = 0; side < 2; ++side) {
                glm::vec3 direction(0.0f);
                direction[axis] = side == 0 ? -1.0f : 1.0f;
                const voxel::WorldRayHit hit = world.Raycast(center, direction, flash.radius);
                extent[static_cast<size_t>(axis * 2 + side)] = hit.hit ? hit.distance + 0.5f * vs : flash.radius;
            }
        }
        flash.clipMin = -glm::vec3(extent[0], extent[2], extent[4]);
        flash.clipMax = glm::vec3(extent[1], extent[3], extent[5]);
        if (flash.lifetime > 0.0f) scene.CreateLight(flash);
    }

    MechanismHit DamageMechanism(Simulation& simulation, scene::Entity entity, const glm::dvec3& centre, uint16_t powerQ,
                                 BlastResult& into) {
        MechanismHit hit;
        scene::Scene& scene = simulation.Scene();
        scene::Mechanism* m = scene.TryGet<scene::Mechanism>(entity);
        if (!m || (m->desc.flags & scene::MechanismDesc_Warded) != 0) return hit;
        const MechanismKind kind = m->desc.kind;
        const glm::ivec3 keepMin = m->desc.hingeBox.min - m->modelOffset;
        const glm::ivec3 keepMax = m->desc.hingeBox.max - m->modelOffset;
        const bool replicate = simulation.Config().replication.enabled;

        uint32_t netId = 0;
        if (scene::NetId* id = replicate ? scene.TryGet<scene::NetId>(entity) : nullptr) {
            netId = id->value;
            id->despawn = scene::DespawnReason::Broken;
        }
        scene::DamageResult r = scene.Damage(entity, centre, kMechanismHitRadius,
                                             scene::DamageOptions{powerQ, true, keepMin, keepMax, true});
        if (netId != 0 && !r.destroyed)
            if (scene::NetId* id = scene.TryGet<scene::NetId>(entity)) id->despawn = scene::DespawnReason::Destroyed;
        if (r.removed == 0) return hit;
        hit.removed = r.removed;
        ++into.objectsHit;
        into.objectVoxels += r.removed;
        TakeDamagePieces(simulation, r, glm::vec3(centre), kMechanismHitRadius, replicate, into);

        // The survivor stays a mechanism: kinematic, posed by its timetable, in its layer.
        const uint32_t tick = simulation.Tick();
        if (!r.destroyed) {
            if ((m = scene.TryGet<scene::Mechanism>(entity))) {
                m->modelOffset += r.keptOffset;
                physics::PhysicsWorld& physics = simulation.Physics();
                if (scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity)) {
                    if (rigid->motion != physics::MotionType::Kinematic) {
                        rigid->motion = physics::MotionType::Kinematic;
                        physics.SetMotionType(rigid->body, physics::MotionType::Kinematic);
                    }
                    const float vs = simulation.World().VoxelSize();
                    const MechanismPose pose = PoseAt(*m, PositionAt(*m, tick), vs);
                    scene.SetTransform(entity, glm::vec3(pose.position), pose.rotation, true);
                    physics.SetLayer(rigid->body, MechanismLayer(*m, tick));
                }
                if (netId != 0) {
                    if (const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity)) {
                        ObjectEvent model;
                        model.kind = ObjectEvent::Kind::Model;
                        model.netId = netId;
                        model.entity = entity;
                        model.model = voxels->model;
                        model.offset = r.keptOffset;
                        model.motion = physics::MotionType::Kinematic;
                        into.objectEvents.push_back(std::move(model));
                    }
                }
            }
        }

        // The hinge is gone (or the whole mechanism): a lever breaks, a door or a portcullis is freed.
        if (r.destroyed || r.keptBoxVoxels == 0) {
            const uint8_t flag = kind == MechanismKind::Lever ? uint8_t{scene::MechanismFlag_Broken} : uint8_t{scene::MechanismFlag_Freed};
            if (!r.destroyed) ReleaseMechanism(simulation, entity, tick, flag, into.mechanismChanges);
            (kind == MechanismKind::Lever ? hit.broken : hit.freed) = true;
        }
        return hit;
    }

    BlastResult MechanismBlast(Simulation& simulation, scene::Entity target, const voxel::SphereQ& sphere, uint16_t power) {
        BlastResult result;
        SetCenter(simulation.World(), sphere, result);
        result.islandsSearched = true;
        DamageMechanism(simulation, target, result.center, power, result);

        // The push to every other body, as Blast step 4 does (mechanisms and held bodies are posed, not pushed).
        scene::Scene& scene = simulation.Scene();
        physics::PhysicsWorld& physics = simulation.Physics();
        const glm::vec3 center(result.center);
        const float radius = result.radius;
        for (const auto [entity, body] : scene.Registry().view<const scene::RigidBody>().each()) {
            if (!physics.IsValid(body.body)) continue;
            if (scene.Registry().any_of<scene::Mechanism, scene::Held>(entity)) continue;
            const glm::vec3 push = BlastPush(center, radius, physics.GetState(body.body).position);
            if (push == glm::vec3(0.0f)) continue;
            physics.AddImpulse(body.body, push * physics.GetMass(body.body));
        }
        physics.ActivateInBox(math::AABB(center - glm::vec3(radius * 2.0f), center + glm::vec3(radius * 2.0f)));
        return result;
    }
}
