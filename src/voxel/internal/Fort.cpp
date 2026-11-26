module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

module vortex.voxel;

// Hollowkeep's fort plan and stamper (engine/docs/design/fortress.md, [fort-1]). The plan turns the baked site into
// records; the stamper decides each voxel of a record from the record and its column's ground only, so a chunk's
// voxels never depend on the chunks around it.

namespace vortex::voxel {

    namespace {
        constexpr float kFoundation = 1.5f;      // every part reaches this far under its own column's ground
        constexpr float kWallBatter = 2.5f;      // a wall's batter ends this high over its column's ground
        constexpr float kBlockBatter = 3.0f;     // a tower's or the keep's
        constexpr float kBatterRun = 3.0f;       // 1 m out per 3 m up: 18.4 degrees
        constexpr float kBatterMax = 1.0f;       // below that the flare is buried
        constexpr float kParapet = 1.2f, kMerlon = 0.75f;
        constexpr float kCurtainWalk = 8.8f, kInnerWalk = 8.0f;
        constexpr float kClearOverGround = 14.0f;   // the ditch clears this high over the ground (trees, rocks)

        float Cross(glm::vec2 x, glm::vec2 y) { return x.x * y.y - x.y * y.x; }
        float Dot(glm::vec2 x, glm::vec2 y) { return x.x * y.x + x.y * y.y; }
        float Length(glm::vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

        /// Merlons 2 m wide with embrasures of 0.75 m, by the distance along the parapet.
        bool Merlon(float along) { return along - 2.75f * std::floor(along / 2.75f) < 2.0f; }

        /// How far a battered face stands out at height y under the batter's top.
        float Batter(float y, float top) { return y < top ? std::min(kBatterMax, (top - y) / kBatterRun) : 0.0f; }

        /// Quoins: alternate courses of 0.75 x 0.375 m and 0.375 x 0.75 m blocks at a square corner; du and dv are the
        /// distances in from the corner's two faces.
        bool Quoin(float du, float dv, float course) {
            const bool even = int(std::floor(course / 0.375f)) % 2 == 0;
            return even ? (du < 0.75f && dv < 0.375f) : (du < 0.375f && dv < 0.75f);
        }

        /// The 12 directions of a spiral stair's steps, 30 degrees apart, as literals (no libm function decides a voxel).
        constexpr float kTurn[12][2] = {{1.0f, 0.0f},   {0.8660254f, 0.5f},   {0.5f, 0.8660254f},   {0.0f, 1.0f},
                                        {-0.5f, 0.8660254f}, {-0.8660254f, 0.5f}, {-1.0f, 0.0f},  {-0.8660254f, -0.5f},
                                        {-0.5f, -0.8660254f}, {0.0f, -1.0f},  {0.5f, -0.8660254f}, {0.8660254f, -0.5f}};

        /// The sector (0..11) of q around a spiral stair's middle.
        int Sector(glm::vec2 q) {
            for (int k = 0; k < 12; ++k) {
                const glm::vec2 a(kTurn[k][0], kTurn[k][1]), b(kTurn[(k + 1) % 12][0], kTurn[(k + 1) % 12][1]);
                if (Cross(a, q) >= 0.0f && Cross(q, b) > 0.0f) return k;
            }
            return 0;
        }

        /// A spiral stair (fortress.md "Towers"): in a sector whose lowest step tops `base` (0.25 m per sector, 3 m a turn),
        /// a voxel at `rel` over the stair's foot is step when it lies within 0.5 m under the next step's top, so the
        /// steps overlap and carry each other.
        bool Step(float rel, float base) {
            const float top = std::floor((rel - base) / 3.0f + 1.0f) * 3.0f + base;
            return rel >= top - 0.5f;
        }

        /// A splayed arrow slit through a shell, by the depth into the shell (0 at the outer face): 0.14 m wide outside,
        /// 1 m wide at the inner face.
        bool Slit(float lateral, float depthIn, float shell) { return lateral < 0.07f + 0.43f * std::clamp(depthIn / shell, 0.0f, 1.0f); }

        /// The ground-plane direction away from the game's sun (0.45, 0.7, 0.35): faces turned this way take damp.
        constexpr float kShadeX = -0.78935f, kShadeZ = -0.61394f;

        /// A number in [0, 1) from 2 integers (ragged edges: repair patches, unfinished tops).
        float Ragged(int a, int b) {
            uint32_t h = uint32_t(a) * 0x9e3779b1u ^ (uint32_t(b) + 0x7f4a7c15u) * 0x85ebca77u;
            h ^= h >> 15;
            h *= 0x2c1b3c6du;
            h ^= h >> 12;
            return float(h >> 8) * (1.0f / 16777216.0f);
        }

        /// Damp on a battered base: the outer 0.25 m of a face turned from the sun, up to 1-1.5 m over the ground.
        bool Damp(float outward, float depthIn, float aboveGround, int along) {
            return outward > 0.25f && depthIn < 0.25f && aboveGround < 1.5f - 0.5f * Ragged(along, 7);
        }

        /// A prop's size (long, wide, high; metres) as sim::HollowkeepTemplate builds it.
        glm::vec3 PropSize(FortProp kind) {
            switch (kind) {
            case FortProp::Plank: return {2.0f, 0.25f, 0.125f};
            case FortProp::Beam: return {2.5f, 0.25f, 0.25f};
            case FortProp::Ladder: return {3.0f, 0.5f, 0.125f};
            case FortProp::Crate: return {0.75f, 0.75f, 0.75f};
            case FortProp::Barrel: return {0.625f, 0.625f, 0.875f};
            case FortProp::Cart: return {2.0f, 1.25f, 0.875f};
            case FortProp::Table: return {2.5f, 0.875f, 0.8f};
            case FortProp::HayWagon: return {2.0f, 1.25f, 1.75f};
            }
            return glm::vec3(1.0f);
        }

        uint64_t Mix(uint64_t x) {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ull;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebull;
            return x ^ (x >> 31);
        }
    }

    FortPlan PlanFort(const LandMap& map, const LandGeneratorSettings& land) {
        FortPlan plan;
        const LandSite& site = map.site;
        if (!site.placed) return plan;
        plan.placed = true;
        plan.site = site;
        plan.origin = land.origin;
        const WorldMaterials& m = land.materials;
        const float vs = land.voxelSize;
        const auto V = [vs](float metres) { return int(std::floor(metres / vs)); };
        const glm::vec2 keep = land.origin + glm::vec2(site.x, site.z);
        const auto ground = [&](glm::vec2 p) { return LandHeight(map, land, p.x, p.y); };
        const auto vertex = [&](const std::array<float, kSiteDirections>& radii, int k) {
            const int i = ((k % kSiteDirections) + kSiteDirections) % kSiteDirections;
            return keep + LandSiteDirection(i) * radii[size_t(i)];
        };
        plan.keep = keep;
        plan.axis = LandSiteGateAxis(site);
        plan.across = glm::vec2(-plan.axis.y, plan.axis.x);
        plan.gatePoint = vertex(site.curtain, site.gate);
        plan.gateGround = ground(plan.gatePoint);
        plan.innerGate = vertex(site.inner, site.gate);
        plan.keepGround = site.keepGround;
        plan.bridgeSpan = LandSiteBridgeSpan(site);
        const glm::vec2 axis = plan.axis, across = plan.across, gatePoint = plan.gatePoint;
        const float gateGround = plan.gateGround;

        const auto setBounds = [&](FortRecord& r, glm::vec2 lo, glm::vec2 hi, float y0, float y1) {
            r.min = glm::ivec3(V(lo.x), V(y0), V(lo.y));
            r.max = glm::max(glm::ivec3(V(hi.x) + 1, V(y1) + 1, V(hi.y) + 1), r.min + 1);
        };
        const auto box = [&](glm::vec3 lo, glm::vec3 hi, uint8_t id) {
            FortRecord r;
            r.shape = FortShape::Box;
            r.id = id;
            r.a = glm::vec2(lo.x, lo.z);
            r.b = glm::vec2(hi.x, hi.z);
            r.lo = lo.y;
            r.hi = hi.y;
            r.min = glm::ivec3(V(lo.x), V(lo.y), V(lo.z));
            r.max = glm::max(glm::ivec3(V(hi.x), V(hi.y), V(hi.z)), r.min + 1);
            plan.records.push_back(r);
        };
        const auto column = [&](glm::vec2 p, float half, float bottom, float top, uint8_t id) {
            box(glm::vec3(p.x - half, bottom, p.y - half), glm::vec3(p.x + half, top, p.y + half), id);
        };
        // The lowest and highest ground in a rectangle, sampled every metre.
        const auto groundRange = [&](glm::vec2 lo, glm::vec2 hi, float& gLo, float& gHi) {
            gLo = 1.0e30f;
            gHi = -1.0e30f;
            for (float z = lo.y; z <= hi.y + 0.25f; z += 0.5f)
                for (float x = lo.x; x <= hi.x + 0.25f; x += 0.5f) {
                    const float g = ground(glm::vec2(std::min(x, hi.x), std::min(z, hi.y)));
                    gLo = std::min(gLo, g);
                    gHi = std::max(gHi, g);
                }
        };

        // A pitched roof record (fortress.md "Roofs"): its bounds cover the eaves' overhang and the apex.
        const auto roof = [&](FortRoof kind, FortBlock footprint, glm::vec2 c, glm::vec2 dir, float half, float length, float eave, float pitch, uint8_t cover) {
            FortRecord r;
            r.shape = FortShape::Roof;
            r.roof = kind;
            r.block = footprint;
            r.a = c;
            r.b = dir;
            r.half = half;
            r.length = length;
            r.lo = eave;
            r.height = pitch;
            r.id = cover;
            const float extent = kind == FortRoof::Gable ? std::max(half, length + 0.25f) : half;
            const float reach = extent * (std::abs(dir.x) + std::abs(dir.y)) + 0.5f + vs;
            setBounds(r, c - reach, c + reach, eave - 0.5f, eave + pitch * half + vs);
            plan.records.push_back(r);
        };

        // 1. The ditch across the neck (land.md "Hollowkeep's greybox").
        {
            FortRecord r;
            r.shape = FortShape::Trench;
            r.lo = gateGround - 5.0f;
            glm::vec2 lo(1.0e9f), hi(-1.0e9f);
            for (int k = -2; k <= 2; ++k) {
                const glm::vec2 v = vertex(site.curtain, site.gate + k), out = LandSiteDirection(site.gate + k);
                lo = glm::min(lo, glm::min(v, v + out * 11.0f));
                hi = glm::max(hi, glm::max(v, v + out * 11.0f));
            }
            lo = glm::floor(lo - 2.0f);
            hi = glm::ceil(hi + 2.0f);
            float gLo = 0.0f, gHi = 0.0f;
            groundRange(lo, hi, gLo, gHi);
            setBounds(r, lo, hi, r.lo - 0.5f, gHi + kClearOverGround + 1.0f);
            plan.records.push_back(r);
        }

        // 2. The bridge: level with the gate from 3 m out to 1 m past the ditch, open to the sky, on two trestles.
        {
            const float span = plan.bridgeSpan;
            const glm::vec2 width = glm::abs(across) * 2.0f;
            const glm::vec2 d0 = glm::min(gatePoint + axis * 3.0f, gatePoint + axis * span) - width;
            const glm::vec2 d1 = glm::max(gatePoint + axis * 3.0f, gatePoint + axis * span) + width;
            box(glm::vec3(d0.x, gateGround - 0.3f, d0.y), glm::vec3(d1.x, gateGround, d1.y), m.planks);
            const auto groundTop = [&](glm::vec2 c) {
                float top = ground(c);
                for (const float side : {-2.0f, -1.0f, 1.0f, 2.0f}) top = std::max(top, ground(c + across * side));
                return top + 1.0f;
            };
            for (float t = 3.0f; t < span; t += 0.5f) {
                const glm::vec2 a = gatePoint + axis * t, b = gatePoint + axis * std::min(span, t + 0.5f);
                const glm::vec2 c0 = glm::min(a, b) - width, c1 = glm::max(a, b) + width;
                const float top = std::max(gateGround + 3.0f, groundTop((a + b) * 0.5f));
                box(glm::vec3(c0.x, gateGround, c0.y), glm::vec3(c1.x, top, c1.y), 0);
            }
            const float floor = gateGround - 5.0f;
            for (const float t : {0.4f, 0.7f}) {
                const glm::vec2 c = gatePoint + axis * (3.0f + (span - 3.0f) * t);
                for (const float side : {-1.6f, 1.6f}) column(c + across * side, 0.25f, floor - 0.5f, gateGround - 0.3f, m.wood);
                column(c, 0.25f, gateGround - 1.3f, gateGround - 0.3f, m.wood);
            }
        }

        // 3. The curtain and the inner wall: one Wall record per run between two site directions, walks stepping with
        // each run's highest ground, gaps at the gatehouse and the inner gate.
        // Every run's walk is at least its own highest ground plus the walk height; where two runs meet, both take the
        // higher of their walks, so the walk is continuous: level for 4 m beside a tower, a ramp between.
        const auto towerAt = [&](int k) {
            const int i = ((k % kSiteDirections) + kSiteDirections) % kSiteDirections;
            const int dk = std::abs(((i - site.gate) % kSiteDirections + kSiteDirections + 12) % kSiteDirections - 12);
            return i % 3 == 0 && dk > 1;
        };
        std::array<int, kSiteDirections> curtainRecord;   // the record of the curtain's run from direction k to k + 1
        curtainRecord.fill(-1);
        const auto walls = [&](const std::array<float, kSiteDirections>& radii, float half, float walk, glm::vec2 gapAt, float gap) {
            const bool curtain = &radii == &site.curtain;
            std::array<float, kSiteDirections> own;
            for (int k = 0; k < kSiteDirections; ++k) {
                const glm::vec2 a = vertex(radii, k), b = vertex(radii, k + 1);
                const float len = Length(b - a);
                float runTop = -1.0e30f;
                for (float t = 0.0f; t <= len + 0.25f; t += 0.5f) runTop = std::max(runTop, ground(a + (b - a) * (std::min(t, len) / std::max(len, 0.01f))));
                own[size_t(k)] = runTop + walk;
            }
            // Each run's level ends (4 m beside a tower, less on a short run) and the ramp between them.
            std::array<float, kSiteDirections> flatA{}, flatB{}, ramp{};
            for (int k = 0; k < kSiteDirections; ++k) {
                const float len = Length(vertex(radii, k + 1) - vertex(radii, k));
                // Level 4.5 m beside a tower (its door stands on the level part) and 1.5 m at other ends (a neighbour's
                // end overlaps that far, at the same height); on a short run the other ends give way first, and with
                // no room left to ramp the run is level end to end.
                const bool towerA = curtain && towerAt(k), towerB = curtain && towerAt(k + 1);
                float fa = towerA ? 4.5f : 1.5f, fb = towerB ? 4.5f : 1.5f;
                const float excess = fa + fb - len;
                if (excess > 0.0f) {
                    const float soft = (towerA ? 0.0f : fa) + (towerB ? 0.0f : fb);
                    const float cut = soft > 0.0f ? std::min(1.0f, excess / soft) : 0.0f;
                    if (!towerA) fa -= fa * cut;
                    if (!towerB) fb -= fb * cut;
                }
                flatA[size_t(k)] = fa;
                flatB[size_t(k)] = fb;
                ramp[size_t(k)] = std::max(0.0f, len - fa - fb);
            }
            // The walk's height at each direction: at least both runs' own, then raised until no ramp climbs more than
            // 1 in 1 (voxel steps of 0.125 m; walls only grow).
            std::array<float, kSiteDirections> junction{};
            for (int k = 0; k < kSiteDirections; ++k)
                junction[size_t(k)] = std::max(own[size_t((k + kSiteDirections - 1) % kSiteDirections)], own[size_t(k)]);
            for (int pass = 0; pass < 2 * kSiteDirections; ++pass)
                for (int k = 0; k < kSiteDirections; ++k) {
                    const size_t prev = size_t((k + kSiteDirections - 1) % kSiteDirections), next = size_t((k + 1) % kSiteDirections);
                    junction[size_t(k)] = std::max({junction[size_t(k)], junction[next] - ramp[size_t(k)], junction[prev] - ramp[prev]});
                }
            for (int k = 0; k < kSiteDirections; ++k) {
                const glm::vec2 a = vertex(radii, k), b = vertex(radii, k + 1);
                const float len = Length(b - a);
                if (len < 0.01f) continue;
                const glm::vec2 d = (b - a) / len;
                const glm::vec2 n(d.y, -d.x);
                float runLow = 1.0e30f;
                // The lowest ground under everything the run writes: its ends past the corners, its batter and buttresses.
                for (float t = -half; t <= len + half + 0.25f; t += 0.5f) {
                    const glm::vec2 p = a + d * std::min(t, len + half);
                    for (const float s : {-half - 0.5f, 0.0f, half + 1.0f, half + kBatterMax + 1.25f}) runLow = std::min(runLow, ground(p + n * s));
                }
                FortRecord r;
                r.shape = FortShape::Wall;
                r.a = a;
                r.b = b;
                r.half = half;
                r.lo = junction[size_t(k)];
                r.hi = junction[size_t((k + 1) % kSiteDirections)];
                r.flatA = flatA[size_t(k)];
                r.flatB = flatB[size_t(k)];
                if (curtain) {
                    curtainRecord[size_t(k)] = int(plan.records.size());
                }
                const float off = Cross(d, gapAt - a), along = Dot(gapAt - a, d);
                if (std::abs(off) < gap) {
                    const float h = std::sqrt(gap * gap - off * off);
                    r.gapLo = along - h;
                    r.gapHi = along + h;
                }
                const float reach = half + kBatterMax + 1.25f + vs;
                setBounds(r, glm::min(a, b) - reach, glm::max(a, b) + reach, runLow - kFoundation - 1.0f,
                          std::max(r.lo, r.hi) + kParapet + kMerlon + vs);
                plan.records.push_back(r);
            }
        };
        walls(site.curtain, 1.25f, kCurtainWalk, gatePoint, 7.5f);
        walls(site.inner, 1.0f, kInnerWalk, plan.innerGate, 1.6f);
        // Hoardings on the 4 curtain runs by the gate (fortress.md "Walls"): their bounds rise to the gallery's roof.
        for (int k = site.gate - 2; k <= site.gate + 1; ++k) {
            const int i = curtainRecord[size_t(((k % kSiteDirections) + kSiteDirections) % kSiteDirections)];
            if (i < 0) continue;
            FortRecord& r = plan.records[size_t(i)];
            r.trim = FortTrim_Hoarding;
            r.max.y = std::max(r.max.y, V(std::max(r.lo, r.hi) + 3.0f) + 1);
        }
        // The high route H1 (fortress.md "The 3 bypass loops"): a curtain run with no tower at either end, 5-8
        // directions from the gate, where the outside ground stands highest (the shortest climb); a brick repair patch in
        // its outer face and a gap in its parapet, construction scaffolding outside up to the walk.
        int highRun = -1;
        float highLo = 0.0f, highHi = 0.0f;
        {
            float best = -1.0e30f;
            for (int k = 0; k < kSiteDirections; ++k) {
                const int dk = std::abs(((k - site.gate) % kSiteDirections + kSiteDirections + 12) % kSiteDirections - 12);
                if (dk < 5 || dk > 8 || towerAt(k) || towerAt(k + 1) || curtainRecord[size_t(k)] < 0) continue;
                const FortRecord& r = plan.records[size_t(curtainRecord[size_t(k)])];
                const float len = Length(r.b - r.a);
                const float lo = 1.5f, hi = len - 1.5f;
                if (hi - lo < 5.0f) continue;   // 2 flights of 2 m and a deck between
                const glm::vec2 d = (r.b - r.a) / len;
                glm::vec2 n(d.y, -d.x);
                if (Dot(n, (r.a + r.b) * 0.5f - keep) < 0.0f) n = -n;
                const float outside = ground(r.a + d * ((lo + hi) * 0.5f) + n * 3.0f);
                if (outside > best) {
                    best = outside;
                    highRun = k;
                    highLo = lo;
                    highHi = hi;
                }
            }
            if (highRun >= 0) {
                FortRecord& r = plan.records[size_t(curtainRecord[size_t(highRun)])];
                r.patchLo = highLo;
                r.patchHi = highHi;
            }
        }
        // The unfinished tower: on the side away from the high route, 6-9 directions from the gate.
        int unfinishedTower = -1;
        {
            const int highSide = highRun >= 0 ? ((highRun - site.gate + kSiteDirections * 2) % kSiteDirections < 12 ? 1 : -1) : 1;
            for (int k = 0; k < kSiteDirections && unfinishedTower < 0; k += 3) {
                const int dk = std::abs(((k - site.gate) % kSiteDirections + kSiteDirections + 12) % kSiteDirections - 12);
                const int sideOf = (k - site.gate + kSiteDirections * 2) % kSiteDirections < 12 ? 1 : -1;
                // Its scaffold and the stacks in front of it need 8.5 m behind the curtain, and the inner wall beyond.
                if (towerAt(k) && dk >= 6 && dk <= 9 && sideOf != highSide && site.curtain[size_t(k)] - site.inner[size_t(k)] >= 11.5f) unfinishedTower = k;
            }
        }

        // 4. Blocks: the flank towers, the gate towers, the keep.
        const auto block = [&](FortBlock kind, glm::vec2 c, glm::vec2 out, float half, float floor, float top) {
            FortRecord r;
            r.shape = FortShape::Block;
            r.block = kind;
            r.a = c;
            r.b = out;
            r.half = half;
            r.lo = floor;
            r.hi = top;
            // A square corner turned 45 degrees lies (half + batter) x sqrt(2) along an axis.
            const float reach = (half + kBatterMax + 0.25f) * (std::abs(out.x) + std::abs(out.y)) + vs;
            float gLo = 0.0f, gHi = 0.0f;
            groundRange(c - reach, c + reach, gLo, gHi);
            const float crown = kind == FortBlock::Keep ? 2.5f + kMerlon : kParapet + kMerlon;
            setBounds(r, c - reach, c + reach, std::min(gLo, floor - 0.5f) - kFoundation - 1.0f, top + crown + vs);
            plan.records.push_back(r);
        };
        for (int k = 0; k < kSiteDirections; k += 3) {
            const int dk = std::abs(((k - site.gate) % kSiteDirections + kSiteDirections + 12) % kSiteDirections - 12);
            if (dk <= 1) continue;
            const glm::vec2 c = vertex(site.curtain, k), out = LandSiteDirection(k);
            float gLo = 0.0f, gHi = 0.0f;
            groundRange(c - 3.5f, c + 3.5f, gLo, gHi);
            // Doors onto the wall walks: the run to direction k + 1 lies on the tower's left (+side), the run from k - 1
            // on its right; each door on its walk's centre line (0.25 m in from the wall's middle) where it meets the
            // tower's side, at the walk's own height there (a walk may ramp near a short run's end).
            const glm::vec2 side(-out.y, out.x);
            const auto doorAt = [&](glm::vec2 other, float v, float& u) {
                const glm::vec2 dl = (other - c) / Length(other - c);
                glm::vec2 inward(-dl.y, dl.x);
                if (Dot(inward, keep - c) < 0.0f) inward = -inward;
                const float crossing = Dot(dl, side);
                if (std::abs(crossing) < 0.1f) {
                    u = 0.0f;
                    return 0.0f;
                }
                const float run = (v - 0.25f * Dot(inward, side)) / crossing;
                u = run * Dot(dl, out) + 0.25f * Dot(inward, out);
                return run;
            };
            const int left = curtainRecord[size_t(k)], right = curtainRecord[size_t((k + kSiteDirections - 1) % kSiteDirections)];
            float leftU = 0.0f, rightU = 0.0f, leftWalk = -1.0e30f, rightWalk = -1.0e30f;
            const float tl = doorAt(vertex(site.curtain, k + 1), 3.5f, leftU);
            const float tr = doorAt(vertex(site.curtain, k - 1), -3.5f, rightU);
            if (left >= 0) leftWalk = FortWalk(plan.records[size_t(left)], tl);
            if (right >= 0) {
                const FortRecord& run = plan.records[size_t(right)];
                rightWalk = FortWalk(run, Length(run.b - run.a) - tr);
            }
            // The tower stands at least 6 m over its walks; its floor is the bailey's ground outside its back door (the
            // site may slope up behind it).
            block(FortBlock::Tower, c, out, 3.5f, ground(c - out * 4.75f), std::max(gHi + 14.0f, std::max(leftWalk, rightWalk) + 6.0f));
            FortRecord& tower = plan.records.back();
            tower.walkLeft = leftWalk;
            tower.walkRight = rightWalk;
            tower.walkLeftU = leftU;
            tower.walkRightU = rightU;
            // A slate roof over the battlements: the tower's own D shape, 2 in 1, its eaves 0.25 m out; the unfinished
            // tower has none.
            if (k == unfinishedTower) {
                tower.trim |= FortTrim_Unfinished;
            } else {
                const float towerTop = tower.hi;
                roof(FortRoof::Cone, FortBlock::Tower, c, out, 3.75f, 0.0f, towerTop + kParapet + kMerlon, 2.0f, m.slate);
            }
        }
        for (const float side : {-1.0f, 1.0f}) {
            const glm::vec2 c = gatePoint + across * (10.0f * side) + axis * 1.0f;
            float gLo = 0.0f, gHi = 0.0f;
            groundRange(c - 3.0f, c + 3.0f, gLo, gHi);
            block(FortBlock::GateTower, c, axis, 3.0f, gateGround, std::max(gHi, gateGround) + 15.0f);
            plan.records.back().trim = FortTrim_Machicolations;
            const float top = plan.records.back().hi;
            roof(FortRoof::Cone, FortBlock::GateTower, c, axis, 4.0f, 0.0f, top + kParapet + kMerlon, 2.0f, m.slate);
        }

        // 5. The gatehouse on its pad: a battered granite base, ashlar above, the passage, the timber upper room (N3),
        // the portcullis slot, a crenellated top.
        {
            const glm::vec2 ext = glm::abs(across) * 7.0f + glm::abs(axis) * 4.0f;
            const glm::vec2 lo = gatePoint - ext, hi = gatePoint + ext;
            for (int step = 0; step < 4; ++step) {
                const float flare = 1.0f - 0.25f * float(step);
                const float bottom = step == 0 ? gateGround - kFoundation : gateGround + 0.75f * float(step);
                box(glm::vec3(lo.x - flare, bottom, lo.y - flare), glm::vec3(hi.x + flare, gateGround + 0.75f * float(step + 1), hi.y + flare), m.granite);
            }
            box(glm::vec3(lo.x, gateGround, lo.y), glm::vec3(hi.x, gateGround + 4.0f, hi.y), m.granite);
            box(glm::vec3(lo.x - 0.125f, gateGround + 4.0f, lo.y - 0.125f), glm::vec3(hi.x + 0.125f, gateGround + 4.25f, hi.y + 0.125f), m.granite);
            box(glm::vec3(lo.x, gateGround + 4.25f, lo.y), glm::vec3(hi.x, gateGround + 14.0f, hi.y), m.ashlar);
            // Machicolations on all 4 sides (fortress.md "Walls"): corbels of 3 stepped stones every 1.5 m under a
            // parapet that stands 0.25-0.75 m out from the faces; between the corbels the gallery's floor is open (drop
            // holes).
            const float t0 = gateGround + 14.0f;
            for (int side = 0; side < 4; ++side) {
                const bool alongXEdge = side < 2;
                const float sign = (side % 2 == 0) ? -1.0f : 1.0f;
                const float face = alongXEdge ? (side == 0 ? lo.y : hi.y) : (side == 2 ? lo.x : hi.x);
                const float from = (alongXEdge ? lo.x : lo.y) - 0.75f, to = (alongXEdge ? hi.x : hi.y) + 0.75f;
                // A box from `out0` to `out1` metres out of the face, from a0 to a1 along it.
                const auto edge = [&](float a0, float a1, float out0, float out1, float y0, float y1) {
                    const float f0 = face + sign * out0, f1 = face + sign * out1;
                    const float n0 = std::min(f0, f1), n1 = std::max(f0, f1);
                    if (alongXEdge)
                        box(glm::vec3(a0, y0, n0), glm::vec3(a1, y1, n1), m.ashlar);
                    else
                        box(glm::vec3(n0, y0, a0), glm::vec3(n1, y1, a1), m.ashlar);
                };
                for (float a = from + 0.75f; a + 0.75f <= to - 0.75f; a += 1.5f) {
                    edge(a, a + 0.75f, 0.0f, 0.25f, t0 - 1.125f, t0 - 0.75f);
                    edge(a, a + 0.75f, 0.0f, 0.5f, t0 - 0.75f, t0 - 0.375f);
                    edge(a, a + 0.75f, 0.0f, 0.75f, t0 - 0.375f, t0);
                }
                edge(from, to, 0.25f, 0.75f, t0, t0 + kParapet);
                for (float a = from; a < to; a += 2.75f) edge(a, std::min(to, a + 2.0f), 0.25f, 0.75f, t0 + kParapet, t0 + kParapet + kMerlon);
            }
            // A tiled gable roof across the passage, its ridge along the gatehouse's 14 m (a 1 m walk all round under
            // the parapet), stone gable ends; a bell turret on the ridge's middle.
            roof(FortRoof::Gable, FortBlock::Tower, gatePoint, glm::abs(across), 3.0f, 6.0f, t0, 1.0f, m.roofTile);
            {
                const float ridge = t0 + 3.0f;
                for (const float du : {-0.75f, 0.75f})
                    for (const float dv : {-0.75f, 0.75f}) column(gatePoint + glm::abs(across) * du + glm::abs(axis) * dv, 0.125f, ridge - 1.0f, ridge + 2.0f, m.wood);
                column(gatePoint, 1.0f, ridge - 0.25f, ridge, m.planks);
                column(gatePoint, 0.35f, ridge + 0.9f, ridge + 1.6f, m.iron);   // the bell
                column(gatePoint, 0.125f, ridge + 1.6f, ridge + 2.0f, m.wood);  // its stock
                roof(FortRoof::Cone, FortBlock::Keep, gatePoint, glm::abs(axis), 1.25f, 0.0f, ridge + 2.0f, 1.5f, m.roofTile);
            }
            // The passage: walls to 2.5 m, a barrel vault to 4.5 m, a straight groove where the portcullis runs, 3 murder
            // holes from the upper room through the vault's crown.
            const glm::vec2 pass = glm::abs(across) * 2.0f + glm::abs(axis) * 5.0f;
            box(glm::vec3(gatePoint.x - pass.x, gateGround, gatePoint.y - pass.y), glm::vec3(gatePoint.x + pass.x, gateGround + 2.5f, gatePoint.y + pass.y), 0);
            {
                FortRecord v;
                v.shape = FortShape::Vault;
                v.id = std::abs(axis.x) > 0.5f ? 0 : 1;
                v.a = gatePoint - pass;
                v.b = gatePoint + pass;
                v.lo = gateGround + 2.5f;
                v.half = 2.0f;
                setBounds(v, v.a, v.b, v.lo, v.lo + v.half);
                plan.records.push_back(v);
            }
            const glm::vec2 groove = gatePoint + axis * 2.5f;
            const glm::vec2 gext = glm::abs(across) * 2.25f + glm::abs(axis) * 0.3f;
            box(glm::vec3(groove.x - gext.x, gateGround, groove.y - gext.y), glm::vec3(groove.x + gext.x, gateGround + 4.5f, groove.y + gext.y), 0);
            const glm::vec2 room = glm::abs(across) * 5.0f + glm::abs(axis) * 3.0f;
            box(glm::vec3(gatePoint.x - room.x, gateGround + 5.0f, gatePoint.y - room.y), glm::vec3(gatePoint.x + room.x, gateGround + 13.0f, gatePoint.y + room.y), 0);
            box(glm::vec3(gatePoint.x - room.x, gateGround + 4.5f, gatePoint.y - room.y), glm::vec3(gatePoint.x + room.x, gateGround + 5.0f, gatePoint.y + room.y),
                m.planks);
            const glm::vec2 slot = gatePoint + axis * 2.5f;
            const glm::vec2 sext = glm::abs(across) * 2.25f + glm::abs(axis) * 0.2f;
            box(glm::vec3(slot.x - sext.x, gateGround + 4.5f, slot.y - sext.y), glm::vec3(slot.x + sext.x, gateGround + 8.5f, slot.y + sext.y), 0);
            // The murder holes, after the upper room's floor.
            for (const float at : {-2.5f, -1.0f, 0.5f}) column(gatePoint + axis * at, 0.25f, gateGround + 4.0f, gateGround + 5.0f, 0);
        }

        // 6. The low route (fortress.md L1): a culvert through a curtain run with no tower at either end, at least 5
        // directions from the gate, preferring runs with 9 m or more to the inner wall and the most ground over its room.
        // Its iron grate in a granite frame opens 2 m over the outside ground, beyond the batter and with no buttress
        // near it; a 1 m crawl rises 0.5 m through the wall to an iron-bound oak bulkhead and a 4 x 3 m room under the
        // bailey; a spiral stair in the room's middle climbs to the bailey's ground.
        {
            const auto frame = [&](int k, glm::vec2& d, glm::vec2& n, glm::vec2& mid, float& len) {
                const glm::vec2 a = vertex(site.curtain, k), b = vertex(site.curtain, k + 1);
                len = Length(b - a);
                d = (b - a) / len;
                mid = (a + b) * 0.5f;
                n = glm::vec2(d.y, -d.x);
                if (Dot(n, mid - keep) < 0.0f) n = -n;
            };
            int best = -1;
            float bestScore = -1.0e30f;
            for (int k = 0; k < kSiteDirections; ++k) {
                const int dk = std::abs(((k - site.gate) % kSiteDirections + kSiteDirections + 12) % kSiteDirections - 12);
                if (dk < 5 || towerAt(k) || towerAt(k + 1) || k == highRun) continue;   // not the high route's run
                glm::vec2 d, n, mid;
                float len = 0.0f;
                frame(k, d, n, mid, len);
                if (len < 6.0f) continue;
                const float floor = ground(mid + n * 3.25f) + 2.5f;
                float cover = 1.0e30f;
                for (const float in : {2.0f, 3.5f, 5.0f})
                    for (const float sideways : {-2.0f, 0.0f, 2.0f}) cover = std::min(cover, ground(mid - n * in + d * sideways) - (floor + 2.75f));
                const size_t i0 = size_t(k), i1 = size_t((k + 1) % kSiteDirections);
                const float gap = std::min(site.curtain[i0] - site.inner[i0], site.curtain[i1] - site.inner[i1]);
                const float score = std::min(cover, 3.0f) - (gap < 9.0f ? 100.0f : 0.0f);
                if (score > bestScore) {
                    bestScore = score;
                    best = k;
                }
            }
            if (best >= 0) {
                glm::vec2 d, n, mid;
                float len = 0.0f;
                frame(best, d, n, mid, len);
                if (curtainRecord[size_t(best)] >= 0) plan.records[size_t(curtainRecord[size_t(best)])].clearAt = len * 0.5f;
                // The room's floor: 1 m of ground over its roof where the bailey stands high enough, within 0.5 m under
                // and 2 m over the grate's.
                const float mouth = ground(mid + n * 3.25f) + 2.0f;
                float inside = 1.0e30f;
                for (const float in : {2.0f, 3.5f, 5.0f})
                    for (const float sideways : {-2.0f, 0.0f, 2.0f}) inside = std::min(inside, ground(mid - n * in + d * sideways));
                const float floor = std::clamp(inside - 3.75f, mouth - 0.5f, mouth + 2.0f);
                const auto tunnel = [&](glm::vec2 from, glm::vec2 to, float lo, float hi, float half, float height) {
                    FortRecord r;
                    r.shape = FortShape::Tunnel;
                    r.a = from;
                    r.b = to;
                    r.lo = lo;
                    r.hi = hi;
                    r.half = half;
                    r.height = height;
                    const float reach = half + 0.5f;
                    float gLo = 0.0f, gHi = 0.0f;
                    groundRange(glm::min(from, to) - reach, glm::max(from, to) + reach, gLo, gHi);
                    setBounds(r, glm::min(from, to) - reach, glm::max(from, to) + reach, std::min(std::min(lo, hi), gLo) - 0.5f,
                              std::max(lo, hi) + height + 0.5f);
                    plan.records.push_back(r);
                    return plan.records.size() - 1;
                };
                // The crawl, from beyond the batter and its buttresses into the room.
                const glm::vec2 crawlFrom = mid + n * 3.25f, crawlTo = mid - n * 2.0f;
                const size_t crawl = tunnel(crawlFrom, crawlTo, mouth, floor, 0.5f, 1.0f);
                plan.records[crawl].grateTo = 0.25f;
                plan.records[crawl].bulkheadAt = Length(crawlTo - crawlFrom) - 0.5f;
                const size_t room = tunnel(mid - n * 2.0f, mid - n * 5.0f, floor, floor, 2.0f, 2.5f);
                plan.records[room].caps = 3;
                // A spiral stair in the room's middle up to the bailey's ground; where the room's roof stands above the
                // ground (a low site), to the roof, a drop of at most 2.75 m.
                const glm::vec2 c = mid - n * 3.5f;
                float top = floor + 2.75f;
                for (const glm::vec2 o : {glm::vec2(0.0f), glm::vec2(1.25f, 0.0f), glm::vec2(-1.25f, 0.0f), glm::vec2(0.0f, 1.25f), glm::vec2(0.0f, -1.25f)})
                    top = std::max(top, ground(c + o));
                FortRecord sp;
                sp.shape = FortShape::Spiral;
                sp.a = c;
                sp.lo = floor;
                sp.hi = top;
                sp.half = 1.0f;
                sp.height = 2.5f;
                setBounds(sp, c - 1.5f, c + 1.5f, floor - 0.5f, top + 2.5f);
                plan.records.push_back(sp);
                plan.culvertExit = c;
                plan.culvertExitHeight = top;
                plan.culvert = true;
                plan.culvertMouth = mid + n * 1.25f;
                plan.culvertFloor = mouth;
            }
        }

        // An oriented box (a deck, a beam, a doorway's air) from a to b, `half` either side, from y0 to y1.
        const auto beam = [&](glm::vec2 from, glm::vec2 to, float half, float y0, float y1, uint8_t id, float rise = 0.0f) {
            FortRecord r;
            r.shape = FortShape::Beam;
            r.a = from;
            r.b = to;
            r.half = half;
            r.lo = y0;
            r.hi = y1;
            r.id = id;
            r.height = rise;
            setBounds(r, glm::min(from, to) - half - vs, glm::max(from, to) + half + vs, y0 + std::min(0.0f, rise), y1 + std::max(0.0f, rise));
            plan.records.push_back(r);
        };

        // 7. The bailey's lean-to buildings (fortress.md "Lean-to buildings"): against the curtain's inner face on the
        // runs 3-9 directions from the gate (not the culvert's run), clear of a tower's skirt; as deep as the outer
        // bailey allows with a 1 m lane left before the inner wall's batter; the roof 1 in 2 from 1.75 m under the walk
        // (the workshop's 1 m: the mid route M1 steps from its roof onto the walk).
        {
            static constexpr FortBuilding kKinds[] = {FortBuilding::Workshop, FortBuilding::Stables, FortBuilding::Forge, FortBuilding::Barracks,
                                                      FortBuilding::Kitchen, FortBuilding::Granary, FortBuilding::Barracks, FortBuilding::Stables};
            int placed = 0;
            for (int k = 0; k < kSiteDirections; ++k) {
                const int dk = std::abs(((k - site.gate) % kSiteDirections + kSiteDirections + 12) % kSiteDirections - 12);
                const int ri = curtainRecord[size_t(k)];
                if (dk < 3 || dk > 9 || ri < 0) continue;
                const FortRecord& wall = plan.records[size_t(ri)];
                if (wall.clearAt > -1.0e29f) continue;
                const float len = Length(wall.b - wall.a);
                const glm::vec2 d = (wall.b - wall.a) / len;
                glm::vec2 in(-d.y, d.x);
                if (Dot(in, keep - (wall.a + wall.b) * 0.5f) < 0.0f) in = -in;
                const size_t i0 = size_t(k), i1 = size_t((k + 1) % kSiteDirections);
                const float gap = std::min(site.curtain[i0] - site.inner[i0], site.curtain[i1] - site.inner[i1]);
                float depth = std::min(5.0f, gap - 1.25f - 1.0f - kBatterMax - 1.0f);
                if (depth < 2.5f) continue;
                // Clear of a tower's battered skirt (its back is square, 3.5 m + the batter): the run meets the tower at
                // an angle, so the distance along it is not enough.
                const auto clearOfTower = [&](float t, int corner) {
                    if (!towerAt(corner)) return true;
                    const glm::vec2 c = vertex(site.curtain, corner), o = LandSiteDirection(corner), side(-o.y, o.x);
                    for (float w = -0.25f; w <= depth + 0.75f; w += 0.25f) {
                        const glm::vec2 q = wall.a + d * t + in * (1.25f + w) - c;
                        if (std::max(std::abs(Dot(q, o)), std::abs(Dot(q, side))) < 3.5f + kBatterMax + 0.25f) return false;
                    }
                    return true;
                };
                // Shallower by 0.5 m while that is too short (a tower's corner cuts into the deep side first).
                float t0 = 1.5f, t1 = len - 1.5f;
                for (;; depth -= 0.5f) {
                    t0 = 1.5f;
                    t1 = len - 1.5f;
                    while (t0 < len && !clearOfTower(t0 - 0.25f, k)) t0 += 0.25f;
                    while (t1 > 0.0f && !clearOfTower(t1 + 0.25f, k + 1)) t1 -= 0.25f;
                    if (t1 - t0 >= 3.0f || depth - 0.5f < 2.5f) break;
                }
                const float length = t1 - t0;   // posts every 2 m from its start, corner posts at both ends
                if (length < 3.0f) continue;
                const FortBuilding kind = kKinds[placed % 8];
                const glm::vec2 a = wall.a + d * t0 + in * 1.25f, b = a + d * length;
                float floor = -1.0e30f, walk = 1.0e30f;
                for (float t = 0.0f; t <= length + 0.01f; t += 1.0f) {
                    walk = std::min(walk, FortWalk(wall, t0 + t));
                    for (float w = 0.0f; w <= depth + 0.01f; w += 1.0f) floor = std::max(floor, ground(a + d * t + in * w));
                }
                const float raise = kind == FortBuilding::Granary ? 0.75f : 0.0f;
                const float high = walk - (kind == FortBuilding::Workshop ? 1.0f : 1.75f);
                depth = std::min(depth, 2.0f * (high - floor - raise - 2.75f));   // the front wall keeps a 2.25 m door
                if (depth < 2.5f) continue;
                FortRecord r;
                r.shape = FortShape::LeanTo;
                r.building = kind;
                r.a = a;
                r.b = b;
                r.out = in;
                r.half = depth;
                r.lo = floor;
                r.hi = high;
                r.height = (kind == FortBuilding::Workshop || kind == FortBuilding::Barracks) && high - floor - raise >= 6.5f ? 2.0f : 1.0f;
                r.id = kind == FortBuilding::Stables || kind == FortBuilding::Granary ? m.thatch
                       : kind == FortBuilding::Forge || kind == FortBuilding::Kitchen ? m.roofTile
                                                                                     : m.planks;
                const glm::vec2 c0 = glm::min(a, b), c1 = glm::max(a, b);
                const glm::vec2 far0 = glm::min(a + in * (depth + 0.5f), b + in * (depth + 0.5f)), far1 = glm::max(a + in * (depth + 0.5f), b + in * (depth + 0.5f));
                float gLo = 0.0f, gHi = 0.0f;
                groundRange(glm::min(c0, far0) - 0.5f, glm::max(c1, far1) + 0.5f, gLo, gHi);
                setBounds(r, glm::min(c0, far0) - 0.5f, glm::max(c1, far1) + 0.5f, std::min(gLo, floor) - 0.75f, high + 2.0f);
                plan.records.push_back(r);
                ++placed;
            }
        }

        // 8. The walkway (fortress.md "Collapse-to-ramp set pieces"): a timber bridge 1.5 m wide from the wall walks'
        // landing in a flank tower, out of its back door, up at most 1 in 1 to the inner wall's walk (higher than the
        // curtain's). Clear of both ends and carried only by 2 struts under its beams: cut them and the deck falls free,
        // its far end on the inner wall's walk (a ramp into the bailey). The longest such span is chosen.
        {
            int best = -1;
            float bestSpan = 0.0f;
            for (int k = 0; k < kSiteDirections; k += 3) {
                const int dk = std::abs(((k - site.gate) % kSiteDirections + kSiteDirections + 12) % kSiteDirections - 12);
                if (!towerAt(k) || k == unfinishedTower || dk < 3 || dk > 9) continue;
                const size_t i = size_t(k);
                const glm::vec2 c = vertex(site.curtain, k);
                const FortRecord* innerRun = nullptr;
                const FortRecord* tower = nullptr;
                for (const FortRecord& r : plan.records) {
                    if (r.shape == FortShape::Wall && r.half < 1.2f && Length(r.a - vertex(site.inner, k)) < 0.01f) innerRun = &r;
                    if (r.shape == FortShape::Block && r.block == FortBlock::Tower && Length(r.a - c) < 0.01f) tower = &r;
                }
                if (innerRun == nullptr || tower == nullptr || std::abs(tower->walkLeft - tower->walkRight) > 0.01f) continue;
                // From 0.375 m clear of the tower (3 voxels whatever the facing) to 2 m out of the inner wall's middle: its
                // face, and the corner of the 2 runs that meet here beside the deck.
                const float span = (site.curtain[i] - 3.5f - 0.375f) - (site.inner[i] + 2.0f);
                const float rise = FortWalk(*innerRun, 0.5f) + 0.25f + vs - tower->walkLeft;
                if (span < 2.0f || std::abs(rise) > span || span <= bestSpan) continue;
                best = k;
                bestSpan = span;
            }
            if (best >= 0) {
                const size_t i = size_t(best);
                const glm::vec2 o = LandSiteDirection(best), side(-o.y, o.x);
                const glm::vec2 c = vertex(site.curtain, best);
                const FortRecord* innerRun = nullptr;
                float landing = 0.0f;
                for (const FortRecord& r : plan.records) {
                    if (r.shape == FortShape::Wall && r.half < 1.2f && Length(r.a - vertex(site.inner, best)) < 0.01f) innerRun = &r;
                    if (r.shape == FortShape::Block && r.block == FortBlock::Tower && Length(r.a - c) < 0.01f) landing = r.walkLeft;
                }
                const float walk = FortWalk(*innerRun, 0.5f);
                const float deckTop = walk + 0.25f + vs;   // over the walk, a voxel clear of it at any voxel size
                const glm::vec2 from = c - o * (3.5f + 0.375f), edge = keep + o * (site.inner[i] + 2.0f), to = keep + o * (site.inner[i] + 0.25f);
                const float span = Length(edge - from), slope = (deckTop - landing) / span;
                // The doorways: through the tower's back shell onto its landing, through the inner wall's parapet.
                beam(c - o * 1.75f, c - o * 3.75f, 0.6f, landing, landing + 2.25f, 0);
                beam(edge + o * 0.25f, keep + o * (site.inner[i] - 0.25f), 1.0f, walk, deckTop + 2.25f, 0);
                // A plank step a voxel high on the walk, 0.25 m clear of the deck's end: 2 steps under the stepping height.
                beam(to - o * 0.25f, to - o * 0.75f, 0.75f, walk, walk + vs, m.planks);
                // The deck: up from the landing's level to the wall's face, then level over the walk.
                beam(from, edge, 0.75f, landing - 0.25f, landing, m.planks, deckTop - landing);
                beam(edge, to, 0.75f, deckTop - 0.25f, deckTop, m.planks);
                // The carrying beams under the slope, stopping 0.5 m short of its end.
                const glm::vec2 beamEnd = keep + o * (site.inner[i] + 2.5f);
                const float beamRise = slope * Length(beamEnd - from);
                for (const float s : {-0.625f, 0.625f}) beam(from + side * s, beamEnd + side * s, 0.125f, landing - 0.5f, landing - 0.25f, m.wood, beamRise);
                // The struts at a third of the span: 0.375 m square under each carrying beam, up into its rows (the
                // same voxel rule as the beam, so they meet whatever the facing).
                const float along = span / 3.0f;
                const glm::vec2 strut = from - o * along;
                const float foot = std::min(ground(strut + side * 0.625f), ground(strut - side * 0.625f));
                const float under = landing + slope * along - 0.25f;
                for (const float s : {-0.625f, 0.625f})
                    beam(strut + side * s - o * 0.1875f, strut + side * s + o * 0.1875f, 0.1875f, foot - 0.5f, under, m.wood);
                plan.walkway = true;
                plan.walkwayStrut = strut;
                plan.walkwayGround = foot;
                plan.walkwayDeck = under + 0.25f;
            }
        }

        // 9. The keep on its pad: 16 x 16 m, 22 m high, its door towards the gate.
        block(FortBlock::Keep, keep, axis, 8.0f, site.keepGround, site.keepGround + 22.0f);
        plan.records.back().trim = FortTrim_Machicolations;
        {
            const float top = site.keepGround + 22.0f;
            const glm::vec2 side(-axis.y, axis.x);
            // A slate pyramid inside the parapet's walk (1 in 1), small cones on the 4 corner turrets, and the great
            // hall's chimney through the roof's back slope.
            roof(FortRoof::Cone, FortBlock::Keep, keep, axis, 5.25f, 0.0f, top, 1.0f, m.slate);
            for (const float du : {-6.75f, 6.75f})
                for (const float dv : {-6.75f, 6.75f})
                    roof(FortRoof::Cone, FortBlock::Keep, keep + axis * du + side * dv, axis, 1.5f, 0.0f, top + 2.5f + kMerlon, 2.0f, m.slate);
            const glm::vec2 flue = keep - axis * 3.5f + side * 2.0f;
            column(flue, 0.5f, top - 0.5f, top + 6.5f, m.brick);
            column(flue, 0.25f, top + 3.0f, top + 6.5f, 0);
        }

        // 10. Construction scaffolding (fortress.md "Construction scaffolding, walkways and cranes"): putlog scaffolds,
        // standing 1.25 m off the face (clear of the batter), decks 2.25 m deep every 2.5 m (2 m clear under the next
        // deck's ledgers) with stair flights between them at alternate ends (a player cannot climb a ladder) and a 1 m
        // lane beside each flight; stacks of planks and cut stone in front. With a base (a door's floor) the lowest deck
        // lies on it; without, the decks count down from a fixed top deck and a flight from the ground at the higher
        // end climbs to the lowest.
        const auto scaffold = [&](glm::vec2 a, glm::vec2 b, glm::vec2 away, float top, float base, uint8_t site) {
            constexpr float kLift = 2.5f;
            const float len = Length(b - a);
            if (len < 6.0f) return;   // 2 flights of 2.5 m at alternate ends
            const glm::vec2 along = (b - a) / len;
            const float groundA = ground(a + along * 0.5f + away * 2.125f), groundB = ground(b - along * 0.5f + away * 2.125f);
            if (groundA > groundB) std::swap(a, b);   // a flight from the ground starts at b
            FortRecord r;
            r.shape = FortShape::Scaffold;
            r.a = a;
            r.b = b;
            r.out = away;
            r.id = site;
            float gLo = 0.0f, gHi = 0.0f;
            const glm::vec2 lo = glm::min(glm::min(a, b), glm::min(a + away * 3.75f, b + away * 3.75f)), hi = glm::max(glm::max(a, b), glm::max(a + away * 3.75f, b + away * 3.75f));
            groundRange(lo, hi, gLo, gHi);
            const float foot = std::max(groundA, groundB);
            if (base > -1.0e29f) {
                // Decks level with the door's floor. On level ground the lowest lies on it; on a slope it stands 1.8 m
                // clear of the ground under its ledgers (the door stays open), reached by a flight from the ground
                // where one fits, else only from above.
                r.lo = base - 0.25f;
                r.hi = r.lo + kLift * std::floor((top - r.lo) / kLift);
                r.height = 0.0f;
                if (gHi > base + 0.125f) {
                    r.lo += kLift * std::ceil((gHi + 2.05f - r.lo) / kLift);
                    r.height = 0.25f * std::ceil((r.lo - foot) / 0.25f);
                    if (r.height > len - 3.5f) r.height = -1.0f;
                }
            } else {
                // The lowest deck over the highest ground, or lower (down to the flight's foot) while the flight is too
                // long: its top stays clear of the lowest deck's own flight.
                r.hi = top;
                r.lo = top - kLift * std::floor((top - gHi) / kLift);
                r.height = 0.25f * std::ceil((r.lo - foot) / 0.25f);
                while (r.height > len - 3.5f && r.lo - kLift >= foot) {
                    r.lo -= kLift;
                    r.height = 0.25f * std::ceil((r.lo - foot) / 0.25f);
                }
                if (r.height > len - 3.5f) return;
            }
            if (r.hi < r.lo + kLift - 0.01f) return;
            setBounds(r, lo - 0.5f, hi + 0.5f, std::min(gLo, r.lo) - 0.5f, r.hi + 1.5f);
            plan.records.push_back(r);
            // The materials of the work, in front of its first bay: a stack of planks, a pile of cut stone.
            const glm::vec2 d = (b - a) / len;
            const glm::vec2 stack = a + d * 1.0f + away * 4.5f, pile = a + d * 2.75f + away * 4.5f;
            const float g0 = ground(stack), g1 = ground(pile);
            beam(stack - d * 0.75f, stack + d * 0.75f, 0.4f, g0 - 0.25f, g0 + 0.5f, m.planks);
            beam(pile - d * 0.5f, pile + d * 0.5f, 0.5f, g1 - 0.25f, g1 + 0.75f, m.ashlar);
        };
        // H1: along the whole run outside the repair patch; its top deck level with the walk at the gap.
        if (highRun >= 0) {
            const FortRecord& wall = plan.records[size_t(curtainRecord[size_t(highRun)])];
            const float len = Length(wall.b - wall.a);
            const glm::vec2 d = (wall.b - wall.a) / len;
            glm::vec2 n(d.y, -d.x);
            if (Dot(n, (wall.a + wall.b) * 0.5f - keep) < 0.0f) n = -n;
            const glm::vec2 face = wall.a + n * wall.half;
            const float walk = FortWalk(wall, (highLo + highHi) * 0.5f);
            scaffold(face + d * 0.5f, face + d * (len - 0.5f), n, walk - 0.25f, -1.0e30f, 0);
        }
        // Along the unfinished tower's back (the bailey side), from its door's floor up to its finished top.
        for (size_t j = 0; j < plan.records.size(); ++j) {
            const FortRecord r = plan.records[j];
            if (r.shape != FortShape::Block || (r.trim & FortTrim_Unfinished) == 0) continue;
            const glm::vec2 back = r.a - r.b * r.half, side(-r.b.y, r.b.x);
            scaffold(back - side * (r.half - 0.5f), back + side * (r.half - 0.5f), -r.b, r.hi - 2.5f, r.lo, 1);
            break;
        }
        // On the keep's gate face, from its door's floor up to the sill of its upper storey's windows.
        {
            const glm::vec2 side(-axis.y, axis.x), face = keep + axis * 8.0f;
            scaffold(face - side * 5.25f, face + side * 5.25f, axis, site.keepGround + 18.0f, site.keepGround, 2);
        }

        // 11. Room kits, lights and props (fortress.md "Vignettes and room kits", "Movable props"): furniture as stamped
        // voxels, each light's flame in its own voxel (worldgen.md C8), and the spots where the template drops the movable
        // props. A frame (o, u, v) is a room's middle and its 2 horizontal axes; uv() and uvBox() place in it.
        {
            const glm::vec3 kFlame(1.0f, 0.6f, 0.28f), kEmbers(1.0f, 0.42f, 0.16f);
            const auto cell = [&](glm::vec3 at, uint8_t id) { box(at, at, id); };   // the one voxel holding `at`
            const auto uv = [](glm::vec2 o, glm::vec2 u, glm::vec2 v, float a, float b) { return o + u * a + v * b; };
            const auto uvBox = [&](glm::vec2 o, glm::vec2 u, glm::vec2 v, float u0, float u1, float v0, float v1, float y0, float y1, uint8_t id) {
                const glm::vec2 p0 = uv(o, u, v, u0, v0), p1 = uv(o, u, v, u1, v1);
                box(glm::vec3(std::min(p0.x, p1.x), y0, std::min(p0.y, p1.y)), glm::vec3(std::max(p0.x, p1.x), y1, std::max(p0.y, p1.y)), id);
            };
            // A torch on an iron bracket 2 voxels out of a face (n: out of it, along x or z), its flame a voxel over the
            // bracket's end at y.
            const auto torch = [&](glm::vec2 face, glm::vec2 n, float y) {
                const glm::vec2 p1 = face + n * (0.5f * vs), p2 = face + n * (1.5f * vs);
                cell(glm::vec3(p1.x, y - vs * 0.5f, p1.y), m.iron);
                cell(glm::vec3(p2.x, y - vs * 0.5f, p2.y), m.iron);
                cell(glm::vec3(p2.x, y + vs * 0.5f, p2.y), m.torch);
                const glm::vec2 l = p2 + n * 0.3f;
                plan.lights.push_back({glm::vec3(l.x, y + 0.35f, l.y), kFlame, 3.0f, 6.0f});
            };
            const auto prop = [&](FortProp kind, glm::vec2 at, float y, bool alongX) {
                if (plan.props.size() < 40) plan.props.push_back({kind, glm::vec3(at.x, y, at.y), alongX});
            };
            const bool axisX = std::abs(axis.x) > 0.5f;
            // Clear of the walls: the curtain's inner face and the inner wall's outer batter, by the site's radii in the
            // nearest direction (a margin for the polygon's chords).
            const auto radius = [&](const std::array<float, kSiteDirections>& radii, glm::vec2 p) {
                const glm::vec2 d = p - keep;
                return radii[size_t(LandSiteNearestDirection(d.x, d.y))];
            };
            const auto inOuterBailey = [&](glm::vec2 p) {
                const float r = Length(p - keep);
                return r <= radius(site.curtain, p) - 2.5f && r >= radius(site.inner, p) + 1.25f + kBatterMax + 0.75f;
            };
            const auto inInnerBailey = [&](glm::vec2 p) { return Length(p - keep) <= radius(site.inner, p) - 1.25f - kBatterMax - 0.75f; };

            // The guardroom over the gate passage: the room holds u in [-3, 3] (u out of the castle) and v in [-5, 5],
            // its floor 5 m up; the portcullis rises through the slot at u = 2.5. A stone stair against the gatehouse's
            // bailey face climbs to a door at v = 3-4.25, on the side of the gate where it stays clear of the curtain and
            // the inner wall (the room's kit is mirrored with it).
            {
                float side = 1.0f;
                for (const float s : {1.0f, -1.0f}) {
                    bool fits = true;
                    for (const float a : {-5.25f, -4.25f})
                        for (const float b : {3.0f, 6.0f, 9.25f}) fits = fits && inOuterBailey(gatePoint + axis * a + across * (b * s));
                    if (fits) {
                        side = s;
                        break;
                    }
                }
                const glm::vec2 o = gatePoint, u = axis, v = across * side;
                const float floor = gateGround + 5.0f;
                float sLo = 0.0f, sHi = 0.0f;
                groundRange(glm::min(uv(o, u, v, -5.25f, 3.0f), uv(o, u, v, -4.25f, 9.25f)), glm::max(uv(o, u, v, -5.25f, 3.0f), uv(o, u, v, -4.25f, 9.25f)), sLo,
                            sHi);
                for (int k = 0; k < 20; ++k)
                    uvBox(o, u, v, -5.25f, -4.25f, 9.25f - 0.25f * float(k + 1), 9.25f - 0.25f * float(k), sLo - 0.5f, gateGround + 0.25f * float(k + 1),
                          m.ashlar);
                uvBox(o, u, v, -5.25f, -4.25f, 3.0f, 4.25f, sLo - 0.5f, floor, m.ashlar);   // the landing
                uvBox(o, u, v, -4.25f, -2.875f, 3.0f, 4.25f, floor, floor + 2.25f, 0);       // the door
                // The winch drum over the slot's inner side, on 2 posts.
                uvBox(o, u, v, 1.5f, 2.0f, -2.0f, 2.0f, floor + 0.75f, floor + 1.25f, m.wood);
                for (const float side : {-2.375f, 2.125f}) uvBox(o, u, v, 1.5f, 2.0f, side, side + 0.25f, floor, floor + 1.5f, m.wood);
                // A weapon rack on the back wall: 2 rails and 4 spears.
                for (const float y : {floor + 0.5f, floor + 1.375f}) uvBox(o, u, v, -3.0f, -2.75f, -4.5f, -2.5f, y, y + 0.125f, m.wood);
                for (const float b : {-4.25f, -3.75f, -3.25f, -2.75f}) {
                    const glm::vec2 q = uv(o, u, v, -2.875f, b);
                    box(glm::vec3(q.x, floor, q.y), glm::vec3(q.x, floor + 1.875f, q.y), m.iron);
                }
                // 2 arrow barrels by the rack, a bench along the side wall.
                uvBox(o, u, v, -1.875f, -1.25f, -4.75f, -4.125f, floor, floor + 0.75f, m.wood);
                uvBox(o, u, v, -0.875f, -0.25f, -4.75f, -4.125f, floor, floor + 0.75f, m.wood);
                uvBox(o, u, v, -2.0f, 0.0f, 4.375f, 4.75f, floor, floor + 0.5f, m.planks);
                // 2 braziers in the front corners: an iron bowl of embers on a leg.
                for (const float b : {-4.25f, 4.25f}) {
                    uvBox(o, u, v, 1.875f, 2.125f, b - 0.125f, b + 0.125f, floor, floor + 0.5f, m.iron);
                    uvBox(o, u, v, 1.75f, 2.25f, b - 0.25f, b + 0.25f, floor + 0.5f, floor + 0.75f, m.iron);
                    uvBox(o, u, v, 1.875f, 2.125f, b - 0.125f, b + 0.125f, floor + 0.75f, floor + 0.875f, m.torch);
                    const glm::vec2 l = uv(o, u, v, 2.0f, b);
                    plan.lights.push_back({glm::vec3(l.x, floor + 1.4f, l.y), kEmbers, 3.5f, 7.0f});
                }
                const glm::vec2 lever = uv(o, u, v, -1.25f, 2.5f);   // 3 m and more from the portcullis (the authoring rule)
                plan.guardLever = glm::vec3(lever.x, floor, lever.y);
                // 2 barrels by the back wall.
                for (const float b : {-2.0f, -1.25f}) prop(FortProp::Barrel, uv(o, u, v, -2.25f, b), floor, axisX);
                // Torches in the passage, on its walls inside the portcullis.
                for (const float side : {-1.0f, 1.0f}) torch(uv(o, u, v, -3.5f, 2.0f * side), -v * side, gateGround + 2.25f);
            }

            // The great hall: the keep's second storey (u toward the gate, its inner faces at 6 m), 2 tables with
            // benches, a stone fireplace in the middle of the back wall and a torch in the middle of each side wall (the
            // windows are 2.5 m either side of a face's middle), soot.
            {
                const glm::vec2 o = keep, u = axis, v(-axis.y, axis.x);
                const float floor = site.keepGround + 11.25f;
                uvBox(o, u, v, -6.0f, -5.25f, -1.25f, -0.875f, floor, floor + 1.5f, m.ashlar);   // the jambs
                uvBox(o, u, v, -6.0f, -5.25f, 0.875f, 1.25f, floor, floor + 1.5f, m.ashlar);
                uvBox(o, u, v, -6.0f, -5.25f, -1.25f, 1.25f, floor + 1.5f, floor + 2.25f, m.ashlar);   // the hood
                uvBox(o, u, v, -6.0f, -5.625f, -1.0f, 1.0f, floor + 2.25f, floor + 3.5f, m.ashlar);
                uvBox(o, u, v, -6.0f, -5.0f, -1.25f, 1.25f, floor - 0.25f, floor, m.brick);      // the hearth
                uvBox(o, u, v, -5.875f, -5.5f, -0.5f, 0.5f, floor, floor + 0.125f, m.torch);     // its embers
                uvBox(o, u, v, -6.25f, -6.0f, -1.0f, 1.0f, floor + 3.5f, floor + 5.25f, m.soot);  // soot over the hood
                {
                    const glm::vec2 l = uv(o, u, v, -5.1f, 0.0f);
                    plan.lights.push_back({glm::vec3(l.x, floor + 0.75f, l.y), kEmbers, 4.0f, 8.0f});
                }
                for (const float side : {-2.0f, 2.0f}) {
                    for (const float bench : {-0.8125f, 0.8125f})
                        uvBox(o, u, v, -0.5f, 2.5f, side + bench - 0.1875f, side + bench + 0.1875f, floor, floor + 0.4375f, m.planks);
                    prop(FortProp::Table, uv(o, u, v, 1.0f, side), floor, axisX);
                }
                for (const float side : {-1.0f, 1.0f}) torch(uv(o, u, v, 0.0f, 6.0f * side), -v * side, floor + 2.0f);
                // Soot on the chimney's top, its flue left open.
                const float top = site.keepGround + 22.0f;
                const glm::vec2 flue = keep - axis * 3.5f + v * 2.0f;
                column(flue, 0.5f, top + 6.0f, top + 6.5f, m.soot);
                column(flue, 0.25f, top + 6.0f, top + 6.5f, 0);
            }

            // The lean-tos' kits by kind, clear of the door, the stair and the hearth.
            std::vector<FortRecord> leanTos;
            for (const FortRecord& r : plan.records)
                if (r.shape == FortShape::LeanTo) leanTos.push_back(r);
            for (const FortRecord& r : leanTos) {
                const float len = Length(r.b - r.a);
                const glm::vec2 d = (r.b - r.a) / len, in = r.out;
                const float depth = r.half;
                const float floor = r.lo + (r.building == FortBuilding::Granary ? 0.75f : 0.0f);
                const int storeys = int(r.height);
                const float doorT = 2.0f * std::floor(len / 4.0f);
                const auto at = [&](float t, float w) { return r.a + d * t + in * w; };
                const bool hearth = r.building == FortBuilding::Forge || r.building == FortBuilding::Kitchen;
                std::vector<glm::vec4> used;   // (t0, t1, w0, w1) of every kit piece and prop
                const auto fits = [&](float t0, float t1, float w0, float w1) {
                    if (t0 < 0.25f || t1 > len - 0.25f || w0 < 0.25f || w1 > depth - 0.25f) return false;
                    if (storeys > 1 && t1 > len - 4.25f && w0 < 1.25f) return false;                  // the stair
                    if (w1 > depth - 1.5f && t1 > doorT - 0.25f && t0 < doorT + 2.25f) return false;   // the door's way in
                    return !(hearth && t0 < 1.75f && w0 < 1.5f);                                       // the hearth
                };
                const auto free = [&](float t0, float t1, float w0, float w1) {
                    if (!fits(t0, t1, w0, w1)) return false;
                    for (const glm::vec4& q : used)
                        if (t0 < q.y && t1 > q.x && w0 < q.w && w1 > q.z) return false;
                    return true;
                };
                // A kit piece (pieces of one kit may stand on each other: a berth over a berth, an anvil on its block).
                const auto piece = [&](float t0, float t1, float w0, float w1, float y0, float y1, uint8_t id) {
                    const bool onHearth = hearth && id == m.torch;   // the embers lie on it
                    if (!onHearth && !fits(t0, t1, w0, w1)) return;
                    beam(at(t0, (w0 + w1) * 0.5f), at(t1, (w0 + w1) * 0.5f), (w1 - w0) * 0.5f, y0, y1, id);
                    used.emplace_back(t0, t1, w0, w1);
                };
                // A prop on the floor: the first free spot from the far end, its footprint taken as the circle round it
                // (the props stand square to the world, the building may not).
                const auto place = [&](FortProp kind, int count) {
                    const glm::vec3 size = PropSize(kind);
                    const float rad = 0.5f * std::sqrt(size.x * size.x + size.y * size.y) + 0.0625f;
                    for (int k = 0; k < count; ++k)
                        for (float t = len - 0.25f - rad; t >= 0.25f + rad; t -= 0.25f) {
                            bool done = false;
                            for (float w = 0.25f + rad; w <= depth - 0.25f - rad && !done; w += 0.25f)
                                if (free(t - rad, t + rad, w - rad, w + rad)) {
                                    prop(kind, at(t, w), floor, axisX);
                                    used.emplace_back(t - rad, t + rad, w - rad, w + rad);
                                    done = true;
                                }
                            if (done) break;
                        }
                };
                switch (r.building) {
                case FortBuilding::Forge:
                    piece(0.375f, 1.625f, 1.125f, 1.375f, floor + 0.75f, floor + 0.875f, m.torch);   // embers on the hearth
                    piece(2.75f, 3.25f, 1.0f, 1.5f, floor, floor + 0.5f, m.wood);                    // the anvil's block
                    piece(2.5f, 3.5f, 1.125f, 1.375f, floor + 0.5f, floor + 0.75f, m.iron);          // the anvil
                    piece(3.75f, 4.5f, 0.5f, 1.25f, floor, floor + 0.625f, m.planks);                // the quench tub
                    piece(3.875f, 4.375f, 0.625f, 1.125f, floor + 0.125f, floor + 0.5f, m.water);
                    {
                        const glm::vec2 l = at(1.0f, 1.75f);
                        plan.lights.push_back({glm::vec3(l.x, floor + 1.25f, l.y), kEmbers, 4.0f, 7.0f});
                    }
                    place(FortProp::Barrel, 2);
                    break;
                case FortBuilding::Kitchen: {
                    const float end = std::min(len - 0.75f, 5.25f);
                    piece(0.375f, 1.625f, 1.125f, 1.375f, floor + 0.75f, floor + 0.875f, m.torch);   // embers in the oven's mouth
                    piece(2.25f, end, depth * 0.5f - 0.375f, depth * 0.5f + 0.375f, floor + 0.75f, floor + 0.875f, m.planks);   // the table
                    piece(2.25f, 2.5f, depth * 0.5f - 0.375f, depth * 0.5f + 0.375f, floor, floor + 0.75f, m.wood);
                    piece(end - 0.25f, end, depth * 0.5f - 0.375f, depth * 0.5f + 0.375f, floor, floor + 0.75f, m.wood);
                    for (float t = 2.25f; t + 0.625f <= len - 0.5f; t += 0.875f) piece(t, t + 0.625f, 0.25f, 0.875f, floor, floor + 0.75f, m.wood);   // casks
                    const glm::vec2 l = at(1.0f, 1.75f);
                    plan.lights.push_back({glm::vec3(l.x, floor + 1.25f, l.y), kEmbers, 3.0f, 6.0f});
                    break;
                }
                case FortBuilding::Stables:
                    for (float t = 2.0f; t < len - 0.5f; t += 2.0f) {   // stall partitions from the back wall
                        beam(at(t, 0.25f), at(t, depth - 1.5f), 0.125f, floor, floor + 1.25f, m.planks);
                        used.emplace_back(t - 0.125f, t + 0.125f, 0.25f, depth - 1.5f);
                    }
                    piece(0.25f, 1.25f, 0.25f, 1.0f, floor, floor + 0.75f, m.thatch);               // hay
                    for (float t = 2.5f; t + 1.0f <= len - 0.25f; t += 2.0f) {                       // a trough in each stall
                        piece(t, t + 1.0f, 0.25f, 0.625f, floor, floor + 0.5f, m.planks);
                        piece(t + 0.125f, t + 0.875f, 0.25f, 0.5f, floor + 0.25f, floor + 0.5f, m.thatch);
                    }
                    place(FortProp::Barrel, 2);
                    break;
                case FortBuilding::Barracks:
                    for (float t = 0.25f; t + 1.5f <= len - 0.25f; t += 2.0f) {   // bunks of 2 berths along the back wall
                        for (const float y : {floor + 0.375f, floor + 1.375f}) piece(t, t + 1.5f, 0.25f, 1.0f, y, y + 0.125f, m.planks);
                        for (const float e : {0.0f, 1.25f}) piece(t + e, t + e + 0.25f, 0.75f, 1.0f, floor, floor + 1.5f, m.wood);
                    }
                    piece(1.0f, 2.0f, depth - 1.25f, depth - 0.75f, floor, floor + 0.5f, m.wood);   // a chest
                    break;
                case FortBuilding::Workshop: {
                    // The bench across the end wall (the stair runs along the back wall at the other end).
                    piece(0.25f, 0.875f, 0.5f, 2.5f, floor + 0.75f, floor + 0.875f, m.planks);
                    for (const float e : {0.5f, 2.25f}) piece(0.25f, 0.875f, e, e + 0.25f, floor, floor + 0.75f, m.wood);
                    piece(3.0f, 4.5f, 1.25f, 1.75f, floor, floor + 0.25f, m.wood);                  // timber
                    piece(3.0f, 4.5f, 1.25f, 1.75f, floor + 0.25f, floor + 0.5f, m.wood);
                    place(FortProp::Crate, 2);
                    break;
                }
                case FortBuilding::Granary:
                    for (float t = 0.5f; t + 0.625f <= len - 0.5f; t += 0.875f) piece(t, t + 0.625f, 0.375f, 1.0f, floor, floor + 0.5f, m.thatch);   // sacks
                    break;
                }
            }

            // At the work sites: planks, beams, ladders, crates, a cart and the hay wagon in front of the keep's scaffold,
            // clear of the way from the inner gate to the keep's door (its middle 2.5 m); planks, beams and ladders behind
            // the unfinished tower. Each kind is a stack on timber skids: a level block of planks under its footprint where the
            // ground slopes (none where the slope would need more than 1 m), so the template drops each piece onto the one
            // before and it rests.
            for (size_t j = 0; j < plan.records.size(); ++j) {
                const FortRecord r = plan.records[j];
                if (r.shape != FortShape::Scaffold || r.id == 0) continue;
                const float len = Length(r.b - r.a);
                const glm::vec2 d = (r.b - r.a) / len;
                const auto at = [&](float t, float out) { return r.a + d * t + r.out * out; };
                const bool along = std::abs(d.x) > 0.5f;
                const auto stack = [&](FortProp kind, float t, float out, int count) {
                    const glm::vec2 c = at(t, out);
                    const glm::vec3 size = PropSize(kind);
                    const glm::vec2 half = (along ? glm::vec2(size.x, size.y) : glm::vec2(size.y, size.x)) * 0.5f + 0.125f;
                    for (const float sx : {-1.0f, 1.0f})
                        for (const float sz : {-1.0f, 1.0f}) {
                            const glm::vec2 corner = c + glm::vec2(half.x * sx, half.y * sz);
                            if (r.id == 2 ? !inInnerBailey(corner) : !inOuterBailey(corner)) return;   // clear of the walls
                        }
                    float gLo = 0.0f, gHi = 0.0f;
                    groundRange(c - half, c + half, gLo, gHi);
                    if (gHi - gLo > 1.0f) return;   // too steep for skids under it
                    float base = gHi;
                    if (gHi - gLo > 0.125f) {
                        base = gHi + 0.125f;
                        box(glm::vec3(c.x - half.x, gLo - 0.25f, c.y - half.y), glm::vec3(c.x + half.x, base, c.y + half.y), m.planks);
                    }
                    for (int k = 0; k < count; ++k) prop(kind, c, base + size.z * float(k), along);
                };
                if (r.id == 2) {
                    stack(FortProp::Plank, 1.75f, 6.0f, 7);
                    stack(FortProp::Beam, 9.0f, 6.0f, 3);
                    stack(FortProp::Ladder, 9.0f, 7.25f, 2);
                    stack(FortProp::Crate, 1.0f, 7.75f, 3);
                    stack(FortProp::Crate, 2.25f, 7.75f, 3);
                    stack(FortProp::Cart, 1.75f, 10.0f, 1);
                    stack(FortProp::HayWagon, 9.0f, 10.0f, 1);
                } else {
                    stack(FortProp::Plank, 1.5f, 6.0f, 3);
                    stack(FortProp::Beam, 3.0f, 6.75f, 3);
                    stack(FortProp::Ladder, 3.0f, 7.5f, 2);
                }
            }
        }

        return plan;
    }

    uint64_t FortPlanHash(const FortPlan& plan) {
        uint64_t h = Mix(plan.placed ? 0x666f7274ull : 0ull);
        const auto put = [&](uint64_t v) { h = Mix(h ^ v) + 0x9e3779b97f4a7c15ull; };
        const auto putf = [&](float f) { put(std::bit_cast<uint32_t>(f)); };
        for (const FortRecord& r : plan.records) {
            put(uint64_t(r.shape) | uint64_t(r.block) << 8 | uint64_t(r.id) << 16);
            for (const float f : {r.a.x, r.a.y, r.b.x, r.b.y, r.lo, r.hi, r.half, r.gapLo, r.gapHi, r.walkLeft, r.walkRight, r.walkLeftU, r.walkRightU,
                                  r.clearAt, r.flatA, r.flatB, r.height, r.grateTo, r.bulkheadAt})
                putf(f);
            put(uint64_t(r.caps) | uint64_t(r.trim) << 8 | uint64_t(r.roof) << 16 | uint64_t(r.building) << 24);
            for (const float f : {r.length, r.out.x, r.out.y, r.patchLo, r.patchHi}) putf(f);
            put(uint64_t(uint32_t(r.min.x)) | uint64_t(uint32_t(r.min.y)) << 32);
            put(uint64_t(uint32_t(r.min.z)) | uint64_t(uint32_t(r.max.x)) << 32);
            put(uint64_t(uint32_t(r.max.y)) | uint64_t(uint32_t(r.max.z)) << 32);
        }
        for (const float f : {plan.guardLever.x, plan.guardLever.y, plan.guardLever.z}) putf(f);
        for (const FortLight& l : plan.lights)
            for (const float f : {l.at.x, l.at.y, l.at.z, l.color.x, l.color.y, l.color.z, l.intensity, l.radius}) putf(f);
        for (const FortPropSpot& o : plan.props) {
            put(uint64_t(o.kind) | uint64_t(o.alongX) << 8);
            for (const float f : {o.at.x, o.at.y, o.at.z}) putf(f);
        }
        return h;
    }

    float FortWalk(const FortRecord& wall, float t) {
        const float len = Length(wall.b - wall.a);
        const float ramp = len - wall.flatA - wall.flatB;
        if (t <= wall.flatA || ramp <= 0.0f) return wall.lo;
        if (t >= len - wall.flatB) return wall.hi;
        return wall.lo + (wall.hi - wall.lo) * (t - wall.flatA) / ramp;
    }

    bool FortRows(const FortPlan& plan, int x0, int z0, int x1, int z1, int& lo, int& hi) {
        bool any = false;
        for (const FortRecord& r : plan.records) {
            if (r.max.x <= x0 || r.min.x >= x1 || r.max.z <= z0 || r.min.z >= z1) continue;
            lo = any ? std::min(lo, r.min.y) : r.min.y;
            hi = any ? std::max(hi, r.max.y) : r.max.y;
            any = true;
        }
        return any;
    }

    void StampFort(const FortPlan& plan, const WorldMaterials& m, float vs, const glm::ivec3& coord, const float* ground, Chunk& chunk) {
        const glm::ivec3 c0 = coord * kChunkSize, c1 = c0 + kChunkSize;
        const auto set = [&](int x, int y, int z, uint8_t id) { chunk.ids[Chunk::VoxelIndex(x - c0.x, y - c0.y, z - c0.z)] = id; };
        for (const FortRecord& r : plan.records) {
            const glm::ivec3 lo = glm::max(r.min, c0), hi = glm::min(r.max, c1);
            if (lo.x >= hi.x || lo.y >= hi.y || lo.z >= hi.z) continue;
            switch (r.shape) {
            case FortShape::Box:
                for (int z = lo.z; z < hi.z; ++z)
                    for (int y = lo.y; y < hi.y; ++y)
                        for (int x = lo.x; x < hi.x; ++x) set(x, y, z, r.id);
                break;

            case FortShape::Trench:
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const float g = ground[(x - c0.x) + (z - c0.z) * kChunkSize] * vs;
                        if (int(std::floor((g + kClearOverGround) / vs)) <= lo.y || int(std::floor(r.lo / vs)) >= hi.y) continue;
                        const glm::vec2 p((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs);
                        float off = 0.0f;
                        if (!LandSiteOutsideCurtain(plan.site, p.x - plan.origin.x, p.y - plan.origin.y, off) || off < 4.0f || off > 10.0f) continue;
                        const float bottom = r.lo + std::max(0.0f, std::abs(off - 7.0f) - 1.5f);
                        if (g <= bottom + 0.25f) continue;
                        const int y0 = std::max(lo.y, int(std::floor(bottom / vs))), y1 = std::min(hi.y, int(std::floor((g + kClearOverGround) / vs)));
                        for (int y = y0; y < y1; ++y) set(x, y, z, 0);
                    }
                break;

            case FortShape::Wall: {
                const float len = Length(r.b - r.a);
                const glm::vec2 d = (r.b - r.a) / len;
                glm::vec2 n(d.y, -d.x);
                if (Dot(n, (r.a + r.b) * 0.5f - plan.keep) < 0.0f) n = -n;   // outward, away from the keep
                const int buttresses = len > 10.0f ? int(std::floor(len / 7.0f)) : 0;
                const float spacing = len / float(buttresses + 1);
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 p((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs);
                        const float t = Dot(p - r.a, d), s = Dot(p - r.a, n);
                        if (t < -r.half || t > len + r.half || (t >= r.gapLo && t <= r.gapHi)) continue;
                        if (s < -r.half || s > r.half + kBatterMax + 1.25f) continue;
                        const float g = ground[(x - c0.x) + (z - c0.z) * kChunkSize] * vs;
                        const float batterTop = g + kWallBatter;
                        const int k = int(std::floor(t / spacing + 0.5f));
                        const float tb = float(k) * spacing;
                        const bool buttress = k >= 1 && k <= buttresses && std::abs(t - tb) <= 0.75f &&
                                               !(r.gapHi >= r.gapLo && tb > r.gapLo - 3.0f && tb < r.gapHi + 3.0f) && std::abs(tb - r.clearAt) > 3.0f;
                        const float walk = FortWalk(r, t);
                        const int y0 = std::max(lo.y, int(std::floor((g - kFoundation) / vs)));
                        const bool hoard = (r.trim & FortTrim_Hoarding) != 0;
                        const float beat = t + 50.0f - 2.0f * std::floor((t + 50.0f) / 2.0f);
                        for (int y = y0; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs;
                            // The hoarding: putlog beams through the holes, a plank floor level with the walk (a drop slot
                            // by the face every 2 m), an outer plank wall with slits, posts on the parapet and a pent roof
                            // of planks falling 0.6 per metre outward.
                            if (hoard && s > r.half - 0.5f && ym >= walk - 0.5f) {
                                const float out = s - r.half, roofY = walk + 2.6f - std::max(0.0f, out) * 0.6f;
                                uint8_t id = 255;
                                if (out > 1.75f) id = 255;
                                else if (ym >= roofY - 0.25f && ym < roofY) id = m.planks;
                                else if (out <= 0.0f) {
                                    if (ym >= walk + kParapet && ym < roofY && beat < 0.25f) id = m.wood;   // posts
                                } else if (out <= 1.5f) {
                                    if (ym < walk - 0.25f) id = beat >= 1.0f && beat < 1.25f ? m.wood : 255;
                                    else if (ym < walk) id = out <= 0.375f && beat < 0.5f ? 0 : m.planks;
                                    else if (out > 1.25f && ym < walk + 1.75f)
                                        id = beat >= 0.9f && beat < 1.0f && ym >= walk + 0.5f && ym < walk + 1.5f ? 0 : m.planks;
                                    else if (ym < roofY - 0.25f) id = 0;
                                }
                                if (id != 255) {
                                    set(x, y, z, id);
                                    continue;
                                }
                                if (out > 0.0f) continue;
                            }
                            if (ym < walk) {
                                const float flare = Batter(ym, batterTop);
                                const bool band = ym >= walk - 0.25f || (ym >= batterTop && ym < batterTop + 0.25f);
                                float outer = r.half + std::max(flare, band ? 0.125f : 0.0f);
                                if (buttress && ym < walk - 2.0f) {
                                    const float depth = ym < batterTop ? 1.0f + flare : std::max(0.25f, 1.0f - 0.25f * std::floor((ym - batterTop) / 2.0f));
                                    outer = std::max(outer, r.half + depth);
                                }
                                if (s > outer) continue;
                                // Putlog holes (0.25 m square, every 2 m) just under the curtain's walk, for hoardings and
                                // scaffolds.
                                const float beat = t + 50.0f - 2.0f * std::floor((t + 50.0f) / 2.0f);
                                if (r.half > 1.2f && s > r.half - 0.25f && s <= r.half && ym >= walk - 0.5f && ym < walk - 0.25f && beat >= 1.0f &&
                                    beat < 1.25f) {
                                    set(x, y, z, 0);
                                    continue;
                                }
                                uint8_t id = m.rubbleCore;
                                // The repair patch: brick in the outer face, its lower edge ragged.
                                if (t >= r.patchLo && t <= r.patchHi && s > r.half - 0.75f && s <= r.half &&
                                    ym >= walk - 4.5f + 1.5f * Ragged(int(std::floor(t * 2.0f)), 3)) {
                                    set(x, y, z, m.brick);
                                    continue;
                                }
                                if (ym < batterTop || s > r.half)
                                    id = Damp(n.x * kShadeX + n.y * kShadeZ, outer - s, ym - g, int(std::floor(t * 2.0f))) ? m.graniteDamp : m.granite;
                                else if (s > r.half - 0.75f || s < -r.half + 0.75f || ym >= walk - 0.5f) id = m.ashlar;
                                set(x, y, z, id);
                            } else if (s >= r.half - 0.5f && s <= r.half && (ym < walk + kParapet || (ym < walk + kParapet + kMerlon && Merlon(t + 50.0f)))) {
                                // An arrow slit (one voxel wide) through every second merlon.
                                const float along = t + 50.0f, merlon = std::floor(along / 2.75f);
                                const bool slit = int(merlon) % 2 == 0 && std::abs(along - (merlon * 2.75f + 1.0f)) < 0.07f && ym >= walk + 0.5f &&
                                                  ym < walk + kParapet + kMerlon - 0.25f;
                                // A doorway through the parapet onto the hoarding every 4 m.
                                const float door = t + 50.0f - 4.0f * std::floor((t + 50.0f) / 4.0f);
                                const bool doorway = (hoard && door >= 1.75f && door < 2.5f && ym < walk + 2.0f) || (t >= r.patchLo + 0.5f && t <= r.patchHi - 0.5f);
                                set(x, y, z, slit || doorway ? 0 : m.ashlar);
                            }
                        }
                    }
                break;
            }

            case FortShape::Vault: {
                const float centre = r.id == 0 ? (r.a.y + r.b.y) * 0.5f : (r.a.x + r.b.x) * 0.5f;
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 p((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs);
                        if (p.x < r.a.x || p.x >= r.b.x || p.y < r.a.y || p.y >= r.b.y) continue;
                        const float lateral = (r.id == 0 ? p.y : p.x) - centre;
                        for (int y = lo.y; y < hi.y; ++y) {
                            const float rise = (float(y) + 0.5f) * vs - r.lo;
                            if (rise >= 0.0f && lateral * lateral + rise * rise < r.half * r.half) set(x, y, z, 0);
                        }
                    }
                break;
            }

            case FortShape::Tunnel: {
                const float len = Length(r.b - r.a);
                const glm::vec2 d = (r.b - r.a) / len, n(-d.y, d.x);
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 p((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs);
                        const float t = Dot(p - r.a, d), s = Dot(p - r.a, n);
                        if (t < 0.0f || t > len || std::abs(s) > r.half + 0.25f) continue;
                        const float floorY = r.lo + (r.hi - r.lo) * t / len;
                        const float g = ground[(x - c0.x) + (z - c0.z) * kChunkSize] * vs;
                        const bool capA = (r.caps & 1) != 0 && t < 0.25f, capB = (r.caps & 2) != 0 && t > len - 0.25f;
                        for (int y = lo.y; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs;
                            if (ym >= floorY + r.height + 0.25f) continue;
                            if (ym < floorY - 0.25f) {
                                if (ym >= g - 0.25f) set(x, y, z, m.ashlar);   // a footing where the ground lies lower
                                continue;
                            }
                            uint8_t id = t < 2.0f * r.grateTo ? m.granite : m.ashlar;   // the lining; a granite frame at the mouth
                            if (std::abs(s) < r.half && ym >= floorY && ym < floorY + r.height) {
                                id = 0;
                                if (t < r.grateTo && s + 50.0f - 0.25f * std::floor((s + 50.0f) / 0.25f) < 0.125f) id = m.iron;
                                else if (r.bulkheadAt >= 0.0f && t >= r.bulkheadAt && t < r.bulkheadAt + 0.25f) id = m.oakIron;
                                else if (capB || (capA && !(std::abs(s) < 0.5f && ym < floorY + 1.0f))) id = m.ashlar;
                            }
                            set(x, y, z, id);
                        }
                    }
                break;
            }

            case FortShape::Spiral:
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 q = glm::vec2((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs) - r.a;
                        const float r2 = Dot(q, q);
                        if (r2 >= 1.5625f) continue;
                        const bool shaft = r2 < 1.0f, newel = r2 < 0.04f;
                        const float base = shaft && !newel ? 0.25f * float(Sector(q) + 1) : 0.0f;
                        for (int y = lo.y; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs, rel = ym - r.lo;
                            if (rel < -0.25f || ym >= r.hi + 2.25f) continue;
                            if (!shaft) {
                                if (rel >= r.height && ym < r.hi) set(x, y, z, m.ashlar);   // the shaft's lining above the room
                                continue;
                            }
                            if (rel < 0.0f) set(x, y, z, m.ashlar);
                            else if (ym < r.hi && (newel || Step(rel, base))) set(x, y, z, m.ashlar);
                            else set(x, y, z, 0);
                        }
                    }
                break;

            case FortShape::Roof: {
                const glm::vec2 o = r.b, side(-o.y, o.x);
                const float shell = (std::ceil(r.height) + 1.0f) * vs;   // worldgen.md C5: rise per voxel of run, plus one
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 q = glm::vec2((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs) - r.a;
                        const float u = Dot(q, o), v = Dot(q, side), au = std::abs(u), av = std::abs(v);
                        float d = 0.0f;
                        bool rafter = false, tie = false, king = false, gableEnd = false;
                        if (r.roof == FortRoof::Gable) {
                            if (au > r.length + 0.25f) continue;
                            d = av;
                            const float beat = u + 50.0f - std::floor(u + 50.0f);
                            rafter = beat < 0.25f || d < 0.125f;
                            tie = u + 50.0f - 2.0f * std::floor((u + 50.0f) / 2.0f) < 0.25f;
                            king = tie && d < 0.125f;
                            gableEnd = au >= r.length - 0.25f && au <= r.length;
                        } else {
                            const bool square = r.block == FortBlock::Keep;
                            d = square || u <= 0.0f ? std::max(au, av) : std::max(std::max(u, av), (u + av) * 0.70710678f);
                            for (int k = 0; k < 12 && !rafter; ++k) {
                                const glm::vec2 dir(kTurn[k][0], kTurn[k][1]);
                                rafter = Dot(dir, q) > 0.0f && std::abs(Cross(dir, q)) < 0.125f;
                            }
                            tie = au < 0.125f || av < 0.125f;
                            king = Dot(q, q) < 0.0625f;
                        }
                        if (d > r.half) continue;
                        const float surface = r.lo + r.height * (r.half - d);
                        for (int y = lo.y; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs;
                            if (ym >= surface || ym < r.lo) continue;
                            if (ym >= surface - shell) set(x, y, z, r.id);
                            else if (gableEnd) set(x, y, z, m.ashlar);
                            else if (ym >= surface - shell - 0.25f && rafter) set(x, y, z, m.wood);
                            else if (ym < r.lo + 0.25f && tie && d < r.half - 0.25f) set(x, y, z, m.wood);
                            else if (king) set(x, y, z, m.wood);
                        }
                    }
                break;
            }

            case FortShape::Beam: {
                const float len = Length(r.b - r.a);
                const glm::vec2 d = (r.b - r.a) / len, n(-d.y, d.x);
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 p = glm::vec2((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs) - r.a;
                        const float t = Dot(p, d), s = Dot(p, n);
                        if (t < 0.0f || t > len || std::abs(s) > r.half) continue;
                        const float up = r.height * t / len;   // a sloped beam rises `height` from a to b
                        for (int y = lo.y; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs;
                            if (ym >= r.lo + up && ym < r.hi + up) set(x, y, z, r.id);
                        }
                    }
                break;
            }

            case FortShape::LeanTo: {
                const float len = Length(r.b - r.a);
                const glm::vec2 d = (r.b - r.a) / len, in = r.out;
                const float depth = r.half;
                const bool granary = r.building == FortBuilding::Granary;
                const bool hearth = r.building == FortBuilding::Forge || r.building == FortBuilding::Kitchen;
                const bool earth = r.building == FortBuilding::Stables || hearth;
                const float floorTop = r.lo + (granary ? 0.75f : 0.0f);
                const int storeys = int(r.height);
                const float shell = 2.0f * vs;   // 1 in 2: worldgen.md C5
                const int doorBay = int(std::floor(len / 4.0f));
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 p = glm::vec2((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs) - r.a;
                        const float t = Dot(p, d), w = Dot(p, in);
                        if (t < -0.25f || t > len + 0.25f || w < -0.25f || w > depth + 0.5f) continue;
                        const float g = ground[(x - c0.x) + (z - c0.z) * kChunkSize] * vs;
                        const float roofY = r.hi - 0.5f * std::max(0.0f, w);
                        const bool inside = t >= 0.0f && t <= len && w >= 0.0f && w <= depth;
                        const bool front = w >= depth - 0.25f, end = t < 0.25f || t > len - 0.25f;
                        const int bay = int(std::floor(t / 2.0f));
                        const float inBay = t - 2.0f * float(bay);
                        const bool post = inBay < 0.25f || t > len - 0.25f || w < 0.25f;
                        const bool chimney = hearth && std::abs(t - 1.0f) < 0.375f && std::abs(w - 0.75f) < 0.375f;
                        const bool flue = hearth && std::abs(t - 1.0f) < 0.125f && std::abs(w - 0.75f) < 0.125f;
                        const bool fire = hearth && std::abs(t - 1.0f) < 0.75f && w < 1.5f;
                        const bool stair = storeys > 1 && t > len - 4.25f && t < len - 0.25f && w >= 0.25f && w < 1.25f;
                        const bool staddle = granary && std::abs(inBay - 1.0f) < 0.25f && (std::abs(w - 0.5f) < 0.25f || std::abs(w - (depth - 0.5f)) < 0.25f);
                        const float top = chimney ? roofY + 1.5f : roofY;
                        const int y0 = std::max(lo.y, int(std::floor((std::min(g, r.lo) - 0.5f) / vs)));
                        for (int y = y0; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs;
                            if (ym >= top) break;
                            if (chimney && ym >= floorTop) {
                                set(x, y, z, flue && ym >= floorTop + 1.0f ? 0 : m.brick);
                                continue;
                            }
                            if (ym >= roofY - shell) {
                                set(x, y, z, r.id);   // the roof, overhanging 0.25 m at the ends and 0.5 m at the front
                                continue;
                            }
                            if (!inside) continue;
                            if (ym >= roofY - shell - 0.25f && t + 50.0f - std::floor(t + 50.0f) < 0.25f) {
                                set(x, y, z, m.wood);   // rafters every metre
                                continue;
                            }
                            if (ym < floorTop) {
                                if (granary) {
                                    if (ym >= g - 0.5f && staddle) set(x, y, z, m.ashlar);
                                    else if (ym >= floorTop - 0.25f) set(x, y, z, m.planks);
                                    continue;
                                }
                                if (ym < g - 0.5f) continue;
                                const bool rim = front || end;
                                set(x, y, z, ym >= floorTop - 0.25f && !rim ? (earth ? m.dirt : m.planks) : (rim ? m.ashlar : m.rubbleCore));
                                continue;
                            }
                            const float rel = ym - floorTop;
                            if (fire && rel < 0.75f) {
                                set(x, y, z, m.brick);   // the hearth (forge) or the oven (kitchen)
                                continue;
                            }
                            if (stair && rel < 3.25f && rel < (t - (len - 4.25f)) * 0.8125f) {
                                set(x, y, z, m.planks);   // a straight stair along the end wall to the upper floor
                                continue;
                            }
                            if (storeys > 1 && rel >= 3.0f && rel < 3.25f && !stair && !(front || end)) {
                                set(x, y, z, m.planks);   // the upper floor
                                continue;
                            }
                            if (storeys > 1 && rel >= 2.75f && rel < 3.0f && !stair && !(front || end) && t + 50.0f - std::floor(t + 50.0f) < 0.25f) {
                                set(x, y, z, m.wood);   // its joists
                                continue;
                            }
                            if (front || end) {
                                // The frame: posts every bay, the sill, a plate at each storey, a brace in the end bays.
                                const float storeyRel = rel - 3.0f * std::floor(rel / 3.0f);
                                const bool sill = rel < 0.25f, plate = storeyRel >= 2.75f;
                                const bool brace = front && (bay == 0 || t > len - 2.0f) && std::abs((t > len - 2.0f ? len - t : t) - storeyRel * 0.6667f) < 0.15f;
                                const bool framePost = front ? post : (w < 0.25f || w > depth - 0.25f);
                                // Openings: the door in the middle bay, windows in every other bay, on each storey.
                                const bool door = front && bay == doorBay && inBay >= (r.building == FortBuilding::Stables ? 0.25f : 0.5f) &&
                                                  inBay < (r.building == FortBuilding::Stables ? 1.75f : 1.5f) && rel < 2.25f && rel >= 0.25f;
                                const bool window = front && bay != doorBay && bay % 2 == 1 && inBay >= 0.625f && inBay < 1.375f && storeyRel >= 1.25f &&
                                                    storeyRel < 2.0f;
                                if (door || window) set(x, y, z, 0);
                                else if (framePost || sill || plate || brace) set(x, y, z, m.wood);
                                else set(x, y, z, m.wattleDaub);
                                continue;
                            }
                            set(x, y, z, 0);   // inside
                        }
                    }
                break;
            }

            case FortShape::Scaffold: {
                constexpr float kLift = 2.5f;
                const float len = Length(r.b - r.a);
                const glm::vec2 d = (r.b - r.a) / len, n = r.out;
                const float rise0 = r.height;   // the ground flight's rise to the lowest deck (0: the deck lies on the ground; < 0: none)
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 p = glm::vec2((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs) - r.a;
                        const float t = Dot(p, d), s = Dot(p, n);
                        if (t < -0.25f || t > len + 0.25f || s < 0.0f || s > 3.75f) continue;
                        const float g = ground[(x - c0.x) + (z - c0.z) * kChunkSize] * vs;
                        const float inBay = t + 50.0f - 2.0f * std::floor((t + 50.0f) / 2.0f);
                        const bool atBay = inBay < 0.25f || t > len - 0.25f;
                        const bool innerRow = s >= 1.25f && s < 1.5f, outerRow = s >= 3.5f && s < 3.75f;
                        const bool deckRow = s >= 1.25f && s < 3.75f, lane = s >= 1.75f && s < 2.5f;
                        // A flight's run from its foot: from a's end on even lifts, from b's on odd ones and the ground's.
                        const float runEven = t - 0.5f, runOdd = (len - 0.5f) - t;
                        // Under the top 2.5 m of the flight that comes up to a deck: the deck is open there, no putlog crosses.
                        const auto underFlight = [&](int deckLift) {
                            if (deckLift >= 1) {
                                const float run = (deckLift - 1) % 2 == 0 ? runEven : runOdd;
                                return run >= 0.0f && run < kLift;
                            }
                            return rise0 > 0.0f && runOdd >= 0.0f && runOdd >= rise0 - kLift && runOdd < rise0;
                        };
                        const int y0 = std::max(lo.y, int(std::floor((std::min(g, r.lo) - 0.25f) / vs)));
                        for (int y = y0; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs;
                            if (ym >= r.hi + 1.25f) break;
                            // Standards from the ground to 1 m over the top deck.
                            if ((innerRow || outerRow) && atBay) {
                                if (ym >= g - 0.25f) set(x, y, z, m.wood);
                                continue;
                            }
                            const int lift = int(std::floor((ym - r.lo) / kLift));   // the deck under this height (-1: below the lowest)
                            const float deck = r.lo + kLift * float(lift), rel = ym - deck;
                            const bool topDeck = deck >= r.hi - 0.01f;
                            if (lift < 0) {
                                if (rise0 == 0.0f) {
                                    if (deckRow && ym >= g - 0.25f) set(x, y, z, m.planks);   // the lowest deck stands on the ground
                                    continue;
                                }
                                // The ground flight, solid from the ground up to its step.
                                if (lane && runOdd >= 0.0f && runOdd < rise0 && ym < r.lo - rise0 + (std::floor(runOdd / 0.25f) + 1.0f) * 0.25f) {
                                    set(x, y, z, m.planks);
                                    continue;
                                }
                            }
                            if (topDeck) {
                                if (rel < 0.25f && !(lane && underFlight(lift))) set(x, y, z, m.planks);   // out to the face
                                else if (outerRow && rel >= 1.125f) set(x, y, z, m.wood);                    // the top rail
                                continue;
                            }
                            // The flight from this deck to the next: 0.25 m steps every 0.25 m.
                            const float run = lift % 2 == 0 ? runEven : runOdd;
                            if (lift >= 0 && lane && run >= 0.0f && run < kLift && rel < (std::floor(run / 0.25f) + 1.0f) * 0.25f) {
                                set(x, y, z, m.planks);
                                continue;
                            }
                            // The next deck's ledgers along both rows of standards and its putlogs at each bay.
                            if (rel >= kLift - 0.25f && (lift >= 0 || (lift == -1 && rise0 != 0.0f)) && ((innerRow || outerRow) || (atBay && !underFlight(lift + 1)))) {
                                set(x, y, z, m.wood);
                                continue;
                            }
                            if (lift >= 0 && rel < 0.25f && deckRow && !(lane && underFlight(lift))) set(x, y, z, m.planks);
                        }
                    }
                break;
            }

            case FortShape::Block: {
                const glm::vec2 o = r.b, side(-o.y, o.x);
                const bool isKeep = r.block == FortBlock::Keep;
                const float R = r.half;
                const float first = isKeep ? 5.0f : 3.0f, step = isKeep ? 6.0f : 3.0f;
                // The spiral stair: in the middle of a tower, in the keep's back corner away from the door.
                const glm::vec2 shaft = isKeep ? glm::vec2(-(R - 4.1f), -(R - 4.1f)) : glm::vec2(0.0f);
                const float towerShell = r.block == FortBlock::GateTower ? 1.0f : 1.5f;
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 p((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs);
                        const float u = Dot(p - r.a, o), v = Dot(p - r.a, side);
                        const float au = std::abs(u), av = std::abs(v);
                        const float square = std::max(au, av);
                        const float dist = isKeep || u <= 0.0f ? square : std::max(std::max(u, av), (u + av) * 0.70710678f);
                        if (dist > R + kBatterMax + 0.25f) continue;
                        const float g = ground[(x - c0.x) + (z - c0.z) * kChunkSize] * vs;
                        // The batter is a skirt at the block's foot: 3 m over the lower of the column's ground and the
                        // floor, so a high ground beside a tower does not raise it up to a wall walk's door.
                        const float batterTop = std::min(g, r.lo) + kBlockBatter;
                        // What depends on the column only: the stair, the corners, the openings' geometry.
                        const glm::vec2 q(u, v), sq = q - shaft;
                        const float sr2 = Dot(sq, sq);
                        const bool inShaft = sr2 < 1.0f, newel = sr2 < 0.04f;
                        const float stepBase = inShaft && !newel ? 0.25f * float(Sector(sq) + 1) : 0.0f;
                        const bool turret = isKeep && R - au < 2.5f && R - av < 2.5f;
                        const bool squareCorner = isKeep || u < 0.0f;
                        const float along = au >= av ? v : u;
                        const bool machi = (r.trim & FortTrim_Machicolations) != 0 && !turret;
                        const bool corbel = machi && (along + 50.0f) - 1.5f * std::floor((along + 50.0f) / 1.5f) < 0.75f;
                        const float parapetIn = machi ? R + 0.25f : R - 0.5f, parapetOut = machi ? R + 0.75f : R;
                        const bool joistColumn = u + 50.0f - std::floor(u + 50.0f) < 0.25f;
                        const bool frontDoor = av < (isKeep ? 1.0f : 0.5f) && (isKeep ? u > 0.0f : u < 0.0f);
                        const bool leftDoor = !isKeep && std::abs(u - r.walkLeftU) < 0.5f && v > 0.0f;
                        const bool rightDoor = !isKeep && std::abs(u - r.walkRightU) < 0.5f && v < 0.0f;
                        // Slits and windows through the shell: the lateral offset and depth from each face's direction.
                        float slitLateral[4], slitDepth[4];
                        int slits = 0;
                        const auto addSlit = [&](glm::vec2 dir) {
                            const float radial = Dot(q, dir);
                            if (radial <= 0.0f) return;
                            slitLateral[slits] = std::abs(Cross(dir, q));
                            slitDepth[slits] = R - radial;
                            ++slits;
                        };
                        float windowOff = 1.0e30f;   // the keep's upper storeys: distance from the nearest window's middle
                        float windowRadial = 0.0f;
                        if (isKeep) {
                            for (const glm::vec2 dir : {glm::vec2(1, 0), glm::vec2(-1, 0), glm::vec2(0, 1), glm::vec2(0, -1)}) {
                                const float radial = Dot(q, dir), off = std::abs(std::abs(Cross(dir, q)) - 2.5f);
                                if (radial > 0.0f && off < windowOff) {
                                    windowOff = off;
                                    windowRadial = radial;
                                }
                            }
                            for (const glm::vec2 dir : {glm::vec2(-1, 0), glm::vec2(0, 1), glm::vec2(0, -1), glm::vec2(1, 0)}) addSlit(dir);
                        } else {
                            for (const glm::vec2 dir : {glm::vec2(1, 0), glm::vec2(0.70710678f, 0.70710678f), glm::vec2(0.70710678f, -0.70710678f)}) addSlit(dir);
                        }
                        const int y0 = std::max(lo.y, int(std::floor((std::min(g, r.lo - 0.5f) - kFoundation) / vs)));
                        // An unfinished tower: its shell stops raggedly in its top 2 m; no roof slab, no crown.
                        const bool unfinished = (r.trim & FortTrim_Unfinished) != 0;
                        const float builtTo = unfinished ? r.hi - 2.0f + 1.75f * Ragged(x >> 2, z >> 2) : r.hi;
                        // How far this column's face turns from the sun (damp on the batter), once per column.
                        const glm::vec2 radial = o * u + side * v;
                        const float shadeFacing = (radial.x * kShadeX + radial.y * kShadeZ) / std::max(Length(radial), 1.0e-3f);
                        const int dampAlong = int(std::floor((u + v) * 2.0f));
                        for (int y = y0; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs;
                            // Inside, the top storey is open from 2 m under the top; the shell stops at its ragged height.
                            if (unfinished && (ym >= builtTo || (dist < R - towerShell && ym >= r.hi - 2.0f))) continue;
                            if (ym >= r.hi) {
                                const float crown = turret ? r.hi + 2.5f : r.hi;
                                if (turret && ym < crown && dist <= R) {
                                    set(x, y, z, m.ashlar);
                                } else if (dist <= parapetOut && dist >= parapetIn && ym >= crown &&
                                           (ym < crown + (turret ? 0.0f : kParapet) || (ym < crown + (turret ? 0.0f : kParapet) + kMerlon && Merlon(along + 50.0f)))) {
                                    set(x, y, z, m.ashlar);
                                }
                                continue;
                            }
                            const float rel = ym - r.lo - first;
                            // A landing at each wall walk; the storey floors keep out of the 2.5 m over it, so its door is clear.
                            const bool landing = (v > 0.0f && ym >= r.walkLeft - 0.25f && ym < r.walkLeft) || (v <= 0.0f && ym >= r.walkRight - 0.25f && ym < r.walkRight);
                            const bool overWalk = (ym >= r.walkLeft - 0.25f && ym < r.walkLeft + 2.5f) || (ym >= r.walkRight - 0.25f && ym < r.walkRight + 2.5f);
                            const bool floorRow = !overWalk && rel >= 0.0f && rel - step * std::floor(rel / step) < 0.25f;
                            const bool joistRow = !overWalk && joistColumn && rel + 0.25f >= 0.0f &&
                                                  (rel + 0.25f) - step * std::floor((rel + 0.25f) / step) < 0.25f;
                            const bool band = (rel >= 0.0f && rel - step * std::floor(rel / step) < 0.25f) || (isKeep && ym >= r.lo + 8.0f && ym < r.lo + 8.25f);
                            // The door: at the back of a tower (to the bailey), at the front of the keep (to the gate),
                            // through the shell and the batter in front of it.
                            if (frontDoor && ym >= r.lo && ym < r.lo + (isKeep ? 2.4f : 2.25f) && dist >= R - (isKeep ? 3.0f : towerShell)) {
                                set(x, y, z, 0);
                                continue;
                            }
                            // Machicolations: 3 stepped corbels (0.25, 0.5, 0.75 m out) in the top 1.125 m; nothing
                            // between them, so the gallery behind the parapet has drop holes.
                            if (corbel && dist > R && ym >= r.hi - 1.125f) {
                                const float out = ym < r.hi - 0.75f ? 0.25f : (ym < r.hi - 0.375f ? 0.5f : 0.75f);
                                if (dist <= R + out) {
                                    set(x, y, z, m.ashlar);
                                    continue;
                                }
                            }
                            const float outer = R + std::max(Batter(ym, batterTop), band && ym >= batterTop ? 0.125f : 0.0f);
                            if (dist > outer) continue;
                            const float shell = isKeep ? (ym < r.lo + 8.0f ? 3.0f : 2.0f) : towerShell;
                            if (dist < R - shell) {
                                uint8_t id = 0;
                                if (inShaft && ym >= r.lo) id = newel || Step(ym - r.lo, stepBase) ? m.ashlar : 0;
                                else if (ym < r.lo - 0.5f) id = m.rubbleCore;
                                else if (ym < r.lo) id = isKeep ? m.bedrock : m.ashlar;
                                else if (ym >= r.hi - 0.5f) id = m.ashlar;
                                else if (floorRow || landing) id = m.planks;
                                else if (joistRow) id = m.wood;
                                set(x, y, z, id);
                                continue;
                            }
                            // Doors from the wall walks into a flank tower, on each walk's centre line.
                            const bool walkDoor = dist <= R + 0.25f && ((leftDoor && ym >= r.walkLeft && ym < r.walkLeft + 2.25f) ||
                                                                (rightDoor && ym >= r.walkRight && ym < r.walkRight + 2.25f));
                            // Openings by storey: slits splaying inward; the keep's upper storeys have round-headed windows.
                            const float storeyFloor = ym < r.lo + first ? r.lo : r.lo + first + step * std::floor((ym - r.lo - first) / step) + 0.25f;
                            const float inStorey = ym - storeyFloor;
                            bool opening = false;
                            if (dist <= R && inStorey >= 0.75f && inStorey < 2.25f) {
                                if (isKeep && storeyFloor >= r.lo + 8.0f) {
                                    const float head = inStorey - 1.775f;
                                    opening = windowRadial > R - shell - 0.01f && windowOff < 0.375f && inStorey >= 0.9f &&
                                              (head < 0.0f || windowOff * windowOff + head * head < 0.140625f);
                                } else {
                                    // The keep's front slit only above the door's storey.
                                    const int count = isKeep && storeyFloor <= r.lo ? slits - (Dot(q, glm::vec2(1, 0)) > 0.0f ? 1 : 0) : slits;
                                    for (int i = 0; i < count && !opening; ++i) opening = Slit(slitLateral[i], slitDepth[i], shell);
                                    // Gate towers: cross slits, the cross bar 0.75 m wide at the slit's middle.
                                    if (r.block == FortBlock::GateTower && inStorey >= 1.375f && inStorey < 1.5f && u > 0.0f && av < 0.375f) opening = true;
                                }
                            }
                            if (walkDoor || opening) {
                                set(x, y, z, 0);
                                continue;
                            }
                            uint8_t id = m.ashlar;
                            if (ym < batterTop || dist > R || (isKeep && ym < r.lo + 8.0f)) {
                                id = Damp(shadeFacing, outer - dist, ym - g, dampAlong) ? m.graniteDamp : m.granite;
                            }
                            else if (squareCorner && Quoin(R - au, R - av, ym - r.lo)) id = m.graniteQuoin;
                            set(x, y, z, id);
                        }
                    }
                break;
            }
            }
        }
    }
}
