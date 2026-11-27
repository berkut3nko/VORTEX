module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.voxel;

namespace vortex::sim {

    namespace {
        constexpr int64_t kCreditFloor = -8 * kSpreadWorkPerStep;

        /// One chunk of the step's neighbourhood as the step reads it: a Dense chunk's ids, a decoded copy of a Packed
        /// one, a Uniform / Empty chunk's one id, or nothing (not loaded: read as bedrock, inert in every rule).
        struct Source {
            const voxel::Chunk* chunk = nullptr;   ///< the Dense chunk or the decoded copy (`ids` is its ids)
            const uint8_t* ids = nullptr;
            uint8_t fill = 0;
            bool present = false;
            std::unique_ptr<voxel::Chunk> own;
        };

        void ReadSource(const voxel::World& world, const glm::ivec3& chunk, Source& out) {
            if (!world.IsChunkLoaded(chunk)) return;
            out.present = true;
            uint8_t uniform = 0;
            if (const voxel::Chunk* dense = world.GetChunk(chunk, &uniform)) {
                out.chunk = dense;
                out.ids = dense->ids;
                return;
            }
            if (world.StateOf(chunk) == voxel::CellState::Packed) {
                out.own = std::make_unique<voxel::Chunk>();
                if (world.ReadChunk(chunk, *out.own)) {
                    out.chunk = out.own.get();
                    out.ids = out.own->ids;
                    return;
                }
            }
            out.fill = uniform;
        }

        void SetBit(voxel::BlockMask& mask, uint32_t block) { mask[block >> 6] |= uint64_t{1} << (block & 63u); }
        bool Any(const voxel::BlockMask& mask) {
            for (const uint64_t w : mask)
                if (w != 0) return true;
            return false;
        }
        uint32_t Count(const voxel::BlockMask& mask) {
            uint32_t n = 0;
            for (const uint64_t w : mask) n += static_cast<uint32_t>(std::popcount(w));
            return n;
        }

        /// The blocks of a chunk at `offset` from `chunk` (each axis -1..1, not all 0) within one block of `live`
        /// blocks of `chunk`, across their shared face, edge or corner.
        voxel::BlockMask AcrossBoundary(const voxel::BlockMask& live, const glm::ivec3& offset) {
            voxel::BlockMask out{};
            for (uint32_t w = 0; w < 8; ++w) {
                uint64_t bits = live[w];
                while (bits != 0) {
                    const uint32_t block = w * 64u + static_cast<uint32_t>(std::countr_zero(bits));
                    bits &= bits - 1;
                    const glm::ivec3 b(static_cast<int>(block & 7u), static_cast<int>((block >> 3) & 7u), static_cast<int>(block >> 6));
                    bool near = true;
                    for (int a = 0; a < 3 && near; ++a) near = offset[a] == 0 || b[a] == (offset[a] > 0 ? 7 : 0);
                    if (!near) continue;
                    const glm::ivec3 p = b - offset * 8;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const glm::ivec3 t = p + glm::ivec3(dx, dy, dz);
                                if (t.x < 0 || t.y < 0 || t.z < 0 || t.x > 7 || t.y > 7 || t.z > 7) continue;
                                SetBit(out, static_cast<uint32_t>(t.x + t.y * 8 + t.z * 64));
                            }
                }
            }
            return out;
        }

        /// The first collider at or below `from` - 1 in its column, at most 512 voxels down.
        bool ColumnFloor(const Simulation& simulation, const glm::ivec3& from, glm::ivec3& out) {
            const voxel::World& world = simulation.World();
            const voxel::MaterialLibrary* materials = world.Materials();
            for (int d = 1; d <= 512; ++d) {
                const glm::ivec3 v = from - glm::ivec3(0, d, 0);
                const uint8_t id = world.GetVoxel(v);
                if (id == 0) continue;
                if (materials != nullptr && (materials->Get(voxel::kWorldPalette, id).flags & voxel::kNonStructuralFlags) != 0) continue;
                out = v;
                return true;
            }
            return false;
        }
    }

    uint32_t SpreadDriver::BurningVoxels() const {
        uint32_t n = 0;
        for (const auto& [key, entry] : m_Index) n += entry.burning;
        return n;
    }

    uint32_t SpreadDriver::BurningChunks() const {
        uint32_t n = 0;
        for (const auto& [key, entry] : m_Index) n += entry.burning > 0 ? 1u : 0u;
        return n;
    }

    void SpreadDriver::Recount(const voxel::World& world, const glm::ivec3& chunk) {
        const voxel::SpreadTable& table = voxel::WorldSpreadTable();
        Source source;
        ReadSource(world, chunk, source);
        Entry entry;
        if (source.chunk != nullptr) {
            entry.burning = voxel::LiveBlocks(table, *source.chunk, entry.live);
        } else if (source.present && table.kind[source.fill] == voxel::SpreadKind::Burning) {
            entry.live.fill(~uint64_t{0});
            entry.burning = static_cast<uint32_t>(voxel::kChunkVolume);
        }
        if (entry.burning == 0) {
            m_Index.erase(KeyOf(chunk));
            return;
        }
        m_Index[KeyOf(chunk)] = entry;
    }

    bool SpreadDriver::CapsRefuse(const voxel::World& world, const glm::dvec3& center) const {
        const double chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(world.VoxelSize());
        uint32_t global = 0, local = 0;
        for (const auto& [key, entry] : m_Index) {
            if (entry.burning == 0) continue;
            ++global;
            const glm::dvec3 middle = (glm::dvec3(ChunkOf(key)) + glm::dvec3(0.5)) * chunkMetres;
            if (glm::distance(middle, center) <= kKindleLocalMetres) ++local;
        }
        return global >= kKindleGlobalChunks || local >= kKindleLocalChunks;
    }

    KindleResult SpreadDriver::Kindle(Simulation& simulation, const glm::dvec3& center, double radiusMetres, std::vector<voxel::VoxelEdit>* edits) {
        voxel::World& world = simulation.World();
        if (CapsRefuse(world, center)) {
            ++m_Stats.kindlesRefused;
            return KindleResult::TooMuchFire;
        }
        const double vs = static_cast<double>(world.VoxelSize());
        const voxel::SphereQ sphere = voxel::QuantizeSphere(center / vs, radiusMetres / vs);
        voxel::ShapeQ shape;
        shape.kind = voxel::kShapeSphere;
        shape.a = sphere.center;
        shape.radiusQ = sphere.radius;
        std::vector<voxel::VoxelEdit> lit;
        const OpResult result = ApplyOp(world, WorldOp::Map(voxel::Map_Ignite, shape), &lit);
        if (result.changed == 0) return KindleResult::Nothing;
        for (const glm::ivec3& chunk : result.report.changed) Recount(world, chunk);
        for (const voxel::VoxelEdit& edit : lit) m_Stats.lowestKindleRow = std::min(m_Stats.lowestKindleRow, edit.voxel.y);
        if (edits != nullptr) edits->insert(edits->end(), lit.begin(), lit.end());
        ++m_Stats.kindles;
        m_Stats.peakBurningVoxels = std::max(m_Stats.peakBurningVoxels, BurningVoxels());
        return KindleResult::Done;
    }

    void SpreadDriver::QueueSite(const glm::ivec3& chunk) {
        const Key key = KeyOf(chunk);
        if (m_SiteSet.contains(key)) return;
        if (m_Sites.size() >= kMaxQueuedSites) {
            ++m_Stats.sitesDropped;
            return;
        }
        m_SiteSet.insert(key);
        m_Sites.push_back(chunk);
    }

    std::vector<BlastResult> SpreadDriver::Tick(Simulation& simulation, std::vector<voxel::VoxelEdit>* stepEdits) {
        std::vector<BlastResult> out;
        const uint32_t tick = simulation.Tick();
        RunLandings(simulation, tick, out, stepEdits);
        if (tick % kSpreadSlotTicks != 0) return out;
        voxel::World& world = simulation.World();
        // New fire anywhere (a Kindle, a spell's pitch, a landing) and fire that edits removed: the chunks edits changed.
        std::vector<glm::ivec3> edited = world.TakeEditedChunks();
        std::sort(edited.begin(), edited.end(), [](const glm::ivec3& a, const glm::ivec3& b) { return KeyOf(a) < KeyOf(b); });
        edited.erase(std::unique(edited.begin(), edited.end()), edited.end());
        for (const glm::ivec3& chunk : edited) Recount(world, chunk);
        m_Credit = std::min(m_Credit + kSpreadWorkPerStep, kSpreadWorkPerStep);
        if (m_Credit < 0) {
            ++m_Stats.slotsWaited;
            return out;
        }
        Step(simulation, stepEdits);
        RunSites(simulation, out);
        return out;
    }

    void SpreadDriver::Step(Simulation& simulation, std::vector<voxel::VoxelEdit>* stepEdits) {
        const auto start = std::chrono::steady_clock::now();
        voxel::World& world = simulation.World();
        const voxel::SpreadTable& table = voxel::WorldSpreadTable();

        // The awake chunks (map order: y, z, x) and what each stepped chunk evaluates.
        std::map<Key, voxel::BlockMask> eval;
        std::vector<Key> awake;
        for (const auto& [key, entry] : m_Index)
            if (entry.burning > 0) awake.push_back(key);
        if (awake.empty()) {
            ++m_S;
            ++m_Stats.silentSteps;
            return;
        }
        for (const Key& key : awake) {
            const Entry& entry = m_Index.at(key);
            voxel::BlockMask& mask = eval[key];
            const voxel::BlockMask grown = voxel::DilateBlocks(entry.live);
            for (size_t w = 0; w < mask.size(); ++w) mask[w] |= grown[w];
            const glm::ivec3 chunk = ChunkOf(key);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0 && dz == 0) continue;
                        const glm::ivec3 offset(dx, dy, dz);
                        const glm::ivec3 neighbour = chunk + offset;
                        const voxel::BlockMask across = AcrossBoundary(entry.live, offset);
                        if (!Any(across)) continue;
                        const Key nkey = KeyOf(neighbour);
                        if (!eval.contains(nkey) && !world.IsChunkLoaded(neighbour)) continue;
                        voxel::BlockMask& target = eval[nkey];
                        for (size_t w = 0; w < target.size(); ++w) target[w] |= across[w];
                    }
        }
        const auto workOf = [&eval]() {
            int64_t work = 0;
            for (const auto& [key, mask] : eval) work += 1000 + 64 * static_cast<int64_t>(Count(mask));
            return work;
        };
        int64_t work = workOf();
        if (work > 2 * kSpreadWorkPerStep) {
            // Over the hard cap: the awake chunks only; the fire waits at the chunk borders this step.
            for (auto it = eval.begin(); it != eval.end();) {
                const auto found = m_Index.find(it->first);
                if (found == m_Index.end() || found->second.burning == 0) it = eval.erase(it);
                else ++it;
            }
            ++m_Stats.hardCapSteps;
            work = workOf();
        }
        m_Credit = std::max(m_Credit - work, kCreditFloor);

        std::vector<glm::ivec3> stepped;
        std::vector<voxel::BlockMask> masks;
        for (const auto& [key, mask] : eval) {
            const glm::ivec3 chunk = ChunkOf(key);
            if (!world.IsChunkLoaded(chunk)) {
                m_Index.erase(key);   // left the window while burning: it leaves the index
                continue;
            }
            stepped.push_back(chunk);
            masks.push_back(mask);
        }
        if (reverseOrder) {
            std::reverse(stepped.begin(), stepped.end());
            std::reverse(masks.begin(), masks.end());
        }

        // Every chunk of the step and its neighbours, read once on the main thread; the workers only read them.
        std::map<Key, Source> sources;
        for (const glm::ivec3& chunk : stepped)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const glm::ivec3 c = chunk + glm::ivec3(dx, dy, dz);
                        const Key key = KeyOf(c);
                        if (sources.contains(key)) continue;
                        ReadSource(world, c, sources[key]);
                    }

        ++m_S;
        const uint32_t s = m_S;
        const size_t count = stepped.size();
        std::vector<uint8_t> outputs(count * static_cast<size_t>(voxel::kChunkVolume));
        std::vector<voxel::SpreadCounts> counts(count);
        simulation.Jobs().ParallelFor(count, 1, [&](size_t begin, size_t end) {
            auto box = std::make_unique<voxel::SpreadBox>();
            for (size_t i = begin; i < end; ++i) {
                const glm::ivec3 chunk = stepped[i];
                std::array<const Source*, 27> around{};
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            around[static_cast<size_t>((dx + 1) + (dy + 1) * 3 + (dz + 1) * 9)] = &sources.at(KeyOf(chunk + glm::ivec3(dx, dy, dz)));
                constexpr int n = voxel::kChunkSize;
                for (int z = -voxel::kSpreadPad; z < n + voxel::kSpreadPad; ++z) {
                    const int fz = z < 0 ? -1 : (z >= n ? 1 : 0), lz = z - fz * n;
                    for (int y = -voxel::kSpreadPad; y < n + voxel::kSpreadPad; ++y) {
                        const int fy = y < 0 ? -1 : (y >= n ? 1 : 0), ly = y - fy * n;
                        for (int x = -voxel::kSpreadPad; x < n + voxel::kSpreadPad; ++x) {
                            const int fx = x < 0 ? -1 : (x >= n ? 1 : 0), lx = x - fx * n;
                            const Source& src = *around[static_cast<size_t>((fx + 1) + (fy + 1) * 3 + (fz + 1) * 9)];
                            uint8_t id = table.bedrock;
                            if (src.ids != nullptr) id = src.ids[voxel::Chunk::VoxelIndex(lx, ly, lz)];
                            else if (src.present) id = src.fill;
                            box->ids[voxel::SpreadBox::Index(x, y, z)] = id;
                        }
                    }
                }
                uint8_t* out = outputs.data() + i * static_cast<size_t>(voxel::kChunkVolume);
                const Source& self = *around[13];
                if (self.ids != nullptr) std::memcpy(out, self.ids, static_cast<size_t>(voxel::kChunkVolume));
                else std::memset(out, self.fill, static_cast<size_t>(voxel::kChunkVolume));
                voxel::StepBox(table, *box, chunk, s, masks[i], out, counts[i]);
            }
        });
        sources.clear();   // the writes below change the chunks the sources point into

        for (size_t i = 0; i < count; ++i) {
            const voxel::SpreadCounts& c = counts[i];
            m_Stats.evaluated += c.evaluated;
            m_Stats.ignited += c.ignited;
            m_Stats.burnedOut += c.burnedOut;
            m_Stats.quenched += c.quenched;
            if (c.ignited > 0) m_Stats.lowestStepIgnition = std::min(m_Stats.lowestStepIgnition, stepped[i].y * voxel::kChunkSize + c.lowestIgnited);
            if (c.ignited + c.advanced + c.quenched == 0) continue;
            const uint32_t changed = world.EditVoxelsInChunk(stepped[i], outputs.data() + i * static_cast<size_t>(voxel::kChunkVolume), stepEdits);
            if (changed == 0) continue;
            if (c.shapeChanged) ++m_Stats.recooks;
            else ++m_Stats.recooksSkipped;
            Recount(world, stepped[i]);
            if (c.site) QueueSite(stepped[i]);
        }
        (void)world.TakeEditedChunks();   // the step's own writes, counted above

        ++m_Stats.steps;
        m_Stats.awakeMax = std::max(m_Stats.awakeMax, static_cast<uint32_t>(awake.size()));
        m_Stats.steppedMax = std::max(m_Stats.steppedMax, static_cast<uint32_t>(count));
        m_Stats.peakBurningVoxels = std::max(m_Stats.peakBurningVoxels, BurningVoxels());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        m_Stats.msSum += ms;
        m_Stats.msMax = std::max(m_Stats.msMax, ms);
    }

    void SpreadDriver::RunSites(Simulation& simulation, std::vector<BlastResult>& out) {
        const voxel::SpreadTable& table = voxel::WorldSpreadTable();
        voxel::World& world = simulation.World();
        for (uint32_t n = 0; n < kMaxSitesPerStep && !m_Sites.empty(); ++n) {
            const glm::ivec3 chunk = m_Sites.front();
            m_Sites.pop_front();
            m_SiteSet.erase(KeyOf(chunk));
            BlastResult result = Blast(simulation, ChunkSite(world, chunk), BlastOptions{.noCarve = true});
            ++m_Stats.sites;
            m_Stats.releases += result.IslandCount();
            // A released piece that holds fire lights the floor under its lowest burning voxel when it would land there
            // (decided now, from the column under it: never from the piece's body).
            for (const AppliedOp& applied : result.ops) {
                if (applied.op.kind != WorldOpKind::Detach) continue;
                uint32_t burning = 0;
                glm::ivec3 lowest(0);
                for (const voxel::VoxelEdit& edit : applied.edits) {
                    if (table.kind[edit.oldId] != voxel::SpreadKind::Burning) continue;
                    if (burning == 0 || std::tie(edit.voxel.y, edit.voxel.z, edit.voxel.x) < std::tie(lowest.y, lowest.z, lowest.x)) lowest = edit.voxel;
                    ++burning;
                }
                if (burning < kLandingMinBurning) continue;
                glm::ivec3 floor;
                if (!ColumnFloor(simulation, lowest, floor)) {
                    ++m_Stats.landingsDropped;
                    continue;
                }
                if (m_FallSq1024 == 0) {
                    const double dt = static_cast<double>(simulation.Config().fixedTimeStep);
                    m_FallSq1024 = std::llround(2.0 * static_cast<double>(world.VoxelSize()) / 9.81 / (dt * dt) * 1024.0);
                }
                const int64_t drop = lowest.y - floor.y;
                const uint32_t fallTicks = static_cast<uint32_t>(ISqrt64(drop * m_FallSq1024 / 1024)) + 1u;
                m_Landings.emplace(simulation.Tick() + fallTicks, Landing{lowest});
            }
            out.push_back(std::move(result));
        }
    }

    void SpreadDriver::RunLandings(Simulation& simulation, uint32_t tick, std::vector<BlastResult>& out, std::vector<voxel::VoxelEdit>* stepEdits) {
        (void)out;
        while (!m_Landings.empty() && m_Landings.begin()->first <= tick) {
            const Landing landing = m_Landings.begin()->second;
            m_Landings.erase(m_Landings.begin());
            // The world may have changed while it fell: the column is walked again from the same voxel.
            glm::ivec3 floor;
            if (!ColumnFloor(simulation, landing.from, floor)) {
                ++m_Stats.landingsDropped;
                continue;
            }
            const double vs = static_cast<double>(simulation.World().VoxelSize());
            const glm::dvec3 top = (glm::dvec3(floor) + glm::dvec3(0.5, 1.0, 0.5)) * vs;
            if (Kindle(simulation, top, kLandingRadiusMetres, stepEdits) == KindleResult::Done) ++m_Stats.landingsEmitted;
            else ++m_Stats.landingsDropped;
        }
    }

    std::string FormatSpreadLine(const SpreadStats& stats, uint32_t s) {
        const double avg = stats.steps > 0 ? stats.msSum / static_cast<double>(stats.steps) : 0.0;
        return std::format("Spread: s {}, steps {} (waited {}, silent {}, hard cap {}), awake {} max, stepped {} max, evaluated {}, ignited {}, "
                           "burned out {}, quenched {}, recooks {} (skipped {}), sites {} (dropped {}), releases {}, landings {} (dropped {}), "
                           "ms avg {:.3f} max {:.3f}, table {:016x}",
                           s, stats.steps, stats.slotsWaited, stats.silentSteps, stats.hardCapSteps, stats.awakeMax, stats.steppedMax, stats.evaluated,
                           stats.ignited, stats.burnedOut, stats.quenched, stats.recooks, stats.recooksSkipped, stats.sites, stats.sitesDropped,
                           stats.releases, stats.landingsEmitted, stats.landingsDropped, avg, stats.msMax,
                           voxel::SpreadTableHash(voxel::WorldSpreadTable()));
    }
}
