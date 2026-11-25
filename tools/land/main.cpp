// vortex_land: bakes the world map from the drawn map's files (engine/docs/design/land.md, [land-2]).
//
//   vortex_land --map DIR [--out DIR] [--twice]
//   vortex_land --version
//
// --map is the folder the game's tools/worldmap/worldmap.py writes (height.png, biome_ids.png, region_ids.png,
// world.json). The baked fields and the review pictures go to --out (default build/land): land_height.png,
// land_water.png, land_flow.png, land_relief.png, land_change.png (voxel::WriteLandFiles). --twice bakes a second
// time and compares the hashes.
//
// Log lines: `Land: baked W x H cells of S m in T s (...)`, `Land: N lake(s): ...`, `Land: N river(s), ...`,
// `Land: hash H`, then `Land: checks pass` or `Land: check failed: ...`.
// Exit code 0 when the checks pass, 1 on a missing or bad input, 2 on a bad command line, 3 when a check fails.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <string>

import vortex.core;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

int main(int argc, char** argv) {
    const CommandLine args(argc, argv);
    if (args.Has("--version")) {
        std::printf("vortex_land %s\n", DescribeBuild().c_str());
        return 0;
    }
    Log::Init();
    Log::Info("Build: {}", DescribeBuild());
    const std::optional<std::string> mapDir = args.Get("--map");
    if (!mapDir) {
        std::fprintf(stderr, "usage: vortex_land --map DIR [--out DIR] [--twice]\n");
        return 2;
    }
    const std::filesystem::path out = args.GetString("--out", "build/land");

    LandSource source;
    std::string error;
    if (!LoadLandSource(*mapDir, source, error)) {
        Log::Error("Land: {}", error);
        return 1;
    }
    Log::Info("Land: drawn map {} x {} px at {} m/px", source.width, source.height, source.metresPerPixel);

    const LandMap map = BakeLand(source);
    const LandTimings& t = map.seconds;
    Log::Info("Land: baked {} x {} cells of {} m in {:.2f} s (resample {:.2f}, stream power {:.2f}, slopes {:.2f}, "
              "droplets {:.2f}, lakes {:.2f}, rivers {:.2f})",
              map.width, map.height, map.cellSize, t.total, t.resample, t.streamPower, t.thermal, t.droplets, t.water,
              t.rivers);

    double cut = 0.0, raised = 0.0, maxCut = 0.0;
    uint64_t land = 0;
    for (size_t i = 0; i < map.heights.size(); ++i) {
        if (map.waterKind[i] != WaterKind::None) continue;
        const double d = double(map.heights[i]) - double(map.rawHeights[i]);
        ++land;
        if (d < 0) {
            cut -= d;
            maxCut = std::max(maxCut, -d);
        } else {
            raised += d;
        }
    }
    const double cellArea = double(map.cellSize) * double(map.cellSize);
    Log::Info("Land: dry land {:.2f} km^2; erosion cut {:.0f} m^3 (deepest {:.1f} m), laid down {:.0f} m^3", double(land) * cellArea / 1.0e6,
              cut * cellArea, maxCut, raised * cellArea);

    std::string lakes;
    for (const LandLake& lake : map.lakes) {
        lakes += std::format("{}{:.2f} m over {:.0f} m^2, {:.1f} m deep{}", lakes.empty() ? "" : "; ", lake.level,
                             double(lake.cells) * cellArea, lake.maxDepth, lake.drawn ? " (drawn)" : "");
    }
    Log::Info("Land: {} lake(s): {}; {} outlet cell(s) cut, {} puddle(s) filled", map.lakes.size(), lakes, map.breachedCells,
              map.puddlesFilled);

    size_t cells = 0, reaches = 0, falls = 0, toSea = 0, toLake = 0, joins = 0;
    float biggestStep = 0.0f;
    for (const LandRiver& river : map.rivers) {
        cells += river.cells.size();
        reaches += river.reaches.size();
        for (size_t k = 1; k < river.reaches.size(); ++k) {
            const float step = river.reaches[k - 1].level - river.reaches[k].level;
            biggestStep = std::max(biggestStep, step);
            if (step > 1.0f) ++falls;
        }
        (river.end == RiverEnd::Sea ? toSea : river.end == RiverEnd::Lake ? toLake : joins) += 1;
    }
    Log::Info("Land: {} river(s), {:.1f} km, {} reach(es), {} step(s) over 1 m (largest {:.1f} m); {} reach the sea, {} a "
              "lake, {} join another river; {} bank cell(s) raised",
              map.rivers.size(), double(cells) * map.cellSize / 1000.0, reaches, falls, biggestStep, toSea, toLake, joins,
              map.banksRaised);

    const uint64_t hash = LandHash(map);
    Log::Info("Land: hash {:016x}", hash);
    if (args.Has("--twice")) {
        const uint64_t again = LandHash(BakeLand(source));
        Log::Info("Land: second bake hash {:016x}: {}", again, again == hash ? "equal" : "DIFFERENT");
        if (again != hash) return 3;
    }

    if (!WriteLandFiles(map, out, error)) {
        Log::Error("Land: {}", error);
        return 1;
    }
    Log::Info("Land: wrote {}", out.string());

    const std::string broken = CheckLand(map);
    if (!broken.empty()) {
        Log::Error("Land: check failed: {}", broken);
        return 3;
    }
    Log::Info("Land: checks pass");
    return 0;
}
