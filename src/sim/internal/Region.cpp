module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;

namespace vortex::sim {

    namespace {
        bool SameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

        bool SameTerrain(const voxel::TerrainSettings& a, const voxel::TerrainSettings& b) {
            const voxel::WorldMaterials& x = a.materials;
            const voxel::WorldMaterials& y = b.materials;
            const bool ids = x.grass == y.grass && x.grassDry == y.grassDry && x.dirt == y.dirt && x.stone == y.stone &&
                             x.stoneDark == y.stoneDark && x.sand == y.sand && x.snow == y.snow && x.water == y.water && x.wood == y.wood &&
                             x.leaves == y.leaves && x.leavesAutumn == y.leavesAutumn && x.planks == y.planks && x.brick == y.brick &&
                             x.iron == y.iron && x.gold == y.gold && x.crystal == y.crystal && x.lava == y.lava && x.torch == y.torch &&
                             x.glass == y.glass && x.bedrock == y.bedrock;
            return ids && a.seed == b.seed && SameBits(a.voxelSize, b.voxelSize) && SameBits(a.seaLevel, b.seaLevel) &&
                   SameBits(a.baseHeight, b.baseHeight) && SameBits(a.continentHeight, b.continentHeight) &&
                   SameBits(a.hillHeight, b.hillHeight) && SameBits(a.mountainHeight, b.mountainHeight) &&
                   SameBits(a.horizontalScale, b.horizontalScale) && SameBits(a.snowLine, b.snowLine) && a.caves == b.caves &&
                   SameBits(a.caveMaxDepth, b.caveMaxDepth) && a.trees == b.trees && SameBits(a.treeSpacing, b.treeSpacing) &&
                   SameBits(a.treeDensity, b.treeDensity) && a.skipClassifiedRows == b.skipClassifiedRows;
        }

    }

    bool RegionDesc::operator==(const RegionDesc& o) const {
        return templateId == o.templateId && seed == o.seed && playMinChunk == o.playMinChunk && playMaxChunk == o.playMaxChunk &&
               SameBits(heightRange.x, o.heightRange.x) && SameBits(heightRange.y, o.heightRange.y) && generator == o.generator &&
               flatGroundVoxels == o.flatGroundVoxels && SameTerrain(terrain, o.terrain);
    }

    bool IsValid(const RegionDesc& region, std::string* why) {
        auto fail = [why](const char* reason) {
            if (why) *why = reason;
            return false;
        };
        if (!voxel::IsValid(region.terrain)) return fail("terrain settings out of range");
        if (region.templateId != RegionTemplateId::Empty && region.templateId != RegionTemplateId::Playground)
            return fail("unknown template");
        if (region.generator != RegionGenerator::Terrain && region.generator != RegionGenerator::Flat) return fail("unknown generator");
        const glm::ivec2 size = region.playMaxChunk - region.playMinChunk;
        if (size.x <= 0 || size.y <= 0) return fail("empty play bounds");
        if (size.x > kMaxRegionChunks || size.y > kMaxRegionChunks) return fail("play bounds too large");
        constexpr int32_t kLimit = 1 << 24;        // chunk coordinates stay far inside +-2^25
        if (std::abs(region.playMinChunk.x) > kLimit || std::abs(region.playMinChunk.y) > kLimit ||
            std::abs(region.playMaxChunk.x) > kLimit || std::abs(region.playMaxChunk.y) > kLimit)
            return fail("play bounds out of range");
        const glm::vec2 h = region.heightRange;
        if (!std::isfinite(h.x) || !std::isfinite(h.y) || !(h.y > h.x) || std::abs(h.x) > 100000.0f || std::abs(h.y) > 100000.0f)
            return fail("bad height range");
        const glm::ivec2 rows = RegionRows(region);
        const int rowMin = rows.x, rowMax = rows.y;
        if (rowMax - rowMin <= 0 || rowMax - rowMin > 128) return fail("height range is not 1..128 chunk rows");
        if (region.generator == RegionGenerator::Flat) {
            const int64_t g = region.flatGroundVoxels;
            if (g < static_cast<int64_t>(rowMin) * voxel::kChunkSize || g > static_cast<int64_t>(rowMax) * voxel::kChunkSize)
                return fail("flat ground outside the height range");
        }
        if (region.templateId == RegionTemplateId::Playground) {
            // PlaygroundTemplate searches sites around the origin and keeps those whose plaza lies in
            // the play bounds: the one at the origin must be among them.
            const double chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(region.terrain.voxelSize);
            const glm::dvec2 lo = glm::dvec2(region.playMinChunk) * chunkMetres, hi = glm::dvec2(region.playMaxChunk) * chunkMetres;
            if (lo.x > -kPlaygroundHalfMetres || lo.y > -kPlaygroundHalfMetres || hi.x < kPlaygroundHalfMetres || hi.y < kPlaygroundHalfMetres)
                return fail("a playground's play bounds must contain the plaza site around the origin");
        }
        return true;
    }

    glm::ivec2 RegionRows(const RegionDesc& region) {
        // The same arithmetic as World's constructor for a full-height range.
        const double chunkMetres = static_cast<double>(voxel::kChunkSize) * region.terrain.voxelSize;
        const double snap = static_cast<double>(kRegionHeightSnap);
        const double lo = std::floor(static_cast<double>(region.heightRange.x) / snap) * snap;
        const double hi = std::ceil(static_cast<double>(region.heightRange.y) / snap) * snap;
        return glm::ivec2(static_cast<int>(std::floor(lo / chunkMetres + 1.0e-6)), static_cast<int>(std::ceil(hi / chunkMetres - 1.0e-6)));
    }

    glm::ivec3 RegionWindowMinChunk(const RegionDesc& region) {
        return glm::ivec3(region.playMinChunk.x - kServerWindowMarginChunks, 0, region.playMinChunk.y - kServerWindowMarginChunks);
    }

    PlayArea PlayAreaOf(const RegionDesc& region, const PlayerTuning& tuning) {
        const double chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(region.terrain.voxelSize);
        const double radius = static_cast<double>(tuning.radius);
        PlayArea area;
        area.min = glm::vec2(static_cast<float>(region.playMinChunk.x * chunkMetres + radius),
                             static_cast<float>(region.playMinChunk.y * chunkMetres + radius));
        area.max = glm::vec2(static_cast<float>(region.playMaxChunk.x * chunkMetres - radius),
                             static_cast<float>(region.playMaxChunk.y * chunkMetres - radius));
        return area;
    }

    PlayArea PlayBoundsOf(const RegionDesc& region) {
        const double chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(region.terrain.voxelSize);
        PlayArea b;
        b.min = glm::vec2(static_cast<float>(region.playMinChunk.x * chunkMetres), static_cast<float>(region.playMinChunk.y * chunkMetres));
        b.max = glm::vec2(static_cast<float>(region.playMaxChunk.x * chunkMetres), static_cast<float>(region.playMaxChunk.y * chunkMetres));
        return b;
    }

    glm::ivec3 RegionWindowSize(const RegionDesc& region) {
        const glm::ivec2 s = region.playMaxChunk - region.playMinChunk + glm::ivec2(2 * kServerWindowMarginChunks);
        return glm::ivec3(s.x, 1, s.y);
    }

    voxel::WorldConfig RegionWorldConfig(const RegionDesc& region, voxel::WorldConfig base) {
        base.voxelSize = region.terrain.voxelSize;
        base.windowSize = RegionWindowSize(region);
        base.saveDirectory.clear();
        base.fullHeight = true;
        base.heightRange = region.heightRange;
        base.heightSnap = kRegionHeightSnap;
        return base;
    }

    glm::dvec3 RegionWindowFocus(const RegionDesc& region) {
        const glm::ivec2 rows = RegionRows(region);
        const glm::ivec3 size = RegionWindowSize(region);
        const glm::ivec3 chunk = RegionWindowMinChunk(region) + size / 2;
        const glm::ivec3 voxel = voxel::ChunkToVoxel(glm::ivec3(chunk.x, (rows.x + rows.y) / 2, chunk.z)) + glm::ivec3(voxel::kChunkSize / 2);
        return (glm::dvec3(voxel) + glm::dvec3(0.5)) * static_cast<double>(region.terrain.voxelSize);
    }

    std::shared_ptr<voxel::IWorldGenerator> MakeRegionGenerator(const RegionDesc& region, const voxel::WorldMaterials& ids) {
        if (region.generator == RegionGenerator::Flat) return voxel::CreateFlatGenerator(region.flatGroundVoxels, ids.grass, ids.stone);
        voxel::TerrainSettings settings = region.terrain;
        settings.materials = ids;
        return voxel::CreateTerrainGenerator(settings);
    }

    bool InPlayBounds(const RegionDesc& region, const voxel::World& world, const voxel::SphereQ& sphere) {
        const glm::ivec3 chunk = voxel::VoxelToChunk(math::FloorDiv(sphere.center, voxel::kQuantaPerVoxel));
        const int rowMin = world.WindowMinChunk().y;
        return chunk.x >= region.playMinChunk.x && chunk.x < region.playMaxChunk.x && chunk.z >= region.playMinChunk.y &&
               chunk.z < region.playMaxChunk.y && chunk.y >= rowMin && chunk.y < rowMin + world.WindowSize().y;
    }

    bool CastFitsRegion(const RegionDesc& region, const voxel::World& world, const voxel::SphereQ& sphere) {
        if (!InPlayBounds(region, world, sphere) || sphere.radius > kMaxCastRadiusQ) return false;
        glm::ivec3 min, max;
        voxel::SphereVoxelBounds(sphere, min, max);
        const int rowMin = world.WindowMinChunk().y;
        return min.y >= rowMin * voxel::kChunkSize && max.y <= (rowMin + world.WindowSize().y) * voxel::kChunkSize;
    }

    bool WindowIsRegions(const voxel::World& world, const RegionDesc& region) {
        const glm::ivec3 corner = RegionWindowMinChunk(region);
        const glm::ivec2 rows = RegionRows(region);
        const glm::ivec3 size = RegionWindowSize(region);
        return world.WindowMinChunk() == glm::ivec3(corner.x, rows.x, corner.z) &&
               world.WindowSize() == glm::ivec3(size.x, rows.y - rows.x, size.z);
    }

    uint32_t RegionChunkCapacity(const RegionDesc& region) {
        const glm::ivec3 size = RegionWindowSize(region);
        const glm::ivec2 rows = RegionRows(region);
        const uint64_t cells = static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.z) * static_cast<uint64_t>(rows.y - rows.x);
        return static_cast<uint32_t>(std::clamp<uint64_t>(cells, 1024, kMaxRegionStoreSlots));
    }

    bool ClientWindowMoves(const RegionDesc& region, int32_t windowChunks) {
        if (windowChunks <= 0) return false;
        const glm::ivec3 size = RegionWindowSize(region);
        return size.x != windowChunks || size.z != windowChunks;
    }

    glm::ivec3 ClientWindowSize(const RegionDesc& region, int32_t windowChunks) {
        const glm::ivec2 rows = RegionRows(region);
        if (!ClientWindowMoves(region, windowChunks)) {
            const glm::ivec3 size = RegionWindowSize(region);
            return glm::ivec3(size.x, rows.y - rows.x, size.z);
        }
        return glm::ivec3(windowChunks, rows.y - rows.x, windowChunks);
    }

    voxel::WorldConfig ClientWorldConfig(const RegionDesc& region, int32_t windowChunks, voxel::WorldConfig base) {
        base = RegionWorldConfig(region, base);
        if (ClientWindowMoves(region, windowChunks)) {
            base.windowSize = glm::ivec3(windowChunks, 1, windowChunks);
            // Review of stage 7a: a player who steps back and forth across a chunk border does not
            // drop and fetch a slab (modified chunks come back as ChunkData) every time.
            base.recenterSlackChunks = kClientWindowSlackChunks;
        }
        base.reportLoads = true;
        base.keepUnloadedEdits = false;
        return base;
    }

    glm::ivec3 ClientWindowMinChunk(const RegionDesc& region, int32_t windowChunks, const glm::dvec3& focus) {
        const glm::ivec2 rows = RegionRows(region);
        if (!ClientWindowMoves(region, windowChunks)) {
            const glm::ivec3 corner = RegionWindowMinChunk(region);
            return glm::ivec3(corner.x, rows.x, corner.z);
        }
        // World::WorldToVoxel and World::WindowMinFor, the same arithmetic.
        const double inv = 1.0 / static_cast<double>(region.terrain.voxelSize);
        const glm::ivec3 chunk = voxel::VoxelToChunk(glm::ivec3(glm::floor(focus * inv)));
        const glm::ivec3 half = glm::ivec3(windowChunks, 1, windowChunks) / 2;
        return glm::ivec3(chunk.x - half.x, rows.x, chunk.z - half.z);
    }

    uint32_t ClientChunkCapacity(const RegionDesc& region, int32_t windowChunks) {
        if (!ClientWindowMoves(region, windowChunks)) return RegionChunkCapacity(region);
        const glm::ivec3 size = ClientWindowSize(region, windowChunks);
        const uint64_t cells = static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.y) * static_cast<uint64_t>(size.z);
        return static_cast<uint32_t>(std::clamp<uint64_t>(cells, 1024, kMaxRegionStoreSlots));
    }

    double PlaygroundScale(float voxelSize) {
        const double scale = 0.1 / static_cast<double>(voxelSize);
        return std::abs(scale - 1.0) < 1.0e-5 ? 1.0 : scale;
    }

    std::shared_ptr<voxel::VoxelModel> CrateModel(const voxel::WorldMaterials& m, double scale) {
        const int n = std::max(3, static_cast<int>(std::lround(8.0 * scale)));     // 0.8 m
        const int e = n - 1;
        voxel::VoxelVolume volume{glm::ivec3(n)};
        volume.FillBox(glm::ivec3(0), glm::ivec3(n), m.planks);
        volume.FillBox(glm::ivec3(1), glm::ivec3(e), 0);           // hollow
        for (int i = 0; i < n; ++i) {                              // iron edges
            volume.Set(i, 0, 0, m.iron); volume.Set(i, e, 0, m.iron); volume.Set(i, 0, e, m.iron); volume.Set(i, e, e, m.iron);
            volume.Set(0, i, 0, m.iron); volume.Set(e, i, 0, m.iron); volume.Set(0, i, e, m.iron); volume.Set(e, i, e, m.iron);
        }
        return std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, "crate");
    }

    RegionTemplate PlaygroundTemplate(voxel::World& world, const voxel::WorldMaterials& m, bool loadSite, const PlaygroundArea& area) {
        RegionTemplate t;
        const double scale = PlaygroundScale(world.VoxelSize());
        const auto S1 = [scale](int v) { return static_cast<int>(std::lround(static_cast<double>(v) * scale)); };
        const auto S = [&S1](const glm::ivec3& v) { return glm::ivec3(S1(v.x), S1(v.y), S1(v.z)); };
        auto groundAt = [&world](double x, double z, double& y) {
            const voxel::WorldRayHit hit = world.Raycast(glm::dvec3(x, 60.0, z), glm::vec3(0, -1, 0), 200.0f);
            y = hit.position.y;
            return hit.hit;
        };

        // Pick the candidate spot whose surroundings are highest above the sea and most level.
        glm::dvec3 site(0.0);
        double best = -1.0e9;
        t.sited = false;
        for (int gx = -3; gx <= 3; ++gx)
            for (int gz = -3; gz <= 3; ++gz) {
                const double x = gx * 8.0, z = gz * 8.0;
                if (x - kPlaygroundHalfMetres < area.min.x || x + kPlaygroundHalfMetres > area.max.x ||
                    z - kPlaygroundHalfMetres < area.min.y || z + kPlaygroundHalfMetres > area.max.y)
                    continue;                                        // the plaza would leave the area
                double lo = 1.0e9, hi = -1.0e9, y = 0.0;
                bool ok = true;
                for (int i = 0; i < 9 && ok; ++i) {
                    ok = groundAt(x + (i % 3 - 1) * 7.0, z + (i / 3 - 1) * 7.0, y);
                    lo = std::min(lo, y);
                    hi = std::max(hi, y);
                }
                if (!ok || lo < 1.0) continue;                       // under water or not loaded
                const double score = -(hi - lo) * 4.0 - std::abs(x) * 0.05 - std::abs(z) * 0.05;
                if (score > best) {
                    best = score;
                    site = glm::dvec3(x, (lo + hi) * 0.5, z);
                    t.sited = true;
                }
            }

        // Everything within the near radius of the site (caves included) before it is edited.
        if (loadSite) world.LoadBlocking(site, voxel::LoadScope::Surface);
        const glm::ivec3 c = world.WorldToVoxel(site);                // plaza centre, floor level
        t.anchor = c;
        // Offsets below are in voxels of 0.1 m, scaled to the world's voxels (S); a box keeps at
        // least one voxel per axis.
        auto box = [&t, &S, c](glm::ivec3 lo, glm::ivec3 hi, uint8_t id) {
            const glm::ivec3 a = S(lo);
            t.ops.push_back(WorldOp::Box(c + a, c + glm::max(S(hi), a + 1), id));
        };
        auto light = [&t, &world, &S, c](glm::ivec3 p, glm::vec3 color, float intensity, float radius) {
            t.lights.push_back({.position = world.VoxelCenter(c + S(p)), .color = color, .intensity = intensity, .radius = radius});
        };
        const int r = 80;                                             // plaza half size: 8 m

        box({-r, 0, -r}, {r, 90, r}, 0);                              // clear the air above
        box({-r, -30, -r}, {r, 0, r}, m.stone);                       // foundation
        box({-r, -1, -r}, {r, 0, r}, m.stoneDark);                    // paving
        for (int i = -r; i < r; i += 20) {                            // paving pattern
            box({i, -1, -r}, {i + 1, 0, r}, m.stone);
            box({-r, -1, i}, {r, 0, i + 1}, m.stone);
        }

        // Ruined brick walls with window openings: good targets.
        box({-60, 0, -62}, {-10, 34, -58}, m.brick);
        box({-48, 10, -62}, {-38, 24, -58}, 0);
        box({-28, 10, -62}, {-18, 24, -58}, 0);
        box({-62, 0, -60}, {-58, 26, -20}, m.brick);
        // World::CarveSphere's quantisation of the same float arguments.
        const glm::vec3 hole(c + S(glm::ivec3(-60, 30, -20)));
        t.ops.push_back(WorldOp::Carve(voxel::QuantizeSphere(glm::dvec3(hole), static_cast<double>(static_cast<float>(12.0 * scale))),
                                       voxel::QuantizePower(100.0f)));

        // Watch tower with a torch on each corner.
        box({30, 0, -60}, {58, 70, -32}, m.stone);
        box({33, 0, -57}, {55, 66, -35}, 0);
        box({41, 0, -35}, {47, 20, -32}, 0);                          // doorway
        box({28, 66, -62}, {60, 70, -30}, m.planks);
        for (int i = 0; i < 4; ++i) {
            const glm::ivec3 p(i & 1 ? 58 : 28, 70, i & 2 ? -32 : -62);
            box(p, p + glm::ivec3(2, 8, 2), m.wood);
            box(p + glm::ivec3(0, 8, 0), p + glm::ivec3(2, 10, 2), m.torch);
            light(p + glm::ivec3(1, 11, 1), glm::vec3(1.0f, 0.62f, 0.3f), 6.0f, 7.0f);
        }

        // Timber frame.
        for (int x : {-50, -20}) {
            box({x, 0, 30}, {x + 3, 40, 33}, m.wood);
            box({x, 0, 57}, {x + 3, 40, 60}, m.wood);
            box({x, 37, 30}, {x + 3, 40, 60}, m.wood);
        }
        box({-50, 40, 28}, {-17, 42, 62}, m.planks);

        // Crystal cluster and a lava pit: emissive voxels lighting their surroundings.
        for (int i = 0; i < 5; ++i) {
            const glm::ivec3 p(34 + i * 5, 0, 40 + (i * 7) % 11);
            box(p, p + glm::ivec3(3, 8 + i * 4, 3), m.crystal);
        }
        light({44, 16, 45}, glm::vec3(0.45f, 0.8f, 1.0f), 4.0f, 6.0f);
        box({-8, -6, -8}, {8, 0, 8}, 0);
        box({-8, -6, -8}, {8, -3, 8}, m.lava);
        light({0, 2, 0}, glm::vec3(1.0f, 0.4f, 0.12f), 5.0f, 7.0f);

        // Gold and iron blocks to compare materials.
        box({-6, 0, 30}, {2, 8, 38}, m.gold);
        box({6, 0, 30}, {14, 8, 38}, m.iron);

        // The player starts on the plaza's edge, looking across it; crates stacked in front.
        t.spawn = world.VoxelToWorld(c + S(glm::ivec3(-55, 2, 62)));
        const std::shared_ptr<voxel::VoxelModel> crate = CrateModel(m, scale);
        for (int i = 0; i < 6; ++i) {
            const glm::ivec3 p(-34 + (i % 3) * 9 + (i / 3) * 4, 1 + (i / 3) * 9, 6);
            t.objects.push_back({.model = crate, .position = world.VoxelToWorld(c + S(p))});
        }
        return t;
    }

    RegionTemplate BuildRegionTemplate(Simulation& simulation, const RegionDesc& region) {
        if (region.templateId == RegionTemplateId::Playground) {
            const double chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(simulation.World().VoxelSize());
            const PlaygroundArea area{glm::dvec2(region.playMinChunk) * chunkMetres, glm::dvec2(region.playMaxChunk) * chunkMetres};
            return PlaygroundTemplate(simulation.World(), simulation.WorldMaterialIds(), false, area);
        }
        RegionTemplate t;
        // Empty: spawn above the middle of the play bounds, on the ground if there is any.
        const glm::ivec2 mid = (region.playMinChunk + region.playMaxChunk) / 2;
        const double vs = static_cast<double>(simulation.World().VoxelSize());
        const glm::dvec2 xz = glm::dvec2(mid * voxel::kChunkSize) * vs + glm::dvec2(0.5 * vs);
        const double top = static_cast<double>(region.heightRange.y) - 1.0;
        const voxel::WorldRayHit hit = simulation.World().Raycast(glm::dvec3(xz.x, top, xz.y), glm::vec3(0, -1, 0),
                                                                  region.heightRange.y - region.heightRange.x);
        t.spawn = glm::dvec3(xz.x, hit.hit ? hit.position.y : top, xz.y);
        t.anchor = simulation.World().WorldToVoxel(t.spawn);
        return t;
    }

    uint32_t ApplyTemplateOps(Simulation& simulation, const RegionTemplate& t) {
        uint32_t changed = 0;
        for (size_t i = 0; i < t.ops.size(); ++i) {
            const OpResult r = ApplyOp(simulation.World(), t.ops[i]);
            changed += r.changed;
            if (!r.Complete() || r.report.outOfBounds) {
                Log::Warn("Template: operation {} of {} applied only in part ({} chunk(s) not loaded, {} without a store slot{})", i + 1,
                          t.ops.size(), r.report.skipped.size(), r.report.unsynced.size(),
                          r.report.outOfBounds ? ", refused above the build ceiling" : "");
            }
        }
        return changed;
    }

    std::vector<scene::Entity> CreateTemplateLights(Simulation& simulation, const RegionTemplate& t) {
        std::vector<scene::Entity> out;
        for (const TemplateLight& l : t.lights)
            out.push_back(simulation.Scene().CreateLight({.position = l.position, .color = l.color, .intensity = l.intensity, .radius = l.radius}));
        return out;
    }

    std::vector<scene::Entity> CreateTemplateObjects(Simulation& simulation, const RegionTemplate& t) {
        std::vector<scene::Entity> out;
        for (const TemplateObject& o : t.objects)
            out.push_back(simulation.Scene().CreateVoxelObject({.model = o.model, .position = glm::vec3(o.position)}));
        return out;
    }

    RegionTemplate MechanismYardTemplate(voxel::World& world, const voxel::WorldMaterials& m, const glm::ivec3& plazaAnchor) {
        RegionTemplate t;
        const float vs = world.VoxelSize();
        const double scale = PlaygroundScale(vs);
        const auto S1 = [scale](int v) { return static_cast<int>(std::lround(static_cast<double>(v) * scale)); };
        const auto S = [&S1](const glm::ivec3& v) { return glm::ivec3(S1(v.x), S1(v.y), S1(v.z)); };
        const auto V = [vs](double metres) { return std::max(1, static_cast<int>(std::lround(metres / static_cast<double>(vs)))); };
        const glm::ivec3 c = plazaAnchor;
        t.anchor = c;
        // Offsets in voxels of 0.1 m, scaled as PlaygroundTemplate scales them; the yard lies north (-z) of the plaza.
        auto box = [&t, &S, c](glm::ivec3 lo, glm::ivec3 hi, uint8_t id) {
            const glm::ivec3 a = S(lo);
            t.ops.push_back(WorldOp::Box(c + a, c + glm::max(S(hi), a + 1), id));
        };

        // Pad.
        box({-70, 0, -150}, {60, 90, -84}, 0);
        box({-70, -30, -150}, {60, 0, -84}, m.stone);
        box({-70, -1, -150}, {60, 0, -84}, m.stoneDark);
        // Hut, its roof and doorway.
        box({-60, 0, -140}, {-20, 30, -110}, m.brick);
        box({-57, 0, -137}, {-23, 30, -113}, 0);
        box({-62, 30, -142}, {-18, 32, -108}, m.planks);
        box({-45, 0, -113}, {-36, 21, -110}, 0);
        // Gate wall, its passage, the slot above it for the rising gate.
        box({-10, 0, -100}, {50, 40, -97}, m.brick);
        box({5, 0, -100}, {35, 30, -97}, 0);
        box({5, 30, -99}, {35, 40, -98}, 0);
        // Lever post.
        box({39, 0, -136}, {41, 10, -134}, m.wood);

        const auto model = [](const glm::ivec3& size, const char* name, auto solid) {
            voxel::VoxelVolume volume(size);
            for (int z = 0; z < size.z; ++z)
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x)
                        if (const uint8_t id = solid(x, y, z); id != 0) volume.Set(x, y, z, id);
            return std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, name);
        };

        // Door 0: planks with a wooden hinge column at x = 0 (a cast must be able to break it: iron, hardness 8,
        // never breaks to the cast power 6), hinged about +y, opening either way.
        const glm::ivec3 doorSize(V(0.9), V(2.1), 3);
        TemplateMechanism door;
        door.model = model(doorSize, "yard door", [&](int x, int, int) { return x == 0 ? m.wood : m.planks; });
        door.spawnVoxel = c + S(glm::ivec3(-45, 0, -113));
        door.desc.kind = MechanismKind::Door;
        door.desc.flags = scene::MechanismDesc_Usable;
        door.desc.pivotQ = {0, 0, 24};
        door.desc.axis = 2;
        door.desc.minPos = -1024;
        door.desc.maxPos = 1024;
        door.desc.openTicks = door.desc.closeTicks = 72;
        door.desc.useBox = door.desc.blockBox = {glm::ivec3(0), doorSize};
        door.desc.hingeBox = {glm::ivec3(0), glm::ivec3(1, doorSize.y, doorSize.z)};
        door.desc.pushAxis = 4;
        door.desc.anchorCount = 2;
        door.desc.anchors = {door.spawnVoxel + glm::ivec3(-1, 2, 1), door.spawnVoxel + glm::ivec3(-1, doorSize.y - 3, 1)};
        t.mechanisms.push_back(std::move(door));

        // Gate 1: a wooden lattice portcullis that slides up 2.5 m out of the passage.
        const glm::ivec3 gateSize(V(3.0), V(3.0), 1);
        TemplateMechanism gate;
        gate.model = model(gateSize, "yard portcullis",
                           [&](int x, int y, int) { return (x % 3 == 0 || y % 4 == 0 || y == 0) ? m.wood : uint8_t{0}; });
        gate.spawnVoxel = c + S(glm::ivec3(5, 0, -99));
        gate.desc.kind = MechanismKind::Portcullis;
        gate.desc.axis = 2;
        gate.desc.minPos = 0;
        gate.desc.maxPos = static_cast<int16_t>(V(2.5) * 16);
        gate.desc.openTicks = 180;
        gate.desc.closeTicks = 45;
        gate.desc.useBox = gate.desc.blockBox = {glm::ivec3(0), gateSize};
        gate.desc.hingeBox = {glm::ivec3(0, gateSize.y - 1, 0), gateSize};
        gate.desc.pushAxis = 4;
        t.mechanisms.push_back(std::move(gate));

        // Lever 2: on its post behind the wall, turning 45 deg about +x at its foot; it runs gate 1.
        const glm::ivec3 leverSize(2, 6, 2);
        TemplateMechanism lever;
        lever.model = model(leverSize, "yard lever", [&](int, int, int) { return m.wood; });
        lever.spawnVoxel = c + S(glm::ivec3(39, 10, -136));
        lever.desc.kind = MechanismKind::Lever;
        lever.desc.flags = scene::MechanismDesc_Usable;
        lever.desc.pivotQ = {16, 0, 16};
        lever.desc.axis = 0;
        lever.desc.minPos = 0;
        lever.desc.maxPos = 512;
        lever.desc.openTicks = lever.desc.closeTicks = 12;
        lever.desc.useBox = lever.desc.blockBox = {glm::ivec3(0), leverSize};
        lever.desc.hingeBox = {glm::ivec3(0), glm::ivec3(leverSize.x, 1, leverSize.z)};
        lever.desc.pushAxis = 4;
        lever.link = 1;
        t.mechanisms.push_back(std::move(lever));

        t.spawn = world.VoxelCenter(c + S(glm::ivec3(20, 0, -90)));
        return t;
    }

    namespace {
        glm::quat QuarterTurns(uint8_t turns) {
            constexpr double kPi = 3.14159265358979323846;
            return glm::angleAxis(static_cast<float>(turns * (kPi / 2.0)), glm::vec3(0.0f, 1.0f, 0.0f));
        }

        glm::vec3 AxisUnit(uint8_t axis) {
            switch (axis) {
                case 0: return {1.0f, 0.0f, 0.0f};
                case 1: return {-1.0f, 0.0f, 0.0f};
                case 2: return {0.0f, 1.0f, 0.0f};
                case 3: return {0.0f, -1.0f, 0.0f};
                case 4: return {0.0f, 0.0f, 1.0f};
                default: return {0.0f, 0.0f, -1.0f};
            }
        }

        /// A template mechanism as a timetable at rest at its initial position (no entity).
        Mechanism TemplateAtRest(const TemplateMechanism& tm) {
            Mechanism m;
            m.desc = tm.desc;
            m.spawnVoxel = tm.spawnVoxel;
            m.quarterTurns = tm.quarterTurns;
            PushSegment(m.track, {0, tm.initialPos, tm.initialPos, 0, 0});
            return m;
        }

        /// The 8 world corners of an original-model voxel box of `m` at `pose`.
        std::array<glm::dvec3, 8> PosedCorners(const MechanismPose& pose, const scene::MechanismBox& box, float voxelSize) {
            std::array<glm::dvec3, 8> out{};
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 local((c & 1) ? box.max.x : box.min.x, (c & 2) ? box.max.y : box.min.y, (c & 4) ? box.max.z : box.min.z);
                out[static_cast<size_t>(c)] = pose.position + glm::dvec3(pose.rotation * (local * voxelSize));
            }
            return out;
        }
    }

    std::vector<scene::Entity> CreateTemplateMechanisms(Simulation& simulation, const RegionTemplate& t) {
        std::string why;
        if (!CheckMechanismAuthoring(t, simulation.World().VoxelSize(), &why)) {
            Log::Error("Mechanisms: template refused: {}", why);
            return {};
        }
        std::vector<scene::Entity> made;
        made.reserve(t.mechanisms.size());
        for (const TemplateMechanism& tm : t.mechanisms) {
            const scene::Entity e = simulation.Scene().CreateVoxelObject({.model = tm.model,
                                                                         .position = glm::vec3(simulation.World().VoxelToWorld(tm.spawnVoxel)),
                                                                         .rotation = QuarterTurns(tm.quarterTurns),
                                                                         .motion = physics::MotionType::Kinematic});
            if (e != scene::kNoEntity &&
                !MakeMechanism(simulation, e, tm.desc, tm.spawnVoxel, tm.quarterTurns, tm.initialPos, simulation.Tick())) {
                simulation.Scene().Destroy(e);
                made.push_back(scene::kNoEntity);
                continue;
            }
            made.push_back(e);
        }
        for (size_t i = 0; i < t.mechanisms.size(); ++i) {
            const int32_t link = t.mechanisms[i].link;
            if (link < 0 || static_cast<size_t>(link) >= made.size() || made[i] == scene::kNoEntity) continue;
            if (Mechanism* m = simulation.Scene().TryGet<Mechanism>(made[i])) m->linked = made[static_cast<size_t>(link)];
        }
        return made;
    }

    bool CheckMechanismAuthoring(const RegionTemplate& t, float voxelSize, std::string* why) {
        const float required = kUseReach + 0.5f;
        for (size_t i = 0; i < t.mechanisms.size(); ++i) {
            const TemplateMechanism& lever = t.mechanisms[i];
            if (lever.desc.kind != MechanismKind::Lever || lever.link < 0) continue;
            if (static_cast<size_t>(lever.link) >= t.mechanisms.size()) {
                if (why) *why = std::format("lever {} links to a missing mechanism {}", i, lever.link);
                return false;
            }
            const TemplateMechanism& gate = t.mechanisms[static_cast<size_t>(lever.link)];
            const Mechanism g = TemplateAtRest(gate);
            const Mechanism l = TemplateAtRest(lever);
            const MechanismPose gPose = PoseAt(g, gate.initialPos, voxelSize);
            const MechanismPose lPose = PoseAt(l, lever.initialPos, voxelSize);

            glm::vec3 n = QuarterTurns(gate.quarterTurns) * AxisUnit(gate.desc.pushAxis);
            n.y = 0.0f;
            if (glm::length(n) < 0.5f) continue;   // a vertical push axis has no outer face to measure from
            n = glm::normalize(n);

            const std::array<glm::dvec3, 8> gCorners = PosedCorners(gPose, gate.desc.blockBox, voxelSize);
            glm::dvec3 cG(0.0);
            for (const glm::dvec3& c : gCorners) cG += c / 8.0;
            double hG = 0.0;
            for (const glm::dvec3& c : gCorners) hG = std::max(hG, std::abs(glm::dot(c - cG, glm::dvec3(n))));

            const std::array<glm::dvec3, 8> lCorners = PosedCorners(lPose, lever.desc.useBox, voxelSize);
            glm::dvec3 cL(0.0);
            for (const glm::dvec3& c : lCorners) cL += c / 8.0;
            const double side = glm::dot(cL - cG, glm::dvec3(n)) >= 0.0 ? 1.0 : -1.0;
            const glm::dvec3 plane = cG - glm::dvec3(n) * (side * hG);
            double nearest = std::numeric_limits<double>::max();
            for (const glm::dvec3& c : lCorners) nearest = std::min(nearest, std::abs(glm::dot(c - plane, glm::dvec3(n))));
            if (nearest < required) {
                if (why) *why = std::format("lever {} reachable through its gate ({:.1f} m < {:.1f} m)", i, nearest, required);
                return false;
            }
        }
        return true;
    }
}
