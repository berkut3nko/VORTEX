module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

module vortex.voxel;

import vortex.core;

// The starter region in detail (engine/docs/design/land.md L6-L8, [land-4]): the fine grid with strata cliffs,
// Hollowkeep's site, the points of interest, the cave and the roads. Like the coarse bake it is deterministic on every
// machine: one thread, a fixed order, ties broken by cell index, and only +, -, *, /, sqrt, floor and comparisons on
// floats.

namespace vortex::voxel {

    namespace {
        constexpr float kEdgeFade = 16.0f;          // the fine grid's correction fades to 0 over its outer metres

        /// Smooth value noise in [0, 1) from integer hashes (no libm); x and z in lattice steps.
        float HashNoise(uint32_t seed, float x, float z) {
            const float x0 = std::floor(x), z0 = std::floor(z);
            const int ix = int(x0), iz = int(z0);
            float fx = x - x0, fz = z - z0;
            fx = fx * fx * (3.0f - 2.0f * fx);
            fz = fz * fz * (3.0f - 2.0f * fz);
            auto at = [&](int a, int b) { return float(Hash3(a, b, 0x517e, seed) >> 8) * (1.0f / 16777216.0f); };
            const float top = at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * fx;
            const float bottom = at(ix, iz + 1) + (at(ix + 1, iz + 1) - at(ix, iz + 1)) * fx;
            return top + (bottom - top) * fz;
        }

        float SmoothStep(float a, float b, float x) {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        /// Strata terraces: in each band of `band` metres, 60 % of the height range rises gently (a bench), the last
        /// 40 % steeply (a cliff).
        float Terrace(float h, float band) {
            const float b = std::floor(h / band);
            const float t = h / band - b;
            const float shaped = t < 0.6f ? t * (0.25f / 0.6f) : 0.25f + (t - 0.6f) * (0.75f / 0.4f);
            return (b + shaped) * band;
        }

        struct Fine {
            const LandMap& m;
            LandFine& f;
            std::vector<float> h;        // the fine height being built
            std::vector<uint8_t> locked; // the site's core: no road stamping into the castle, no strata

            uint32_t Coarse(float mx, float mz) const {
                const int x = std::clamp(int(std::floor(mx / m.cellSize)), 0, m.width - 1);
                const int z = std::clamp(int(std::floor(mz / m.cellSize)), 0, m.height - 1);
                return m.Index(x, z);
            }
            float Mx(int i) const { return float(f.x0 + i) + 0.5f; }
            float Mz(int j) const { return float(f.z0 + j) + 0.5f; }
            bool Inside(float mx, float mz, float border) const {
                return mx >= float(f.x0) + border && mz >= float(f.z0) + border && mx < float(f.x0 + f.width) - border &&
                       mz < float(f.z0 + f.height) - border;
            }
            /// Height of the fine grid at map metres (nearest cell).
            float At(float mx, float mz) const {
                const int i = std::clamp(int(std::floor(mx - float(f.x0))), 0, f.width - 1);
                const int j = std::clamp(int(std::floor(mz - float(f.z0))), 0, f.height - 1);
                return h[f.Index(i, j)];
            }
            bool Wet(float mx, float mz) const { return m.waterKind[Coarse(mx, mz)] != WaterKind::None; }
        };

        /// Coarse distance (metres, 8-neighbour steps) to the nearest cell that `source` accepts.
        template <class Source>
        std::vector<float> CoarseDistance(const LandMap& m, Source source) {
            const uint32_t n = uint32_t(m.heights.size());
            std::vector<float> d(n, 1.0e30f);
            using Item = std::pair<float, uint32_t>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
            for (uint32_t i = 0; i < n; ++i)
                if (source(i)) {
                    d[i] = 0.0f;
                    open.push({0.0f, i});
                }
            while (!open.empty()) {
                const auto [dc, c] = open.top();
                open.pop();
                if (dc > d[c]) continue;
                const int cx = int(c % uint32_t(m.width)), cz = int(c / uint32_t(m.width));
                for (int k = 0; k < 8; ++k) {
                    static constexpr int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                    const int nx = cx + dx[k], nz = cz + dz[k];
                    if (nx < 0 || nz < 0 || nx >= m.width || nz >= m.height) continue;
                    const uint32_t ni = m.Index(nx, nz);
                    const float nd = dc + m.cellSize * (k < 4 ? 1.0f : 1.41421356f);
                    if (nd < d[ni]) {
                        d[ni] = nd;
                        open.push({nd, ni});
                    }
                }
            }
            return d;
        }

        /// The curtain's radius towards a point (the nearest direction's), for masks.
        float CurtainToward(const LandSite& site, float mx, float mz) {
            return site.curtain[size_t(LandSiteNearestDirection(mx - site.x, mz - site.z))];
        }

        /// Inside the curtain wall plus `margin` metres.
        bool InsideCurtain(const LandSite& site, float mx, float mz, float margin) {
            if (!site.placed) return false;
            const float d = std::sqrt((mx - site.x) * (mx - site.x) + (mz - site.z) * (mz - site.z));
            return d <= CurtainToward(site, mx, mz) + margin;
        }

        /// Where the castle's road ends: at the far end of the bridge over the ditch, on the gate's axis.
        void Approach(const LandSite& site, float& ax, float& az) {
            const glm::vec2 d = LandSiteDirection(site.gate), axis = LandSiteGateAxis(site);
            const float r = site.curtain[size_t(site.gate)], span = LandSiteBridgeSpan(site);
            ax = site.x + d.x * r + axis.x * span;
            az = site.z + d.y * r + axis.y * span;
        }

        /// What a keep point offers (land.md "The starter region"): in how many of the 24 directions the ground 50 m out lies
        /// 4 m or more below it (a spur or a knoll falls away on most sides), the direction of the highest ground there
        /// (the neck), and how far the ground within 6 m departs from it (the keep pad's grading).
        struct SpurShape {
            int fall = 0;
            int neck = 0;
            float neckHeight = -1.0e30f;
            float rough = 0.0f;
        };

        SpurShape ShapeAt(const Fine& g, float x, float z) {
            SpurShape s;
            const float g0 = g.At(x, z);
            for (int k = 0; k < kSiteDirections; ++k) {
                const glm::vec2 d = LandSiteDirection(k);
                const float h = g.At(x + d.x * 50.0f, z + d.y * 50.0f);
                if (h < g0 - 4.0f) ++s.fall;
                if (h > s.neckHeight) {
                    s.neckHeight = h;
                    s.neck = k;
                }
            }
            for (int k = 0; k < kSiteDirections; k += 3) {
                const glm::vec2 d = LandSiteDirection(k);
                s.rough = std::max(s.rough, std::abs(g.At(x + d.x * 6.0f, z + d.y * 6.0f) - g0));
            }
            return s;
        }

        /// Micro-grading (the owner's rule 5): a level pad of half sizes (hx, hz) around (cx, cz), blended outward over the
        /// angle of repose (1.5 m of run per metre of rise, plus 1 m) with a noisy edge; the pad itself is locked.
        void GradePad(Fine& g, float cx, float cz, float hx, float hz, float level) {
            const LandFine& f = g.f;
            const float reach = 16.0f;
            const int i0 = std::max(0, int(std::floor(cx - hx - reach - float(f.x0)))), i1 = std::min(f.width - 1, int(std::ceil(cx + hx + reach - float(f.x0))));
            const int j0 = std::max(0, int(std::floor(cz - hz - reach - float(f.z0)))), j1 = std::min(f.height - 1, int(std::ceil(cz + hz + reach - float(f.z0))));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    const uint32_t c = f.Index(i, j);
                    const float mx = g.Mx(i), mz = g.Mz(j);
                    const float ox = std::max(0.0f, std::abs(mx - cx) - hx), oz = std::max(0.0f, std::abs(mz - cz) - hz);
                    const float out = std::sqrt(ox * ox + oz * oz);
                    if (out <= 0.0f) {
                        g.h[c] = level;
                        g.f.surface[c] = LandSurface::Site;
                        g.f.detail[c] = 0;
                        g.locked[c] = 1;
                        continue;
                    }
                    const float edge = out + (HashNoise(0x7061u, mx / 5.0f, mz / 5.0f) - 0.5f) * 2.0f;
                    const float run = 1.5f * std::abs(g.h[c] - level) + 1.0f;
                    if (edge >= run) continue;
                    const float w = 1.0f - SmoothStep(0.0f, run, std::max(0.0f, edge));
                    g.h[c] += (level - g.h[c]) * w;
                    g.f.detail[c] = uint8_t(std::min(float(g.f.detail[c]), (1.0f - w) * 255.0f));
                }
        }

        void PlaceSite(const LandSource& source, Fine& g, LandMap& m, float starterMean) {
            const LandPoint* fromFile = nullptr;
            for (const LandPoint& p : source.points)
                if (p.kind == LandPointKind::Castle) fromFile = &p;
            const uint8_t starter = uint8_t(m.starterRegion);
            const std::vector<float> toStarter = CoarseDistance(m, [&](uint32_t i) { return m.regions[i] == starter; });
            LandSite best;
            if (fromFile) {
                best.x = fromFile->x;
                best.z = fromFile->z;
                best.placed = g.Inside(best.x, best.z, 60.0f);
                best.gate = ShapeAt(g, best.x, best.z).neck;
            } else {
                // A spur or a knoll over the starter region (the owner's answer 9 and rules of 2026-11-25): on mountain
                // ground first (any ground outside the starter region when no mountain spot fits); the ground 50 m out falls
                // 4 m or more in at least 14 of 24 directions; the keep's ground within 6 m departs at most 3 m (micro-grading
                // only); the keep 10-60 m above the starter region's mean and at most 85 m; dry ground within 46 m; the side
                // away from the neck within 150 m of the starter region. Best: the most fall, the least grading, nearer.
                for (int pass = 0; pass < 2 && !best.placed; ++pass)
                    for (int j = 0; j < g.f.height; j += 8)
                        for (int i = 0; i < g.f.width; i += 8) {
                            const float x = g.Mx(i), z = g.Mz(j);
                            if (!g.Inside(x, z, 60.0f)) continue;
                            const uint32_t centre = g.Coarse(x, z);
                            if (m.regions[centre] == starter) continue;
                            if (pass == 0 && m.biomes[centre] != uint8_t(LandBiome::Mountain)) continue;
                            const float g0 = g.At(x, z);
                            if (g0 < starterMean + 10.0f || g0 > std::min(starterMean + 60.0f, 85.0f)) continue;
                            const SpurShape s = ShapeAt(g, x, z);
                            if (s.fall < 14 || s.rough > 3.0f) continue;
                            const glm::vec2 nd = LandSiteDirection(s.neck);
                            const float tx = x - nd.x * 50.0f, tz = z - nd.y * 50.0f;
                            const float near = toStarter[g.Coarse(tx, tz)];
                            if (near > 150.0f) continue;
                            bool dry = true;
                            for (int k = 0; k < kSiteDirections && dry; k += 2) {
                                const glm::vec2 d = LandSiteDirection(k);
                                dry = !g.Wet(x + d.x * 46.0f, z + d.y * 46.0f) && !g.Wet(x + d.x * 23.0f, z + d.y * 23.0f);
                            }
                            if (!dry) continue;
                            const float cost = -float(s.fall) + 1.5f * s.rough + near / 100.0f;
                            if (!best.placed || cost < best.searchCost) {
                                best = LandSite{};
                                best.placed = true;
                                best.x = x;
                                best.z = z;
                                best.gate = s.neck;
                                best.searchCost = cost;
                            }
                        }
            }
            if (!best.placed) {
                m.site = best;
                return;
            }
            // The walls trace contours: march out from the keep until the ground lies 7 m (curtain) or 3 m (inner wall)
            // below it; smooth the radii round the circle twice.
            const float g0 = g.At(best.x, best.z);
            best.keepGround = g0;
            for (int k = 0; k < kSiteDirections; ++k) {
                const glm::vec2 d = LandSiteDirection(k);
                float curtain = 34.0f, inner = 0.0f;
                for (float r = 10.0f; r <= 46.0f; r += 1.0f) {
                    const float h = g.At(best.x + d.x * r, best.z + d.y * r);
                    if (inner == 0.0f && h < g0 - 3.0f) inner = r - 1.0f;
                    if (h < g0 - 7.0f) {
                        curtain = r - 2.0f;
                        break;
                    }
                }
                best.curtain[size_t(k)] = std::clamp(curtain, 16.0f, 42.0f);
                best.inner[size_t(k)] = inner == 0.0f ? 0.6f * best.curtain[size_t(k)] : inner;
            }
            for (int pass = 0; pass < 2; ++pass) {
                std::array<float, kSiteDirections> c = best.curtain, n = best.inner;
                for (int k = 0; k < kSiteDirections; ++k) {
                    const size_t a = size_t((k + kSiteDirections - 1) % kSiteDirections), b = size_t((k + 1) % kSiteDirections);
                    c[size_t(k)] = 0.25f * best.curtain[a] + 0.5f * best.curtain[size_t(k)] + 0.25f * best.curtain[b];
                    n[size_t(k)] = 0.25f * best.inner[a] + 0.5f * best.inner[size_t(k)] + 0.25f * best.inner[b];
                }
                best.curtain = c;
                best.inner = n;
            }
            for (int k = 0; k < kSiteDirections; ++k)
                best.inner[size_t(k)] = std::clamp(best.inner[size_t(k)], 11.0f, best.curtain[size_t(k)] - 7.0f);
            m.site = best;

            // Micro-grading only: the keep's pad (18 x 18 m) and the gatehouse's (14 x 10 m, across the gate).
            GradePad(g, best.x, best.z, 9.0f, 9.0f, g0);
            const glm::vec2 gd = LandSiteDirection(best.gate);
            const float gx = best.x + gd.x * best.curtain[size_t(best.gate)], gz = best.z + gd.y * best.curtain[size_t(best.gate)];
            const bool alongX = std::abs(gd.x) >= std::abs(gd.y);
            m.site.gateGround = g.At(gx, gz);
            GradePad(g, gx, gz, alongX ? 5.0f : 7.0f, alongX ? 7.0f : 5.0f, m.site.gateGround);
            // No plants inside the walls: the castle's ground keeps its height and loses its trees and rocks.
            for (int j = 0; j < g.f.height; ++j)
                for (int i = 0; i < g.f.width; ++i) {
                    const uint32_t c = g.f.Index(i, j);
                    if (g.f.surface[c] == LandSurface::Natural && InsideCurtain(best, g.Mx(i), g.Mz(j), 3.0f)) g.f.surface[c] = LandSurface::Site;
                }
        }

        /// Coarse distance (metres, 8-neighbour steps) to the nearest river or lake cell.
        std::vector<float> DistanceToFreshWater(const LandMap& m) {
            return CoarseDistance(m, [&](uint32_t i) { return m.waterKind[i] == WaterKind::River || m.waterKind[i] == WaterKind::Lake; });
        }

        float Distance(float ax, float az, float bx, float bz) { return std::sqrt((ax - bx) * (ax - bx) + (az - bz) * (az - bz)); }

        /// The largest height difference to the cells `r` metres away along the axes (0 = level).
        float Roughness(const Fine& g, float mx, float mz, float r) {
            const float c = g.At(mx, mz);
            return std::max(std::max(std::abs(g.At(mx + r, mz) - c), std::abs(g.At(mx - r, mz) - c)),
                            std::max(std::abs(g.At(mx, mz + r) - c), std::abs(g.At(mx, mz - r) - c)));
        }

        bool CaveFits(const Fine& g, float mx, float mz, LandCave& cave) {
            // Uphill from the mouth, level-ish into the hill: 26 m of tunnel falling 2 m, then the chamber.
            const float gx = g.At(mx + 2.0f, mz) - g.At(mx - 2.0f, mz), gz = g.At(mx, mz + 2.0f) - g.At(mx, mz - 2.0f);
            const float len = std::sqrt(gx * gx + gz * gz);
            if (len < 1.0e-3f) return false;
            const float ux = gx / len, uz = gz / len;
            cave.x[0] = mx;
            cave.z[0] = mz;
            cave.y[0] = g.At(mx, mz) + cave.radius * 0.6f;
            cave.x[1] = mx + ux * 26.0f;
            cave.z[1] = mz + uz * 26.0f;
            cave.y[1] = cave.y[0] - 2.0f;
            if (!g.Inside(cave.x[1], cave.z[1], 12.0f)) return false;
            // Rock over the chamber and over the middle of the tunnel.
            if (g.At(cave.x[1], cave.z[1]) < cave.y[1] + cave.chamberRadius + 3.0f) return false;
            const float midX = 0.5f * (cave.x[0] + cave.x[1]), midZ = 0.5f * (cave.z[0] + cave.z[1]);
            if (g.At(midX, midZ) < 0.5f * (cave.y[0] + cave.y[1]) + cave.radius + 2.5f) return false;
            for (int k = 0; k <= 4; ++k) {
                const float t = float(k) * 0.25f;
                if (g.Wet(cave.x[0] + (cave.x[1] - cave.x[0]) * t, cave.z[0] + (cave.z[1] - cave.z[0]) * t)) return false;
            }
            cave.placed = true;
            return true;
        }

        void ScorePoints(const LandSource& source, Fine& g, LandMap& m) {
            std::vector<LandPoint>& pts = m.points;
            auto fromFile = [&](LandPointKind kind) -> const LandPoint* {
                for (const LandPoint& p : source.points)
                    if (p.kind == kind) return &p;
                return nullptr;
            };
            float fieldX = 0.0f, fieldZ = 0.0f;
            if (m.site.placed) {
                pts.push_back({LandPointKind::Castle, m.site.x, m.site.z, fromFile(LandPointKind::Castle) != nullptr});
                Approach(m.site, fieldX, fieldZ);
            }
            auto inSite = [&](float mx, float mz) { return InsideCurtain(m.site, mx, mz, 20.0f); };
            const std::vector<float> fresh = DistanceToFreshWater(m);
            const uint8_t starter = uint8_t(m.starterRegion);

            // Village: level, dry ground of the starter region, near fresh water, 120-400 m from the castle's field.
            LandPoint village{LandPointKind::Village, 0.0f, 0.0f, false};
            if (const LandPoint* p = fromFile(LandPointKind::Village)) village = *p;
            else {
                float best = 1.0e30f;
                for (int j = 20; j < g.f.height - 20; j += 4)
                    for (int i = 20; i < g.f.width - 20; i += 4) {
                        const float mx = g.Mx(i), mz = g.Mz(j);
                        const uint32_t c = g.Coarse(mx, mz);
                        if (m.regions[c] != starter || m.waterKind[c] != WaterKind::None || inSite(mx, mz)) continue;
                        const float rough = Roughness(g, mx, mz, 6.0f);
                        if (rough > 1.0f) continue;
                        const float toCastle = m.site.placed ? Distance(mx, mz, fieldX, fieldZ) : 250.0f;
                        if (toCastle < 120.0f || toCastle > 400.0f) continue;
                        const float score = rough * 2.0f + std::abs(std::min(fresh[c], 300.0f) - 40.0f) / 40.0f +
                                            std::abs(toCastle - 220.0f) / 150.0f;
                        if (score < best) {
                            best = score;
                            village.x = mx;
                            village.z = mz;
                        }
                    }
                if (best >= 1.0e30f) village.x = -1.0f;
            }
            if (village.x >= 0.0f) pts.push_back(village);
            const bool haveVillage = village.x >= 0.0f;

            // Landing: dry starter-region ground at the sea, level, nearest the village.
            LandPoint landing{LandPointKind::Landing, -1.0f, 0.0f, false};
            if (const LandPoint* p = fromFile(LandPointKind::Landing)) landing = *p;
            else if (haveVillage) {
                float best = 1.0e30f;
                for (int j = 20; j < g.f.height - 20; j += 2)
                    for (int i = 20; i < g.f.width - 20; i += 2) {
                        const float mx = g.Mx(i), mz = g.Mz(j);
                        const uint32_t c = g.Coarse(mx, mz);
                        if (m.regions[c] != starter || m.waterKind[c] != WaterKind::None) continue;
                        bool sea = false;
                        for (float d : {-6.0f, 6.0f}) sea = sea || m.waterKind[g.Coarse(mx + d, mz)] == WaterKind::Sea ||
                                                             m.waterKind[g.Coarse(mx, mz + d)] == WaterKind::Sea;
                        if (!sea || g.At(mx, mz) < 0.5f) continue;
                        const float score = Distance(mx, mz, village.x, village.z) + Roughness(g, mx, mz, 3.0f) * 40.0f;
                        if (score < best) {
                            best = score;
                            landing.x = mx;
                            landing.z = mz;
                        }
                    }
            }
            if (landing.x >= 0.0f) pts.push_back(landing);

            // Cave: a steep, dry slope of firm ground outside the starter region, 120-450 m from the village, with rock over
            // its tunnel and chamber; harder rock and nearer the village first.
            LandCave cave;
            if (const LandPoint* p = fromFile(LandPointKind::Cave)) {
                CaveFits(g, p->x, p->z, cave);
                cave.x[0] = p->x;
                cave.z[0] = p->z;
            } else if (haveVillage) {
                float best = 1.0e30f;
                for (int j = 20; j < g.f.height - 20; j += 4)
                    for (int i = 20; i < g.f.width - 20; i += 4) {
                        const float mx = g.Mx(i), mz = g.Mz(j);
                        const uint32_t c = g.Coarse(mx, mz);
                        if (m.regions[c] == starter || m.waterKind[c] != WaterKind::None || inSite(mx, mz)) continue;
                        const float toVillage = Distance(mx, mz, village.x, village.z);
                        if (toVillage < 120.0f || toVillage > 450.0f) continue;
                        const float slope = Roughness(g, mx, mz, 2.0f) / 2.0f;
                        if (slope < 0.5f || slope > 1.4f) continue;
                        const float score = toVillage / 100.0f - float(m.hardness[c]) * 0.3f;
                        if (score >= best) continue;
                        LandCave trial;
                        if (!CaveFits(g, mx, mz, trial)) continue;
                        best = score;
                        cave = trial;
                    }
            }
            if (cave.placed || fromFile(LandPointKind::Cave)) {
                m.cave = cave;
                pts.push_back({LandPointKind::Cave, cave.x[0], cave.z[0], fromFile(LandPointKind::Cave) != nullptr});
            }

            // Landmark: the highest dry ground of the fine grid outside the site.
            LandPoint landmark{LandPointKind::Landmark, -1.0f, 0.0f, false};
            if (const LandPoint* p = fromFile(LandPointKind::Landmark)) landmark = *p;
            else {
                float highest = -1.0e30f;
                for (int j = 20; j < g.f.height - 20; j += 2)
                    for (int i = 20; i < g.f.width - 20; i += 2) {
                        const float mx = g.Mx(i), mz = g.Mz(j);
                        if (g.Wet(mx, mz) || inSite(mx, mz)) continue;
                        if (g.At(mx, mz) > highest) {
                            highest = g.At(mx, mz);
                            landmark.x = mx;
                            landmark.z = mz;
                        }
                    }
            }
            if (landmark.x >= 0.0f) pts.push_back(landmark);

        }

        /// A* on the fine grid from `a` to `b` (map metres), so that routes look like people made them, not a ruler:
        /// - a step costs its length times the ground's cost (1 to 2.1: slow noise of 45 m and 15 m, better and worse
        ///   ground) or a third of a metre on a road already made (later roads join and branch off earlier ones);
        /// - plus 30 grade^2 up to maxGrade and 400 per unit of grade above it (switchbacks win where the slope allows);
        /// - plus 12 side-slope^2 (roads follow the contours, not the fall line);
        /// - sea, lakes and the castle are closed; a river step costs 6 m more (a ford).
        bool FindRoad(const Fine& g, const LandMap& m, float ax, float az, float bx, float bz, float maxGrade,
                      const std::vector<uint8_t>& onRoad, std::vector<uint32_t>& path) {
            const LandFine& f = g.f;
            const uint32_t n = uint32_t(f.width) * uint32_t(f.height);
            auto cellOf = [&](float mx, float mz) {
                const int i = std::clamp(int(std::floor(mx - float(f.x0))), 2, f.width - 3);
                const int j = std::clamp(int(std::floor(mz - float(f.z0))), 2, f.height - 3);
                return f.Index(i, j);
            };
            const uint32_t start = cellOf(ax, az), goal = cellOf(bx, bz);
            auto closed = [&](uint32_t c) {
                const float mx = g.Mx(int(c % uint32_t(f.width))), mz = g.Mz(int(c / uint32_t(f.width)));
                const WaterKind w = m.waterKind[g.Coarse(mx, mz)];
                if (w == WaterKind::Sea || w == WaterKind::Lake) return true;
                return InsideCurtain(m.site, mx, mz, 3.0f);
            };
            std::vector<float> cost(n, 1.0e30f);
            std::vector<uint32_t> from(n, UINT32_MAX);
            using Item = std::pair<float, uint32_t>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
            const float gx = g.Mx(int(goal % uint32_t(f.width))), gz = g.Mz(int(goal / uint32_t(f.width)));
            cost[start] = 0.0f;
            constexpr float kLeast = 0.35f;   // the cheapest metre (a road already made): keeps the heuristic admissible
            open.push({kLeast * Distance(g.Mx(int(start % uint32_t(f.width))), g.Mz(int(start / uint32_t(f.width))), gx, gz), start});
            static constexpr int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
            while (!open.empty()) {
                const auto [fc, c] = open.top();
                open.pop();
                if (c == goal) break;
                const int ci = int(c % uint32_t(f.width)), cj = int(c / uint32_t(f.width));
                if (fc > cost[c] + kLeast * Distance(g.Mx(ci), g.Mz(cj), gx, gz) + 1.0e-3f) continue;
                for (int k = 0; k < 8; ++k) {
                    const int ni = ci + dx[k], nj = cj + dz[k];
                    if (ni < 2 || nj < 2 || ni >= f.width - 2 || nj >= f.height - 2) continue;
                    const uint32_t nc = f.Index(ni, nj);
                    if (closed(nc) && nc != goal) continue;
                    const float step = k < 4 ? 1.0f : 1.41421356f;
                    const float grade = std::abs(g.h[nc] - g.h[c]) / step;
                    const float within = std::min(grade, maxGrade);
                    const float mx = g.Mx(ni), mz = g.Mz(nj);
                    const float ground = onRoad[nc] ? kLeast
                                                    : 1.0f + 0.7f * HashNoise(0x524fu, mx / 45.0f, mz / 45.0f) +
                                                          0.4f * HashNoise(0x5250u, mx / 15.0f, mz / 15.0f);
                    const float sx = 0.5f * (g.h[f.Index(ni + 1, nj)] - g.h[f.Index(ni - 1, nj)]);
                    const float sz = 0.5f * (g.h[f.Index(ni, nj + 1)] - g.h[f.Index(ni, nj - 1)]);
                    const float side = std::abs(-sx * float(dz[k]) + sz * float(dx[k])) / step;
                    float add = step * (ground + 30.0f * within * within + 400.0f * (grade - within) + 12.0f * side * side);
                    if (m.waterKind[g.Coarse(g.Mx(ni), g.Mz(nj))] == WaterKind::River) add += 6.0f * step;
                    const float nc2 = cost[c] + add;
                    if (nc2 < cost[nc]) {
                        cost[nc] = nc2;
                        from[nc] = c;
                        open.push({nc2 + kLeast * Distance(mx, mz, gx, gz), nc});
                    }
                }
            }
            if (from[goal] == UINT32_MAX && goal != start) return false;
            path.clear();
            for (uint32_t c = goal; c != start; c = from[c]) path.push_back(c);
            path.push_back(start);
            std::reverse(path.begin(), path.end());
            return true;
        }

        void BuildRoads(const LandSettings& st, Fine& g, LandMap& m) {
            int village = -1;
            for (size_t k = 0; k < m.points.size(); ++k)
                if (m.points[k].kind == LandPointKind::Village) village = int(k);
            if (village < 0) return;
            const LandFine& f = g.f;
            float fieldX = 0.0f, fieldZ = 0.0f;
            if (m.site.placed) Approach(m.site, fieldX, fieldZ);
            const size_t pointCount = m.points.size();
            std::vector<uint8_t> onRoad(size_t(f.width) * size_t(f.height), 0);
            // The routes already laid, per fine cell within 2 cells of one: its nearest point (route, index). A later
            // road or trail takes that point's bed where it runs on or beside an earlier route, so junctions and shared
            // stretches have one height.
            std::vector<int32_t> laidRoute(size_t(f.width) * size_t(f.height), -1);
            std::vector<uint32_t> laidPoint(laidRoute.size(), 0);
            std::vector<float> laidDistance(laidRoute.size(), 1.0e30f);
            // Roads first (the castle, the landing), then trails (the cave, the landmark), which may follow the roads.
            for (int pass = 0; pass < 2; ++pass)
            for (size_t k = 0; k < pointCount; ++k) {
                const LandPoint& p = m.points[k];
                const bool trail = p.kind == LandPointKind::Cave || p.kind == LandPointKind::Landmark;
                const bool toCastle = p.kind == LandPointKind::Castle;   // read now: a crossing added below moves m.points
                if (pass != int(trail)) continue;
                if (p.kind != LandPointKind::Castle && p.kind != LandPointKind::Landing && !trail) continue;
                const float tx = p.kind == LandPointKind::Castle ? fieldX : p.x, tz = p.kind == LandPointKind::Castle ? fieldZ : p.z;
                if (tx < float(f.x0) + 3.0f || tz < float(f.z0) + 3.0f || tx > float(f.x0 + f.width) - 3.0f || tz > float(f.z0 + f.height) - 3.0f)
                    continue;   // outside the fine grid
                const float maxGrade = trail ? st.trailMaxGrade : st.roadMaxGrade;
                std::vector<uint32_t> path;
                if (!FindRoad(g, m, m.points[size_t(village)].x, m.points[size_t(village)].z, tx, tz, maxGrade, onRoad, path)) continue;
                for (uint32_t c : path) onRoad[c] = 1;
                LandRoad road;
                road.from = uint8_t(village);
                road.to = uint8_t(k);
                road.trail = trail;
                // The line: the path's cell centres, smoothed twice over 7 cells (the ends stay), so the road bends in
                // curves and not the grid's steps.
                std::vector<float> cx, cz;
                for (uint32_t c : path) {
                    cx.push_back(g.Mx(int(c % uint32_t(f.width))));
                    cz.push_back(g.Mz(int(c / uint32_t(f.width))));
                }
                for (int pass = 0; pass < 2; ++pass) {
                    std::vector<float> sxv(cx.size()), szv(cz.size());
                    for (size_t a = 0; a < cx.size(); ++a) {
                        const size_t r = std::min<size_t>(3, std::min(a, cx.size() - 1 - a));
                        float sx = 0.0f, sz = 0.0f;
                        for (size_t b = a - r; b <= a + r; ++b) {
                            sx += cx[b];
                            sz += cz[b];
                        }
                        sxv[a] = sx / float(2 * r + 1);
                        szv[a] = sz / float(2 * r + 1);
                    }
                    cx.swap(sxv);
                    cz.swap(szv);
                }
                road.x = cx;
                road.z = cz;
                std::vector<float> raw;
                bool inRiver = false;
                for (size_t a = 0; a < road.x.size(); ++a) {
                    raw.push_back(g.At(road.x[a], road.z[a]));
                    const uint32_t coarse = g.Coarse(road.x[a], road.z[a]);
                    const bool river = m.waterKind[coarse] == WaterKind::River;
                    if (river && !inRiver) m.points.push_back({LandPointKind::Crossing, road.x[a], road.z[a], false});
                    inRiver = river;
                }
                // The bed: the ground (a ford: 0.3 m under a river's water), its lower and upper grade envelopes (the
                // lowest line at or above it, and the highest at or below it, that never climb more than maxGrade per
                // metre) averaged, so it cuts hills and fills dips; a moving average over up to 9 m; then a forward
                // pass that holds every step to maxGrade exactly.
                const size_t len = raw.size();
                // A trail fords a river 0.3 m under its water. A road crosses on a timber bridge: its deck over each run
                // of river points is level at the higher bank (the ground before and after the run), at least 0.9 m and
                // at most 3 m over the water.
                constexpr float kNoDeck = -1.0e30f;
                std::vector<float> deck(len, kNoDeck);
                for (size_t a = 0; a < len;) {
                    if (m.waterKind[g.Coarse(road.x[a], road.z[a])] != WaterKind::River) {
                        ++a;
                        continue;
                    }
                    size_t e = a;
                    float water = -1.0e30f;
                    while (e < len && m.waterKind[g.Coarse(road.x[e], road.z[e])] == WaterKind::River) {
                        water = std::max(water, m.water[g.Coarse(road.x[e], road.z[e])]);
                        ++e;
                    }
                    const float bank = std::max(a > 0 ? raw[a - 1] : water, e < len ? raw[e] : water);
                    const float level = std::clamp(bank, water + 0.9f, water + 3.0f);
                    for (size_t k = a; k < e; ++k) {
                        if (trail) {
                            raw[k] = m.water[g.Coarse(road.x[k], road.z[k])] - 0.3f;
                            ++road.fordCells;
                        } else {
                            raw[k] = level;
                            deck[k] = level;
                            ++road.bridgeCells;
                        }
                    }
                    a = e;
                }
                // The castle road ends at the bridge's level: the gatehouse pad's (a cutting through the neck if needed).
                if (toCastle && m.site.placed && len > 0) deck[len - 1] = m.site.gateGround;
                // Points on or beside a bed already laid are fixed to it, and a bridge's points to its deck, when the
                // grade lets the road reach it from the last fixed point (beside a hairpin two legs of a road lie close at
                // different heights: one wins).
                std::vector<uint8_t> fixed(len, 0);
                float lastBed = 0.0f, along = 1.0e30f;
                for (size_t a = 0; a < len; ++a) {
                    if (a > 0) along += Distance(road.x[a], road.z[a], road.x[a - 1], road.z[a - 1]);
                    const int i = int(std::floor(road.x[a] - float(f.x0))), j = int(std::floor(road.z[a] - float(f.z0)));
                    if (i < 0 || j < 0 || i >= f.width || j >= f.height) continue;
                    const uint32_t c = f.Index(i, j);
                    if (laidRoute[c] < 0) {
                        if (deck[a] <= kNoDeck || std::abs(deck[a] - lastBed) > maxGrade * along + 1.0e-4f) continue;
                        raw[a] = deck[a];
                        fixed[a] = 1;
                        lastBed = deck[a];
                        along = 0.0f;
                        continue;
                    }
                    const LandRoad& other = m.roads[size_t(laidRoute[c])];
                    // The cell's nearest point, then the earlier route's point nearest to this one around it.
                    size_t b = laidPoint[c];
                    float near = Distance(road.x[a], road.z[a], other.x[b], other.z[b]);
                    const size_t first = b >= 3 ? b - 3 : 0, last = std::min(other.x.size() - 1, b + 3);
                    for (size_t e = first; e <= last; ++e) {
                        const float d = Distance(road.x[a], road.z[a], other.x[e], other.z[e]);
                        if (d < near) {
                            near = d;
                            b = e;
                        }
                    }
                    if (near > 1.5f) continue;
                    const float bed = other.bed[b];
                    if (std::abs(bed - lastBed) > maxGrade * along + 1.0e-4f) continue;
                    raw[a] = bed;
                    fixed[a] = 1;
                    lastBed = bed;
                    along = 0.0f;
                }
                std::vector<float> step(len, 0.0f), upper = raw, lower = raw;
                for (size_t a = 1; a < len; ++a) step[a] = Distance(road.x[a], road.z[a], road.x[a - 1], road.z[a - 1]);
                for (size_t a = 1; a < len; ++a) {
                    if (fixed[a]) continue;
                    upper[a] = std::max(upper[a], upper[a - 1] - maxGrade * step[a]);
                    lower[a] = std::min(lower[a], lower[a - 1] + maxGrade * step[a]);
                }
                for (size_t a = len - 1; a-- > 0;) {
                    if (fixed[a]) continue;
                    upper[a] = std::max(upper[a], upper[a + 1] - maxGrade * step[a + 1]);
                    lower[a] = std::min(lower[a], lower[a + 1] + maxGrade * step[a + 1]);
                }
                std::vector<float> mid(len);
                for (size_t a = 0; a < len; ++a) mid[a] = 0.5f * (upper[a] + lower[a]);
                road.bed.resize(len);
                for (size_t a = 0; a < len; ++a) {
                    if (fixed[a]) {
                        road.bed[a] = raw[a];
                        continue;
                    }
                    const size_t r = std::min<size_t>(4, std::min(a, len - 1 - a));
                    float sum = 0.0f;
                    for (size_t b = a - r; b <= a + r; ++b) sum += mid[b];
                    road.bed[a] = sum / float(2 * r + 1);
                }
                // The band the fixed points allow (at most maxGrade per metre away from each), then a forward pass
                // that holds every step to maxGrade inside that band.
                std::vector<float> hiBand(len, 1.0e30f), loBand(len, -1.0e30f);
                for (size_t a = 0; a < len; ++a)
                    if (fixed[a]) hiBand[a] = loBand[a] = raw[a];
                for (size_t a = 1; a < len; ++a) {
                    hiBand[a] = std::min(hiBand[a], hiBand[a - 1] + maxGrade * step[a]);
                    loBand[a] = std::max(loBand[a], loBand[a - 1] - maxGrade * step[a]);
                }
                for (size_t a = len - 1; a-- > 0;) {
                    hiBand[a] = std::min(hiBand[a], hiBand[a + 1] + maxGrade * step[a + 1]);
                    loBand[a] = std::max(loBand[a], loBand[a + 1] - maxGrade * step[a + 1]);
                }
                for (size_t a = 0; a < len; ++a) {
                    if (fixed[a]) continue;
                    float b = road.bed[a];
                    if (a > 0) b = std::min(std::max(b, road.bed[a - 1] - maxGrade * step[a]), road.bed[a - 1] + maxGrade * step[a]);
                    road.bed[a] = std::min(std::max(b, loBand[a]), hiBand[a]);
                }
                for (size_t a = 0; a < len; ++a) {
                    const int ci = int(std::floor(road.x[a] - float(f.x0))), cj = int(std::floor(road.z[a] - float(f.z0)));
                    for (int dj = -2; dj <= 2; ++dj)
                        for (int di = -2; di <= 2; ++di) {
                            const int i = ci + di, j = cj + dj;
                            if (i < 0 || j < 0 || i >= f.width || j >= f.height) continue;
                            const uint32_t c = f.Index(i, j);
                            const float d = Distance(g.Mx(i), g.Mz(j), road.x[a], road.z[a]);
                            if (d < laidDistance[c]) {
                                laidDistance[c] = d;
                                laidRoute[c] = int32_t(m.roads.size());
                                laidPoint[c] = uint32_t(a);
                            }
                        }
                }
                for (size_t a = 1; a < len; ++a) {
                    const float step = Distance(road.x[a], road.z[a], road.x[a - 1], road.z[a - 1]);
                    road.length += step;
                    road.maxGrade = std::max(road.maxGrade, std::abs(road.bed[a] - road.bed[a - 1]) / step);
                }
                m.roads.push_back(std::move(road));
            }
            // Stamp the beds: 1.6 m either side is road at the bed's height (0.9 m for a trail); the ground beside it slopes to the bed as an
            // embankment or a cut of 1 in 2 (1.5 m plus twice the height between bed and ground, at most 8 m), so a road
            // sits in the land and never stands on a wall.
            const uint32_t n = uint32_t(f.width) * uint32_t(f.height);
            std::vector<float> nearest(n, 1.0e30f), bed(n, 0.0f), half(n, 1.6f);
            for (const LandRoad& road : m.roads)
                for (size_t a = 0; a < road.x.size(); ++a) {
                    const int ci = int(std::floor(road.x[a] - float(f.x0))), cj = int(std::floor(road.z[a] - float(f.z0)));
                    for (int dj = -10; dj <= 10; ++dj)
                        for (int di = -10; di <= 10; ++di) {
                            const int i = ci + di, j = cj + dj;
                            if (i < 0 || j < 0 || i >= f.width || j >= f.height) continue;
                            const uint32_t c = f.Index(i, j);
                            const float d = Distance(g.Mx(i), g.Mz(j), road.x[a], road.z[a]);
                            if (d < nearest[c]) {
                                nearest[c] = d;
                                bed[c] = road.bed[a];
                                half[c] = road.trail ? 0.9f : 1.6f;
                            }
                        }
                }
            for (uint32_t c = 0; c < n; ++c) {
                if (nearest[c] > 9.6f || g.locked[c]) continue;
                const float mx = g.Mx(int(c % uint32_t(f.width))), mz = g.Mz(int(c / uint32_t(f.width)));
                const WaterKind w = m.waterKind[g.Coarse(mx, mz)];
                if (w == WaterKind::Sea || w == WaterKind::Lake) continue;
                if (w == WaterKind::River && half[c] > 1.0f) continue;   // under a road's bridge the river stays
                if (nearest[c] <= half[c]) {
                    g.h[c] = bed[c];
                    g.f.surface[c] = LandSurface::Road;
                    g.f.detail[c] = 0;
                } else {
                    const float shoulder = std::min(8.0f, 1.5f + 2.0f * std::abs(bed[c] - g.h[c]));
                    const float out = (nearest[c] - half[c]) / shoulder;
                    if (out >= 1.0f) continue;
                    const float wgt = 1.0f - SmoothStep(0.0f, 1.0f, out);
                    g.h[c] += (bed[c] - g.h[c]) * wgt;
                    g.f.detail[c] = uint8_t(std::min(float(g.f.detail[c]), (1.0f - wgt) * 255.0f));
                }
            }
        }
    }

    glm::vec2 LandSiteDirection(int k) {
        // cos and sin of k x 15 degrees, as literals (no libm function decides a site).
        static constexpr float kTable[kSiteDirections][2] = {
            {1.0f, 0.0f}, {0.96592583f, 0.25881905f}, {0.8660254f, 0.5f}, {0.70710678f, 0.70710678f}, {0.5f, 0.8660254f},
            {0.25881905f, 0.96592583f}, {0.0f, 1.0f}, {-0.25881905f, 0.96592583f}, {-0.5f, 0.8660254f}, {-0.70710678f, 0.70710678f},
            {-0.8660254f, 0.5f}, {-0.96592583f, 0.25881905f}, {-1.0f, 0.0f}, {-0.96592583f, -0.25881905f}, {-0.8660254f, -0.5f},
            {-0.70710678f, -0.70710678f}, {-0.5f, -0.8660254f}, {-0.25881905f, -0.96592583f}, {0.0f, -1.0f}, {0.25881905f, -0.96592583f},
            {0.5f, -0.8660254f}, {0.70710678f, -0.70710678f}, {0.8660254f, -0.5f}, {0.96592583f, -0.25881905f}};
        const int i = ((k % kSiteDirections) + kSiteDirections) % kSiteDirections;
        return glm::vec2(kTable[i][0], kTable[i][1]);
    }

    glm::vec2 LandSiteGateAxis(const LandSite& site) {
        const glm::vec2 gd = LandSiteDirection(site.gate);
        return std::abs(gd.x) >= std::abs(gd.y) ? glm::vec2(gd.x > 0.0f ? 1.0f : -1.0f, 0.0f) : glm::vec2(0.0f, gd.y > 0.0f ? 1.0f : -1.0f);
    }

    bool LandSiteOutsideCurtain(const LandSite& site, float mx, float mz, float& off) {
        const auto cross = [](glm::vec2 x, glm::vec2 y) { return x.x * y.y - x.y * y.x; };
        const glm::vec2 keep(site.x, site.z);
        const glm::vec2 v = glm::vec2(mx, mz) - keep;
        const float len = std::sqrt(v.x * v.x + v.y * v.y);
        if (len < 1.0e-3f) return false;
        const glm::vec2 u = v / len;
        for (int k = -2; k <= 1; ++k) {
            const int i0 = ((site.gate + k) % kSiteDirections + kSiteDirections) % kSiteDirections, i1 = (i0 + 1) % kSiteDirections;
            const glm::vec2 a = LandSiteDirection(i0) * site.curtain[size_t(i0)], b = LandSiteDirection(i1) * site.curtain[size_t(i1)];
            if (cross(a, v) < 0.0f || cross(v, b) < 0.0f) continue;
            const glm::vec2 ab = b - a;
            const float den = cross(u, ab);
            if (std::abs(den) < 1.0e-6f) continue;
            const float s = std::clamp(-cross(u, a) / den, 0.0f, 1.0f);
            const glm::vec2 q = a + ab * s;
            off = len - std::sqrt(q.x * q.x + q.y * q.y);
            return true;
        }
        return false;
    }

    float LandSiteBridgeSpan(const LandSite& site) {
        const glm::vec2 gate = glm::vec2(site.x, site.z) + LandSiteDirection(site.gate) * site.curtain[size_t(site.gate)];
        const glm::vec2 axis = LandSiteGateAxis(site);
        for (float t = 3.0f; t <= 30.0f; t += 0.5f) {
            float off = 0.0f;
            const glm::vec2 p = gate + axis * t;
            if (LandSiteOutsideCurtain(site, p.x, p.y, off) && off > 10.0f) return t + 1.0f;
        }
        return 14.0f;
    }

    int LandSiteNearestDirection(float dx, float dz) {
        int best = 0;
        float bestDot = -1.0e30f;
        for (int k = 0; k < kSiteDirections; ++k) {
            const glm::vec2 d = LandSiteDirection(k);
            const float dot = d.x * dx + d.y * dz;
            if (dot > bestDot) {
                bestDot = dot;
                best = k;
            }
        }
        return best;
    }

    void BakeLandStarter(const LandSource& source, const LandSettings& st, LandMap& m) {
        m.fine = {};
        m.site = {};
        m.points.clear();
        m.roads.clear();
        m.cave = {};
        if (m.starterRegion <= 0) return;
        int minX = m.width, minZ = m.height, maxX = -1, maxZ = -1;
        double sum = 0.0;
        uint64_t count = 0;
        for (int z = 0; z < m.height; ++z)
            for (int x = 0; x < m.width; ++x)
                if (m.regions[m.Index(x, z)] == uint8_t(m.starterRegion)) {
                    minX = std::min(minX, x);
                    minZ = std::min(minZ, z);
                    maxX = std::max(maxX, x);
                    maxZ = std::max(maxZ, z);
                    sum += double(m.heights[m.Index(x, z)]);
                    ++count;
                }
        if (count == 0) return;
        LandFine& f = m.fine;
        const float c = m.cellSize;
        f.x0 = std::max(0, int(std::floor(float(minX) * c - st.fineMargin)));
        f.z0 = std::max(0, int(std::floor(float(minZ) * c - st.fineMargin)));
        const int x1 = std::min(int(float(m.width) * c), int(std::ceil(float(maxX + 1) * c + st.fineMargin)));
        const int z1 = std::min(int(float(m.height) * c), int(std::ceil(float(maxZ + 1) * c + st.fineMargin)));
        f.width = x1 - f.x0;
        f.height = z1 - f.z0;
        const uint32_t n = uint32_t(f.width) * uint32_t(f.height);
        f.surface.assign(n, LandSurface::Natural);
        f.detail.assign(n, 255);
        Fine g{m, f, std::vector<float>(n), std::vector<uint8_t>(n, 0)};
        std::vector<float> base(n);
        for (int j = 0; j < f.height; ++j)
            for (int i = 0; i < f.width; ++i) base[f.Index(i, j)] = LandCoarseHeight(m, g.Mx(i), g.Mz(j));
        g.h = base;

        // Strata cliffs: hard ground on steep slopes becomes benches and cliffs.
        for (int j = 1; j + 1 < f.height; ++j)
            for (int i = 1; i + 1 < f.width; ++i) {
                const uint32_t k = f.Index(i, j);
                const uint32_t coarse = g.Coarse(g.Mx(i), g.Mz(j));
                if (m.hardness[coarse] < 9 || m.waterKind[coarse] != WaterKind::None) continue;
                const float sx = 0.5f * (base[f.Index(i + 1, j)] - base[f.Index(i - 1, j)]);
                const float sz = 0.5f * (base[f.Index(i, j + 1)] - base[f.Index(i, j - 1)]);
                const float slope = std::max(std::abs(sx), std::abs(sz));
                const float w = SmoothStep(0.4f, 0.8f, slope);
                if (w > 0.0f) g.h[k] += (Terrace(base[k], 4.0f) - base[k]) * w;
            }

        PlaceSite(source, g, m, float(sum / double(count)));
        ScorePoints(source, g, m);
        BuildRoads(st, g, m);

        // The correction, faded to 0 over the outer metres so the fine grid meets the coarse land exactly.
        f.delta.resize(n);
        for (int j = 0; j < f.height; ++j)
            for (int i = 0; i < f.width; ++i) {
                const float edge = float(std::min(std::min(i, f.width - 1 - i), std::min(j, f.height - 1 - j)));
                const float w = SmoothStep(0.0f, kEdgeFade, edge);
                const uint32_t k = f.Index(i, j);
                f.delta[k] = (g.h[k] - base[k]) * w;
            }
    }
}
