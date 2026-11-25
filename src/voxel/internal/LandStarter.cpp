module;

#include <algorithm>
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
        constexpr float kFieldDraftZ = -60.0f;      // the approach field's middle in draft metres (north of the ditch)
        constexpr float kSkirt = 30.0f;             // the site template fades out over this many metres
        constexpr float kCoreX = 48.0f, kCoreNorth = -80.0f, kCoreSouth = 56.0f;
        constexpr float kFootprintX = 34.0f, kFootprintZ = 42.0f;   // the castle's walls and a 2 m margin: no road
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

        void SiteToMap(const LandSite& site, float X, float Z, float& mx, float& mz) {
            // Rotation 0: the draft's north (-Z) faces west (-x); each step turns it a quarter clockwise on the map.
            switch (site.rotation & 3) {
            case 0: mx = site.x + Z, mz = site.z - X; break;
            case 1: mx = site.x + X, mz = site.z + Z; break;
            case 2: mx = site.x - Z, mz = site.z + X; break;
            default: mx = site.x - X, mz = site.z - Z; break;
            }
        }

        void MapToSite(const LandSite& site, float mx, float mz, float& X, float& Z) {
            const float dx = mx - site.x, dz = mz - site.z;
            switch (site.rotation & 3) {
            case 0: X = -dz, Z = dx; break;
            case 1: X = dx, Z = dz; break;
            case 2: X = dz, Z = -dx; break;
            default: X = -dx, Z = -dz; break;
            }
        }

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

        /// The site's field height for a centre and a rotation: a least-squares fit over the exact part of the template
        /// (the field and the castle's footprint), and its cost: the mean squared metres the land must move there, plus
        /// what must be cut where the template only cuts. False when the core is not dry or not inside the fine grid.
        bool FitSite(const Fine& g, LandSite& site) {
            double sum = 0.0;
            int n = 0, all = 0;
            for (float Z = kCoreNorth; Z <= kCoreSouth; Z += 8.0f)
                for (float X = -kCoreX; X <= kCoreX; X += 8.0f) {
                    float mx, mz, t, w;
                    LandSurface s;
                    float slack;
                    SiteToMap(site, X, Z, mx, mz);
                    if (!g.Inside(mx, mz, kSkirt + 8.0f) || g.Wet(mx, mz)) return false;
                    LandSiteTemplate(X, Z, t, w, s, slack);
                    ++all;
                    if (slack > 0.0f) continue;
                    sum += double(g.At(mx, mz) - t);
                    ++n;
                }
            site.fieldHeight = float(sum / double(n));
            double cost = 0.0;
            for (float Z = kCoreNorth; Z <= kCoreSouth; Z += 8.0f)
                for (float X = -kCoreX; X <= kCoreX; X += 8.0f) {
                    float mx, mz, t, w;
                    LandSurface s;
                    float slack;
                    SiteToMap(site, X, Z, mx, mz);
                    LandSiteTemplate(X, Z, t, w, s, slack);
                    double d = double(g.At(mx, mz)) - double(site.fieldHeight + t);
                    if (d < 0.0) d = std::min(0.0, d + double(slack));
                    cost += d * d;
                }
            site.searchCost = float(cost / double(all));
            return true;
        }

        void PlaceSite(const LandSource& source, Fine& g, LandMap& m, float starterMean) {
            const LandPoint* fromFile = nullptr;
            for (const LandPoint& p : source.points)
                if (p.kind == LandPointKind::Castle) fromFile = &p;
            const uint8_t starter = uint8_t(m.starterRegion);
            const std::vector<float> toStarter = CoarseDistance(m, [&](uint32_t i) { return m.regions[i] == starter; });
            auto natural = [&](const LandSite& s, float X, float Z) {
                float mx, mz;
                SiteToMap(s, X, Z, mx, mz);
                return g.At(mx, mz);
            };
            LandSite best;
            if (fromFile) {
                // The owner's castle: the rotation whose neck faces the highest ground.
                for (int r = 0; r < 4; ++r) {
                    LandSite c;
                    c.x = fromFile->x;
                    c.z = fromFile->z;
                    c.rotation = r;
                    if (!FitSite(g, c)) continue;
                    if (!best.placed || natural(c, 0.0f, -70.0f) > natural(best, 0.0f, -70.0f)) {
                        best = c;
                        best.placed = true;
                    }
                }
            } else {
                // A spur of the ridge over the starter region (the owner's answer 9): the middle on mountain ground (any
                // ground outside the starter region when no mountain spot fits), the tip (50 m beyond the middle, away
                // from the neck) within 150 m of the starter region, the neck's natural ground at least 5 m above the
                // tip's, the field 8-45 m above the starter region's mean and at most 85 m; the best fit, in the order rotation,
                // row, column.
                for (int pass = 0; pass < 2 && !best.placed; ++pass)
                for (int r = 0; r < 4; ++r)
                    for (int j = 0; j < g.f.height; j += 8)
                        for (int i = 0; i < g.f.width; i += 8) {
                            LandSite c;
                            c.x = g.Mx(i);
                            c.z = g.Mz(j);
                            c.rotation = r;
                            const uint32_t centre = g.Coarse(c.x, c.z);
                            if (m.regions[centre] == starter) continue;
                            if (pass == 0 && m.biomes[centre] != uint8_t(LandBiome::Mountain)) continue;
                            float tx, tz;
                            SiteToMap(c, 0.0f, 50.0f, tx, tz);
                            if (toStarter[g.Coarse(tx, tz)] > 150.0f) continue;
                            if (natural(c, 0.0f, -70.0f) < natural(c, 0.0f, 50.0f) + 5.0f) continue;
                            if (!FitSite(g, c) || c.fieldHeight < starterMean + 8.0f) continue;
                            // Not far above the village, and the keep (27 m) well under the build ceiling.
                            if (c.fieldHeight > std::min(starterMean + 45.0f, 85.0f)) continue;
                            if (!best.placed || c.searchCost < best.searchCost) {
                                best = c;
                                best.placed = true;
                            }
                        }
            }
            m.site = best;
            if (!best.placed) return;
            const float reach = std::max(kCoreX, std::max(-kCoreNorth, kCoreSouth)) + kSkirt + 2.0f;
            for (int j = 0; j < g.f.height; ++j)
                for (int i = 0; i < g.f.width; ++i) {
                    const float mx = g.Mx(i), mz = g.Mz(j);
                    if (std::abs(mx - best.x) > reach || std::abs(mz - best.z) > reach) continue;
                    float X, Z, t, w;
                    LandSurface s;
                    float slack;
                    MapToSite(best, mx, mz, X, Z);
                    // Outside the castle's footprint the edges wander by up to 4 m, so the cliffs' feet, the ravines and
                    // the skirt do not follow straight lines; the footprint stays a rectangle for the walls.
                    // The move shrinks near the footprint (at most 2/3 of the distance to it), so no point moves into it.
                    const float outside = std::max(std::abs(X) - 32.0f, std::abs(Z) - 40.0f);
                    if (outside > 0.0f) {
                        const float scale = std::min(1.0f, outside / 6.0f);
                        X += (HashNoise(0x5173u, mx / 9.0f, mz / 9.0f) - 0.5f) * 8.0f * scale;
                        Z += (HashNoise(0x5174u, mx / 9.0f, mz / 9.0f) - 0.5f) * 8.0f * scale;
                    }
                    LandSiteTemplate(X, Z, t, w, s, slack);
                    if (w <= 0.0f) continue;
                    const uint32_t c = g.f.Index(i, j);
                    const float target = std::clamp(g.h[c], best.fieldHeight + t - slack, best.fieldHeight + t);
                    g.h[c] += (target - g.h[c]) * w;
                    g.f.detail[c] = uint8_t(std::min(g.f.detail[c], uint8_t((1.0f - w) * 255.0f)));
                    if (w >= 1.0f) {
                        g.f.surface[c] = s;
                        g.locked[c] = 1;
                    }
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
                SiteToMap(m.site, 0.0f, kFieldDraftZ, fieldX, fieldZ);
            }
            auto inSite = [&](float mx, float mz) {
                if (!m.site.placed) return false;
                float X, Z;
                MapToSite(m.site, mx, mz, X, Z);
                return std::abs(X) <= kCoreX + 10.0f && Z >= kCoreNorth - 10.0f && Z <= kCoreSouth + 10.0f;
            };
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

        /// A* on the fine grid from `a` to `b` (map metres): a step costs its length times 1 + 30 grade^2 up to maxGrade,
        /// and 60 more per unit of grade above it (a cut the bed must make, so switchbacks win where the slope allows
        /// them); sea, lakes and the castle are closed; a river step costs 6 m more (a ford).
        bool FindRoad(const Fine& g, const LandMap& m, float ax, float az, float bx, float bz, float maxGrade, std::vector<uint32_t>& path) {
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
                if (m.site.placed) {
                    float X, Z;
                    MapToSite(m.site, mx, mz, X, Z);
                    if (std::abs(X) <= kFootprintX && std::abs(Z) <= kFootprintZ) return true;
                }
                return false;
            };
            std::vector<float> cost(n, 1.0e30f);
            std::vector<uint32_t> from(n, UINT32_MAX);
            using Item = std::pair<float, uint32_t>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
            const float gx = g.Mx(int(goal % uint32_t(f.width))), gz = g.Mz(int(goal / uint32_t(f.width)));
            cost[start] = 0.0f;
            open.push({Distance(g.Mx(int(start % uint32_t(f.width))), g.Mz(int(start / uint32_t(f.width))), gx, gz), start});
            static constexpr int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
            while (!open.empty()) {
                const auto [fc, c] = open.top();
                open.pop();
                if (c == goal) break;
                const int ci = int(c % uint32_t(f.width)), cj = int(c / uint32_t(f.width));
                if (fc > cost[c] + Distance(g.Mx(ci), g.Mz(cj), gx, gz) + 1.0e-3f) continue;
                for (int k = 0; k < 8; ++k) {
                    const int ni = ci + dx[k], nj = cj + dz[k];
                    if (ni < 2 || nj < 2 || ni >= f.width - 2 || nj >= f.height - 2) continue;
                    const uint32_t nc = f.Index(ni, nj);
                    if (closed(nc) && nc != goal) continue;
                    const float step = k < 4 ? 1.0f : 1.41421356f;
                    const float grade = std::abs(g.h[nc] - g.h[c]) / step;
                    const float within = std::min(grade, maxGrade);
                    float add = step * (1.0f + 30.0f * within * within + 60.0f * (grade - within));
                    if (m.waterKind[g.Coarse(g.Mx(ni), g.Mz(nj))] == WaterKind::River) add += 6.0f * step;
                    const float nc2 = cost[c] + add;
                    if (nc2 < cost[nc]) {
                        cost[nc] = nc2;
                        from[nc] = c;
                        open.push({nc2 + Distance(g.Mx(ni), g.Mz(nj), gx, gz), nc});
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
            if (m.site.placed) SiteToMap(m.site, 0.0f, kFieldDraftZ, fieldX, fieldZ);
            const size_t pointCount = m.points.size();
            for (size_t k = 0; k < pointCount; ++k) {
                const LandPoint& p = m.points[k];
                if (p.kind != LandPointKind::Castle && p.kind != LandPointKind::Landing && p.kind != LandPointKind::Cave) continue;
                const float tx = p.kind == LandPointKind::Castle ? fieldX : p.x, tz = p.kind == LandPointKind::Castle ? fieldZ : p.z;
                std::vector<uint32_t> path;
                if (!FindRoad(g, m, m.points[size_t(village)].x, m.points[size_t(village)].z, tx, tz, st.roadMaxGrade, path)) continue;
                LandRoad road;
                road.from = uint8_t(village);
                road.to = uint8_t(k);
                // The line: the path's cell centres, smoothed over 7 cells (the ends stay), so the road's edges follow
                // a curve and not the grid's steps.
                std::vector<float> cx, cz;
                for (uint32_t c : path) {
                    cx.push_back(g.Mx(int(c % uint32_t(f.width))));
                    cz.push_back(g.Mz(int(c / uint32_t(f.width))));
                }
                for (size_t a = 0; a < cx.size(); ++a) {
                    const size_t r = std::min<size_t>(3, std::min(a, cx.size() - 1 - a));
                    float sx = 0.0f, sz = 0.0f;
                    for (size_t b = a - r; b <= a + r; ++b) {
                        sx += cx[b];
                        sz += cz[b];
                    }
                    road.x.push_back(sx / float(2 * r + 1));
                    road.z.push_back(sz / float(2 * r + 1));
                }
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
                for (size_t a = 0; a < len; ++a) {
                    const uint32_t coarse = g.Coarse(road.x[a], road.z[a]);
                    if (m.waterKind[coarse] == WaterKind::River) {
                        raw[a] = m.water[coarse] - 0.3f;
                        ++road.fordCells;
                    }
                }
                std::vector<float> step(len, 0.0f), upper = raw, lower = raw;
                for (size_t a = 1; a < len; ++a) step[a] = Distance(road.x[a], road.z[a], road.x[a - 1], road.z[a - 1]);
                for (size_t a = 1; a < len; ++a) {
                    upper[a] = std::max(upper[a], upper[a - 1] - st.roadMaxGrade * step[a]);
                    lower[a] = std::min(lower[a], lower[a - 1] + st.roadMaxGrade * step[a]);
                }
                for (size_t a = len - 1; a-- > 0;) {
                    upper[a] = std::max(upper[a], upper[a + 1] - st.roadMaxGrade * step[a + 1]);
                    lower[a] = std::min(lower[a], lower[a + 1] + st.roadMaxGrade * step[a + 1]);
                }
                std::vector<float> mid(len);
                for (size_t a = 0; a < len; ++a) mid[a] = 0.5f * (upper[a] + lower[a]);
                road.bed.resize(len);
                for (size_t a = 0; a < len; ++a) {
                    const size_t r = std::min<size_t>(4, std::min(a, len - 1 - a));
                    float sum = 0.0f;
                    for (size_t b = a - r; b <= a + r; ++b) sum += mid[b];
                    road.bed[a] = sum / float(2 * r + 1);
                }
                for (size_t a = 1; a < len; ++a) {
                    const float most = st.roadMaxGrade * step[a];
                    road.bed[a] = std::clamp(road.bed[a], road.bed[a - 1] - most, road.bed[a - 1] + most);
                }
                for (size_t a = 1; a < len; ++a) {
                    const float step = Distance(road.x[a], road.z[a], road.x[a - 1], road.z[a - 1]);
                    road.length += step;
                    road.maxGrade = std::max(road.maxGrade, std::abs(road.bed[a] - road.bed[a - 1]) / step);
                }
                m.roads.push_back(std::move(road));
            }
            // Stamp the beds: 1.6 m either side is road at the bed's height, shoulders blend over the next 2.4 m.
            const uint32_t n = uint32_t(f.width) * uint32_t(f.height);
            std::vector<float> nearest(n, 1.0e30f), bed(n, 0.0f);
            for (const LandRoad& road : m.roads)
                for (size_t a = 0; a < road.x.size(); ++a) {
                    const int ci = int(std::floor(road.x[a] - float(f.x0))), cj = int(std::floor(road.z[a] - float(f.z0)));
                    for (int dj = -4; dj <= 4; ++dj)
                        for (int di = -4; di <= 4; ++di) {
                            const int i = ci + di, j = cj + dj;
                            if (i < 0 || j < 0 || i >= f.width || j >= f.height) continue;
                            const uint32_t c = f.Index(i, j);
                            const float d = Distance(g.Mx(i), g.Mz(j), road.x[a], road.z[a]);
                            if (d < nearest[c]) {
                                nearest[c] = d;
                                bed[c] = road.bed[a];
                            }
                        }
                }
            for (uint32_t c = 0; c < n; ++c) {
                if (nearest[c] > 4.0f || g.locked[c]) continue;
                const float mx = g.Mx(int(c % uint32_t(f.width))), mz = g.Mz(int(c / uint32_t(f.width)));
                const WaterKind w = m.waterKind[g.Coarse(mx, mz)];
                if (w == WaterKind::Sea || w == WaterKind::Lake) continue;
                if (nearest[c] <= 1.6f) {
                    g.h[c] = bed[c];
                    g.f.surface[c] = LandSurface::Road;
                    g.f.detail[c] = 0;
                } else {
                    const float wgt = (4.0f - nearest[c]) / 2.4f;
                    g.h[c] += (bed[c] - g.h[c]) * wgt;
                    g.f.detail[c] = uint8_t(std::min(float(g.f.detail[c]), (1.0f - wgt) * 255.0f));
                }
            }
        }
    }

    void LandSiteTemplate(float X, float Z, float& height, float& weight, LandSurface& surface, float& slack) {
        surface = LandSurface::Natural;
        slack = 0.0f;
        // Distance outside the core rectangle, for the skirt (round at the corners).
        const float ox = std::max(0.0f, std::abs(X) - kCoreX), oz = std::max(0.0f, std::max(kCoreNorth - Z, Z - kCoreSouth));
        const float out = std::sqrt(ox * ox + oz * oz);
        weight = out <= 0.0f ? 1.0f : 1.0f - SmoothStep(0.0f, kSkirt, out);
        if (Z < -40.0f) {
            height = 0.0f;                                   // the approach field
            surface = LandSurface::Site;
        } else if (std::abs(X) <= 32.0f && Z <= 40.0f) {
            height = Z >= 5.0f ? 5.0f : 0.0f;                // outer ward 0 m, the rock step, inner ward 5 m
            surface = LandSurface::Site;
        } else if (Z > 40.0f) {
            height = Z < 41.0f ? -12.0f * (Z - 40.0f) : -12.0f;   // the south cliff to the ravine
            slack = Z < 41.0f ? 0.0f : 4.0f;
        } else if (X < -32.0f) {
            height = X > -33.0f ? -11.0f * (-32.0f - X) : -11.0f;  // the west cliff to the stream bed
            slack = X > -33.0f ? 0.0f : 4.0f;
        } else {
            height = -6.0f * std::min(1.0f, (X - 32.0f) / 14.0f);  // the east slope, 6 m down
        }
        if (out > 0.0f) surface = LandSurface::Natural;
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
