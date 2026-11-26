module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
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
        const auto walls = [&](const std::array<float, kSiteDirections>& radii, float half, float walk, glm::vec2 gapAt, float gap) {
            for (int k = 0; k < kSiteDirections; ++k) {
                const glm::vec2 a = vertex(radii, k), b = vertex(radii, k + 1);
                const float len = Length(b - a);
                if (len < 0.01f) continue;
                const glm::vec2 d = (b - a) / len;
                float runTop = -1.0e30f, runLow = 1.0e30f;
                const glm::vec2 n(d.y, -d.x);
                for (float t = 0.0f; t <= len + 0.25f; t += 0.5f) runTop = std::max(runTop, ground(a + d * std::min(t, len)));
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
                r.hi = runTop + walk;
                const float off = Cross(d, gapAt - a), along = Dot(gapAt - a, d);
                if (std::abs(off) < gap) {
                    const float h = std::sqrt(gap * gap - off * off);
                    r.gapLo = along - h;
                    r.gapHi = along + h;
                }
                const float reach = half + kBatterMax + 1.25f + vs;
                setBounds(r, glm::min(a, b) - reach, glm::max(a, b) + reach, runLow - kFoundation - 1.0f, r.hi + kParapet + kMerlon + vs);
                plan.records.push_back(r);
            }
        };
        walls(site.curtain, 1.25f, kCurtainWalk, gatePoint, 7.5f);
        walls(site.inner, 1.0f, kInnerWalk, plan.innerGate, 1.6f);

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
            block(FortBlock::Tower, c, out, 3.5f, ground(c - out * 2.0f), gHi + 14.0f);
        }
        for (const float side : {-1.0f, 1.0f}) {
            const glm::vec2 c = gatePoint + across * (10.0f * side) + axis * 1.0f;
            float gLo = 0.0f, gHi = 0.0f;
            groundRange(c - 3.0f, c + 3.0f, gLo, gHi);
            block(FortBlock::GateTower, c, axis, 3.0f, gateGround, std::max(gHi, gateGround) + 15.0f);
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
            // The parapet and merlons along all 4 edges.
            const float t0 = gateGround + 14.0f;
            for (int side = 0; side < 4; ++side) {
                const bool alongXEdge = side < 2;
                const float fixed = alongXEdge ? (side == 0 ? lo.y : hi.y - 0.5f) : (side == 2 ? lo.x : hi.x - 0.5f);
                const float from = alongXEdge ? lo.x : lo.y, to = alongXEdge ? hi.x : hi.y;
                const auto edge = [&](float a0, float a1, float y0, float y1) {
                    if (alongXEdge)
                        box(glm::vec3(a0, y0, fixed), glm::vec3(a1, y1, fixed + 0.5f), m.ashlar);
                    else
                        box(glm::vec3(fixed, y0, a0), glm::vec3(fixed + 0.5f, y1, a1), m.ashlar);
                };
                edge(from, to, t0, t0 + kParapet);
                for (float a = from; a < to; a += 2.75f) edge(a, std::min(to, a + 2.0f), t0 + kParapet, t0 + kParapet + kMerlon);
            }
            const glm::vec2 pass = glm::abs(across) * 2.0f + glm::abs(axis) * 5.0f;
            box(glm::vec3(gatePoint.x - pass.x, gateGround, gatePoint.y - pass.y), glm::vec3(gatePoint.x + pass.x, gateGround + 4.5f, gatePoint.y + pass.y), 0);
            const glm::vec2 room = glm::abs(across) * 5.0f + glm::abs(axis) * 3.0f;
            box(glm::vec3(gatePoint.x - room.x, gateGround + 5.0f, gatePoint.y - room.y), glm::vec3(gatePoint.x + room.x, gateGround + 13.0f, gatePoint.y + room.y), 0);
            box(glm::vec3(gatePoint.x - room.x, gateGround + 4.5f, gatePoint.y - room.y), glm::vec3(gatePoint.x + room.x, gateGround + 5.0f, gatePoint.y + room.y),
                m.planks);
            const glm::vec2 slot = gatePoint + axis * 2.5f;
            const glm::vec2 sext = glm::abs(across) * 2.25f + glm::abs(axis) * 0.2f;
            box(glm::vec3(slot.x - sext.x, gateGround + 4.5f, slot.y - sext.y), glm::vec3(slot.x + sext.x, gateGround + 8.5f, slot.y + sext.y), 0);
            // The lever's post, 6 m inside the gate beside the passage.
            const glm::vec2 post = gatePoint - axis * 6.0f + across * 3.0f;
            const float postGround = ground(post);
            column(post, 0.15f, postGround - 0.5f, postGround + 1.0f, m.wood);
        }

        // 6. The keep on its pad: 16 x 16 m, 22 m high, its door towards the gate.
        block(FortBlock::Keep, keep, axis, 8.0f, site.keepGround, site.keepGround + 22.0f);
        return plan;
    }

    uint64_t FortPlanHash(const FortPlan& plan) {
        uint64_t h = Mix(plan.placed ? 0x666f7274ull : 0ull);
        const auto put = [&](uint64_t v) { h = Mix(h ^ v) + 0x9e3779b97f4a7c15ull; };
        const auto putf = [&](float f) { put(std::bit_cast<uint32_t>(f)); };
        for (const FortRecord& r : plan.records) {
            put(uint64_t(r.shape) | uint64_t(r.block) << 8 | uint64_t(r.id) << 16);
            for (const float f : {r.a.x, r.a.y, r.b.x, r.b.y, r.lo, r.hi, r.half, r.gapLo, r.gapHi}) putf(f);
            put(uint64_t(uint32_t(r.min.x)) | uint64_t(uint32_t(r.min.y)) << 32);
            put(uint64_t(uint32_t(r.min.z)) | uint64_t(uint32_t(r.max.x)) << 32);
            put(uint64_t(uint32_t(r.max.y)) | uint64_t(uint32_t(r.max.z)) << 32);
        }
        return h;
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
                                               !(r.gapHi >= r.gapLo && tb > r.gapLo - 3.0f && tb < r.gapHi + 3.0f);
                        const int y0 = std::max(lo.y, int(std::floor((g - kFoundation) / vs)));
                        for (int y = y0; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs;
                            if (ym < r.hi) {
                                const float flare = Batter(ym, batterTop);
                                const bool band = ym >= r.hi - 0.25f || (ym >= batterTop && ym < batterTop + 0.25f);
                                float outer = r.half + std::max(flare, band ? 0.125f : 0.0f);
                                if (buttress && ym < r.hi - 2.0f) {
                                    const float depth = ym < batterTop ? 1.0f + flare : std::max(0.25f, 1.0f - 0.25f * std::floor((ym - batterTop) / 2.0f));
                                    outer = std::max(outer, r.half + depth);
                                }
                                if (s > outer) continue;
                                uint8_t id = m.rubbleCore;
                                if (ym < batterTop || s > r.half) id = m.granite;
                                else if (s > r.half - 0.75f || s < -r.half + 0.75f || ym >= r.hi - 0.5f) id = m.ashlar;
                                set(x, y, z, id);
                            } else if (s >= r.half - 0.5f && s <= r.half && (ym < r.hi + kParapet || (ym < r.hi + kParapet + kMerlon && Merlon(t + 50.0f)))) {
                                set(x, y, z, m.ashlar);
                            }
                        }
                    }
                break;
            }

            case FortShape::Block: {
                const glm::vec2 o = r.b, side(-o.y, o.x);
                const bool isKeep = r.block == FortBlock::Keep;
                const float R = r.half;
                const float first = isKeep ? 5.0f : 3.0f, step = isKeep ? 6.0f : 3.0f;
                for (int z = lo.z; z < hi.z; ++z)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::vec2 p((float(x) + 0.5f) * vs, (float(z) + 0.5f) * vs);
                        const float u = Dot(p - r.a, o), v = Dot(p - r.a, side);
                        const float au = std::abs(u), av = std::abs(v);
                        const float square = std::max(au, av);
                        const float dist = isKeep || u <= 0.0f ? square : std::max(std::max(u, av), (u + av) * 0.70710678f);
                        if (dist > R + kBatterMax + 0.25f) continue;
                        const float g = ground[(x - c0.x) + (z - c0.z) * kChunkSize] * vs;
                        const float batterTop = g + kBlockBatter;
                        // The keep's corner turrets rise 2.5 m over its top; square corners carry quoins.
                        const bool turret = isKeep && R - au < 2.5f && R - av < 2.5f;
                        const bool squareCorner = isKeep || u < 0.0f;
                        const float along = au >= av ? v : u;
                        const int y0 = std::max(lo.y, int(std::floor((std::min(g, r.lo - 0.5f) - kFoundation) / vs)));
                        for (int y = y0; y < hi.y; ++y) {
                            const float ym = (float(y) + 0.5f) * vs;
                            if (ym >= r.hi) {
                                const float crown = turret ? r.hi + 2.5f : r.hi;
                                if (turret && ym < crown && dist <= R) {
                                    set(x, y, z, m.ashlar);
                                } else if (dist <= R && dist >= R - 0.5f && ym >= crown &&
                                           (ym < crown + (turret ? 0.0f : kParapet) || (ym < crown + (turret ? 0.0f : kParapet) + kMerlon && Merlon(along + 50.0f)))) {
                                    set(x, y, z, m.ashlar);
                                }
                                continue;
                            }
                            const float rel = ym - r.lo - first;
                            const bool floorRow = rel >= 0.0f && rel - step * std::floor(rel / step) < 0.25f;
                            const bool band = floorRow || (isKeep && ym >= r.lo + 8.0f && ym < r.lo + 8.25f);
                            // The door: at the back of a tower (to the bailey), at the front of the keep (to the gate),
                            // through the shell and the batter in front of it.
                            const bool door = ym >= r.lo && ym < r.lo + (isKeep ? 2.4f : 2.25f) && av < (isKeep ? 1.0f : 0.5f) &&
                                              (isKeep ? u > 0.0f : u < 0.0f) && dist >= R - (isKeep ? 3.0f : 1.5f);
                            if (door) {
                                set(x, y, z, 0);
                                continue;
                            }
                            const float outer = R + std::max(Batter(ym, batterTop), band && ym >= batterTop ? 0.125f : 0.0f);
                            if (dist > outer) continue;
                            const float shell = isKeep ? (ym < r.lo + 8.0f ? 3.0f : 2.0f) : 1.5f;
                            if (dist < R - shell) {
                                uint8_t id = 0;
                                if (ym < r.lo - 0.5f) id = m.rubbleCore;
                                else if (ym < r.lo) id = isKeep ? m.bedrock : m.ashlar;
                                else if (ym >= r.hi - 0.5f) id = m.ashlar;
                                else if (floorRow) id = m.planks;
                                set(x, y, z, id);
                                continue;
                            }
                            uint8_t id = m.ashlar;
                            if (ym < batterTop || dist > R || (isKeep && ym < r.lo + 8.0f)) id = m.granite;
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
