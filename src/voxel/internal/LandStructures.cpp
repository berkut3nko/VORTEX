module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

module vortex.voxel;

import vortex.core;

// The wayside structures (engine/docs/design/land.md "Wayside structures"). Each type is a small plan of Box, Beam and
// Roof records in the structure's own frame (u to its front, v to its left), turned in quarter turns into the world, so
// boxes stay boxes. Every record stands on a foundation 1.5 m under the footprint's lowest ground, or on another record,
// so nothing floats and a blast frees only what it cuts loose.

namespace vortex::voxel {

    namespace {
        constexpr float kCell = 64.0f;          // the placement grid
        constexpr int kTries = 10;              // jittered tries per cell
        constexpr float kSpacing = 45.0f;       // the least distance between two structures' middles
        constexpr float kEdge = 24.0f;          // metres kept from the fine grid's border
        constexpr float kFoundation = 1.5f;
        constexpr float kPiece = 1.0f;          // a ruined wall's top changes every metre

        uint64_t Mix(uint64_t x) {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ull;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebull;
            return x ^ (x >> 31);
        }

        /// The world offset of a local (u, v) after `turn` quarter turns (u to +x at turn 0, turning towards +z).
        glm::vec2 Turned(int turn, glm::vec2 l) {
            switch (turn & 3) {
            case 0: return l;
            case 1: return {-l.y, l.x};
            case 2: return {-l.x, -l.y};
            default: return {l.y, -l.x};
            }
        }

        enum class Facing : uint8_t { Road, Downhill, Hashed };

        /// Where a type may stand (land.md "Wayside structures"): its footprint's half sizes along u and v (metres, with
        /// what lies around it), the ground's relief over its body, and its distance to the nearest road or trail.
        struct Rule {
            float halfU, halfV;
            float bodyU, bodyV;          ///< the part whose relief counts
            float reliefMin, reliefMax;
            float roadMin, roadMax;
            Facing facing;
        };

        constexpr std::array<Rule, kLandStructureKinds> kRules = {{
            {5.0f, 6.0f, 4.0f, 5.0f, 0.0f, 2.0f, 10.0f, 45.0f, Facing::Road},       // Farmstead
            {4.5f, 8.5f, 3.5f, 7.5f, 0.0f, 2.5f, 8.0f, 30.0f, Facing::Road},        // BurnedRow
            {11.5f, 11.5f, 3.0f, 3.0f, 0.0f, 1.5f, 12.0f, 1.0e9f, Facing::Hashed},  // TowerStump (its fallen stage lies up to 11 m out)
            {2.5f, 2.5f, 2.0f, 2.0f, 0.0f, 2.0f, 5.5f, 12.0f, Facing::Road},        // ChimeTower
            {1.5f, 1.5f, 1.25f, 1.25f, 0.0f, 1.5f, 4.0f, 6.5f, Facing::Road},       // Waystone
            {4.0f, 8.5f, 3.25f, 8.25f, 1.5f, 5.0f, 12.0f, 1.0e9f, Facing::Downhill},// HillTower
            {3.5f, 9.0f, 1.0f, 8.0f, 0.0f, 3.0f, 10.0f, 1.0e9f, Facing::Downhill},  // WallRun (across the slope)
            {1.75f, 1.75f, 1.25f, 1.25f, 0.0f, 1.2f, 4.5f, 10.0f, Facing::Road},    // Well
        }};

        /// Builds one structure's records in its own frame.
        class Builder {
        public:
            Builder(const LandMap& map, const LandGeneratorSettings& land, LandStructureSite& site)
                : m_Map(map), m_Land(land), m_Site(site), m(land.materials), vs(land.voxelSize) {}

            glm::vec2 W(float u, float v) const { return m_Site.at + Turned(m_Site.turn, {u, v}); }
            float G(float u, float v) const {
                const glm::vec2 p = W(u, v);
                return LandHeight(m_Map, m_Land, p.x, p.y);
            }
            /// The lowest and highest ground over a local rectangle, every half metre.
            void Range(float u0, float v0, float u1, float v1, float& lo, float& hi) const {
                lo = 1.0e30f;
                hi = -1.0e30f;
                for (float v = v0; v <= v1 + 0.25f; v += 0.5f)
                    for (float u = u0; u <= u1 + 0.25f; u += 0.5f) {
                        const float g = G(std::min(u, u1), std::min(v, v1));
                        lo = std::min(lo, g);
                        hi = std::max(hi, g);
                    }
            }
            int V(float metres) const { return int(std::floor(metres / vs)); }
            float R(uint32_t salt) const { return HashToFloat(Hash32(m_Hash + salt * 0x9e3779b9u)); }

            void Box(float u0, float v0, float u1, float v1, float y0, float y1, uint8_t id) {
                if (y1 <= y0) return;
                const glm::vec2 a = W(u0, v0), b = W(u1, v1);
                const glm::vec2 lo = glm::min(a, b), hi = glm::max(a, b);
                FortRecord r;
                r.shape = FortShape::Box;
                r.id = id;
                r.a = lo;
                r.b = hi;
                r.lo = y0;
                r.hi = y1;
                r.min = glm::ivec3(V(lo.x), V(y0), V(lo.y));
                r.max = glm::max(glm::ivec3(V(hi.x), V(y1), V(hi.y)), r.min + 1);
                Add(r);
            }
            /// An oriented beam from (u0, v0) to (u1, v1), `half` either side, from y0 to y1, rising `rise` towards the end.
            void Beam(float u0, float v0, float u1, float v1, float half, float y0, float y1, uint8_t id, float rise = 0.0f) {
                FortRecord r;
                r.shape = FortShape::Beam;
                r.a = W(u0, v0);
                r.b = W(u1, v1);
                r.half = half;
                r.lo = y0;
                r.hi = y1;
                r.id = id;
                r.height = rise;
                const glm::vec2 lo = glm::min(r.a, r.b) - half - vs, hi = glm::max(r.a, r.b) + half + vs;
                r.min = glm::ivec3(V(lo.x), V(y0 + std::min(0.0f, rise)), V(lo.y));
                r.max = glm::max(glm::ivec3(V(hi.x) + 1, V(y1 + std::max(0.0f, rise)) + 1, V(hi.y) + 1), r.min + 1);
                Add(r);
            }
            /// A square pyramid roof over (u, v), its eaves `half` out at `eave`, rising `pitch` per metre in.
            void Pyramid(float u, float v, float half, float eave, float pitch, uint8_t cover) {
                FortRecord r;
                r.shape = FortShape::Roof;
                r.roof = FortRoof::Cone;
                r.block = FortBlock::Keep;
                r.a = W(u, v);
                r.b = Turned(m_Site.turn, {1.0f, 0.0f});
                r.half = half;
                r.lo = eave;
                r.height = pitch;
                r.id = cover;
                const float reach = half * 2.0f + 0.5f + vs;
                r.min = glm::ivec3(V(r.a.x - reach), V(eave - 0.5f), V(r.a.y - reach));
                r.max = glm::max(glm::ivec3(V(r.a.x + reach) + 1, V(eave + pitch * half + vs) + 1, V(r.a.y + reach) + 1), r.min + 1);
                Add(r);
            }
            /// A gable roof over (u, v) whose ridge runs along v, `length` either side; the slopes fall `pitch` per metre
            /// to the eaves `half` out along u.
            void Gable(float u, float v, float half, float length, float eave, float pitch, uint8_t cover) {
                FortRecord r;
                r.shape = FortShape::Roof;
                r.roof = FortRoof::Gable;
                r.a = W(u, v);
                r.b = Turned(m_Site.turn, {0.0f, 1.0f});
                r.half = half;
                r.length = length;
                r.lo = eave;
                r.height = pitch;
                r.id = cover;
                const float reach = std::max(half, length + 0.25f) + 0.5f + vs;
                r.min = glm::ivec3(V(r.a.x - reach), V(eave - 0.5f), V(r.a.y - reach));
                r.max = glm::max(glm::ivec3(V(r.a.x + reach) + 1, V(eave + pitch * half + vs) + 1, V(r.a.y + reach) + 1), r.min + 1);
                Add(r);
            }

            /// A ruined wall from (u0, v0) to (u1, v1) (one of them a thin axis), cut into pieces of a metre along its long
            /// side, each from `bottom` to `top(piece's middle along the run, its length)`: the plinth to `plinth` in dark
            /// stone, the top `band` in soot when the piece stands higher than `bandFrom`.
            template <class Top>
            void Ruin(float u0, float v0, float u1, float v1, float bottom, float plinth, uint8_t body, float band, float bandFrom, Top&& top) {
                const bool alongV = (v1 - v0) >= (u1 - u0);
                const float a0 = alongV ? v0 : u0, a1 = alongV ? v1 : u1;
                const int pieces = std::max(1, int(std::floor((a1 - a0) / kPiece + 0.5f)));
                const float step = (a1 - a0) / float(pieces);
                for (int k = 0; k < pieces; ++k) {
                    const float p0 = a0 + step * float(k), p1 = k + 1 == pieces ? a1 : p0 + step;
                    const float t = top(0.5f * (p0 + p1), k);
                    const float pu0 = alongV ? u0 : p0, pu1 = alongV ? u1 : p1, pv0 = alongV ? p0 : v0, pv1 = alongV ? p1 : v1;
                    const float plinthTop = std::min(plinth, t);
                    Box(pu0, pv0, pu1, pv1, bottom, plinthTop, m.stoneDark);
                    if (band > 0.0f && t > bandFrom) {
                        Box(pu0, pv0, pu1, pv1, plinthTop, t - band, body);
                        Box(pu0, pv0, pu1, pv1, std::max(plinthTop, t - band), t, m.soot);
                    } else {
                        Box(pu0, pv0, pu1, pv1, plinthTop, t, body);
                    }
                }
            }

            void Build(uint32_t hash);

        private:
            void Add(const FortRecord& r) {
                m_Site.plan.records.push_back(r);
                if (m_Site.plan.records.size() == 1) {
                    m_Site.min = r.min;
                    m_Site.max = r.max;
                } else {
                    m_Site.min = glm::min(m_Site.min, r.min);
                    m_Site.max = glm::max(m_Site.max, r.max);
                }
            }

            void Farmstead(int plots);
            void TowerStump();
            void ChimeTower();
            void Waystone();
            void HillTower();
            void WallRun();
            void Well();
            /// A rubble mound around (u, v): columns of about half a metre, each on its own ground and lower towards the
            /// edge, so the mound follows a slope; a block of `top` crowns the middle.
            void Mound(float u, float v, float halfU, float halfV, float height, uint8_t id, uint8_t top) {
                const int nu = std::max(1, int(std::floor(halfU * 4.0f + 0.5f))), nv = std::max(1, int(std::floor(halfV * 4.0f + 0.5f)));
                const float su = 2.0f * halfU / float(nu), sv = 2.0f * halfV / float(nv);
                float crown = -1.0e30f;
                for (int b = 0; b < nv; ++b)
                    for (int a = 0; a < nu; ++a) {
                        const float u0 = u - halfU + su * float(a), v0 = v - halfV + sv * float(b);
                        const float du = std::abs(u0 + 0.5f * su - u) / halfU, dv = std::abs(v0 + 0.5f * sv - v) / halfV;
                        const float rise = height * (1.0f - 0.6f * std::max(du, dv)) * (0.75f + 0.25f * R(500u + uint32_t(a + 7 * b)));
                        float lo = 0.0f, hi = 0.0f;
                        Range(u0, v0, u0 + su, v0 + sv, lo, hi);
                        Box(u0, v0, u0 + su, v0 + sv, lo - 0.5f, hi + rise, id);
                        if (du < 0.5f && dv < 0.5f) crown = std::max(crown, hi + rise);
                    }
                if (crown > -1.0e29f) Box(u - halfU * 0.4f, v - halfV * 0.4f, u + halfU * 0.4f, v + halfV * 0.4f, crown - 0.25f, crown + 0.25f, top);
            }

            const LandMap& m_Map;
            const LandGeneratorSettings& m_Land;
            LandStructureSite& m_Site;
            const WorldMaterials& m;
            float vs;
            uint32_t m_Hash = 0;
        };

        uint8_t Or(uint8_t id, uint8_t fallback) { return id != 0 ? id : fallback; }

        /// A burned stone ground floor (setting.md 5.1 "Ruin"): `plots` rooms side by side along v under one front, each
        /// with a door; the upper storeys are charred post stumps on the wall tops; one corner slumped into a rubble
        /// mound; a chimney stack stands at the back; charred beams lie on the scorched floors.
        void Builder::Farmstead(int plots) {
            const float t = 0.625f;
            const float A = plots == 1 ? 3.0f + 0.75f * R(1) : 3.5f;
            const float plotW = plots == 1 ? 8.0f + 2.0f * R(2) : 4.5f + 0.5f * R(2);
            const float B = 0.5f * plotW * float(plots);
            float lo = 0.0f, hi = 0.0f;
            Range(-A, -B, A, B, lo, hi);
            m_Site.ground = lo;
            const float bottom = lo - kFoundation, floor = lo;
            const uint8_t charred = Or(m.charredWood, m.wood), scorched = Or(m.scorched, m.dirt), soot = Or(m.soot, m.stoneDark);
            // Clear the rooms (plants, bumps of ground) and lay the scorched floor.
            Box(-A + t, -B + t, A - t, B - t, floor, hi + 8.0f, 0);
            Box(-A + t, -B + t, A - t, B - t, floor - 0.5f, floor, scorched);
            const float base = (hi - floor) + 2.4f + 0.8f * R(3);
            const int slumped = int(Hash32(m_Hash + 77u) & 3u);   // the corner that fell
            const glm::vec2 corner(slumped & 1 ? A : -A, slumped & 2 ? B : -B);
            std::vector<float> plotBase(static_cast<size_t>(plots));
            for (int p = 0; p < plots; ++p) plotBase[size_t(p)] = base * (plots == 1 ? 1.0f : 0.7f + 0.45f * R(10u + uint32_t(p)));
            const auto plotOf = [&](float v) { return std::clamp(int(std::floor((v + B) / plotW)), 0, plots - 1); };
            const auto ragged = [&](float u, float v, int k, uint32_t salt) {
                const float d = std::max(std::abs(u - corner.x), std::abs(v - corner.y));
                const float h = plotBase[size_t(plotOf(v))];
                if (d < 2.5f) return floor + 0.4f + 0.6f * R(salt + uint32_t(k));
                return floor + h * (0.62f + 0.38f * R(salt + uint32_t(k)));
            };
            // The doors (one per plot, in its front wall) and the posts: where a wall piece kept 80 % of its plot's height,
            // a charred stump of the upper storey's frame stands on it.
            std::vector<float> doors(static_cast<size_t>(plots));
            for (int p = 0; p < plots; ++p) doors[size_t(p)] = -B + plotW * float(p) + 1.0f + (plotW - 3.25f) * R(20u + uint32_t(p));
            const auto post = [&](float u, float v, float top, int k) {
                if (top < floor + 0.8f * plotBase[size_t(plotOf(v))]) return;
                const float h = 0.5f + 1.3f * R(40u + uint32_t(k));
                Box(u - 0.1875f, v - 0.1875f, u + 0.1875f, v + 0.1875f, top, top + h, charred);
            };
            const uint8_t body = m.stone;
            int k = 0;
            // The front (u = +A) and back walls run the whole length; the side and party walls fit between them.
            Ruin(A - t, -B, A, B, bottom, floor + 0.5f, body, 0.375f, floor + 1.2f, [&](float v, int i) {
                for (int p = 0; p < plots; ++p)
                    if (v > doors[size_t(p)] && v < doors[size_t(p)] + 1.25f) return floor;
                const float top = ragged(A, v, i, 100);
                if (i % 2 == 0) post(A - 0.3125f, v, top, k++);
                return top;
            });
            Ruin(-A, -B, -A + t, B, bottom, floor + 0.5f, body, 0.375f, floor + 1.2f, [&](float v, int i) {
                const float top = ragged(-A, v, i, 200);
                if (i % 2 == 1) post(-A + 0.3125f, v, top, k++);
                return top;
            });
            for (int p = 0; p <= plots; ++p) {
                const float v = -B + plotW * float(p);
                const float w0 = p == 0 ? v : (p == plots ? v - t : v - 0.5f * t);
                const float w1 = p == 0 ? v + t : (p == plots ? v : v + 0.5f * t);
                Ruin(-A + t, w0, A - t, w1, bottom, floor + 0.5f, body, 0.375f, floor + 1.2f, [&](float u, int i) {
                    const float top = ragged(u, v, i, 300u + uint32_t(p) * 17u);
                    if (i == 2) post(u, 0.5f * (w0 + w1), top, k++);
                    return top;
                });
            }
            // The chimney stack of the first plot: chimneys outlast the fire.
            const float cv = -B + 0.5f * plotW;
            Box(-A, cv - 0.625f, -A + 1.125f, cv + 0.625f, bottom, floor + base + 1.5f, m.stone);
            Box(-A, cv - 0.625f, -A + 1.125f, cv + 0.625f, floor + base + 1.5f, floor + base + 1.875f, soot);
            // Fallen beams on the floors and a heap of burned rubble, then the slumped corner's mound outside.
            for (int p = 0; p < plots; ++p) {
                const float c = -B + plotW * (float(p) + 0.5f);
                const float s = R(50u + uint32_t(p)) < 0.5f ? 1.0f : -1.0f;
                Beam(-A + t + 0.5f, c - s * (0.5f * plotW - t - 0.6f), A - t - 0.5f, c + s * (0.5f * plotW - t - 0.6f), 0.1875f, floor, floor + 0.375f, charred);
                Box(-0.75f, c - 0.5f, 0.5f, c + 0.75f, floor, floor + 0.375f, Or(m.gravel, m.stone));
            }
            Mound(corner.x + (corner.x > 0.0f ? 1.25f : -1.25f), corner.y, 1.0f, 1.25f, 0.5f, Or(m.rubbleCore, m.stone), m.stone);
        }

        /// A felled stage tower (setting.md 5.1 "Ruin": felled tower stumps): a hollow stone stump with a door, and its
        /// fallen stage lying in a line of blocks and a charred beam where it fell.
        void Builder::TowerStump() {
            const float S = 2.5f + 0.5f * R(1), t = 1.0f;
            float lo = 0.0f, hi = 0.0f;
            Range(-S, -S, S, S, lo, hi);
            m_Site.ground = lo;
            const float bottom = lo - kFoundation, floor = lo;
            const float base = (hi - floor) + 4.5f + 3.0f * R(2);
            const uint8_t granite = Or(m.granite, m.stoneDark), ashlar = Or(m.ashlar, m.stone);
            Box(-S + t, -S + t, S - t, S - t, floor, hi + 12.0f, 0);
            Box(-S + t, -S + t, S - t, S - t, floor - 0.5f, floor + 0.25f, Or(m.gravel, m.stone));
            const auto top = [&](int k, uint32_t salt) { return floor + base * (0.7f + 0.3f * R(salt + uint32_t(k))); };
            // The door in the front wall (u = +S): its threshold, and a lintel 2.25 m over the floor that rests on the
            // pieces either side of the door.
            float doorLo = 1.0e30f, doorHi = -1.0e30f;
            Ruin(S - t, -S, S, S, bottom, floor + 1.0f, ashlar, 0.0f, 0.0f, [&](float v, int k) {
                if (std::abs(v) < 0.75f) {
                    doorLo = std::min(doorLo, v - 0.5f);
                    doorHi = std::max(doorHi, v + 0.5f);
                    return floor;
                }
                return top(k, 100);
            });
            Box(S - t, doorLo - 0.5f, S, doorHi + 0.5f, floor + 2.25f, floor + 0.7f * base, ashlar);
            Ruin(-S, -S, -S + t, S, bottom, floor + 1.0f, ashlar, 0.0f, 0.0f, [&](float, int k) { return top(k, 200); });
            Ruin(-S + t, -S, S - t, -S + t, bottom, floor + 1.0f, ashlar, 0.0f, 0.0f, [&](float, int k) { return top(k, 300); });
            Ruin(-S + t, S - t, S - t, S, bottom, floor + 1.0f, ashlar, 0.0f, 0.0f, [&](float, int k) { return top(k, 400); });
            // Granite quoins at the 4 corners, as high as the lower wall beside them.
            for (const float cu : {-S, S - t})
                for (const float cv : {-S, S - t}) Box(cu, cv, cu + t, cv + t, bottom, floor + base * 0.7f, granite);
            // The fallen stage: blocks along one of the 3 sides away from the door, 1-11 m out.
            static constexpr glm::vec2 kSides[3] = {{-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};
            const glm::vec2 d = kSides[Hash32(m_Hash + 5u) % 3u], side(-d.y, d.x);
            for (int k = 0; k < 5; ++k) {
                const float along = S + 1.5f + 2.0f * float(k);
                const float off = (R(60u + uint32_t(k)) - 0.5f) * 1.2f;
                const glm::vec2 c = d * along + side * off;
                const float w = 0.8f + 0.5f * R(70u + uint32_t(k)), l = 0.6f + 0.3f * R(80u + uint32_t(k));
                const glm::vec2 h = glm::abs(d) * l + glm::abs(side) * w;
                Mound(c.x, c.y, h.x, h.y, 0.25f + 0.6f * R(90u + uint32_t(k)), k % 2 == 0 ? ashlar : Or(m.rubbleCore, m.stone), k == 2 ? granite : ashlar);
            }
            const glm::vec2 b0 = d * (S + 1.0f) + side * 1.25f, b1 = d * (S + 9.0f) + side * 1.75f;
            float bLo = 0.0f, bHi = 0.0f;
            Range(std::min(b0.x, b1.x), std::min(b0.y, b1.y), std::max(b0.x, b1.x), std::max(b0.y, b1.y), bLo, bHi);
            Beam(b0.x, b0.y, b1.x, b1.y, 0.1875f, bLo - 0.125f, bHi + 0.25f, Or(m.charredWood, m.wood));
        }

        /// A wayside chime tower (setting.md 5.7): 4 log posts on stone footings, braces, a platform, a pyramid roof and an
        /// iron bell under it.
        void Builder::ChimeTower() {
            const float P = 1.25f, half = 0.1875f;
            float lo = 0.0f, hi = 0.0f;
            Range(-P - 0.5f, -P - 0.5f, P + 0.5f, P + 0.5f, lo, hi);
            m_Site.ground = lo;
            const float bottom = lo - kFoundation, foot = hi + 0.375f;
            const float H = foot + 7.0f + 1.5f * R(1);
            const float deck = H - 2.25f;
            const uint8_t wood = m.wood, planks = m.planks;
            const uint8_t cover = R(2) < 0.5f ? Or(m.slate, m.planks) : m.planks;
            for (const float u : {-P, P})
                for (const float v : {-P, P}) {
                    Box(u - 0.375f, v - 0.375f, u + 0.375f, v + 0.375f, bottom, foot, m.stone);
                    Box(u - half, v - half, u + half, v + half, foot, H, wood);
                }
            // Ring beams over the footings, under the deck and at the top: each joins 2 posts.
            for (const float y : {foot + 2.5f, deck - 0.5f, H - 0.375f}) {
                for (const float u : {-P, P}) Box(u - half, -P - half, u + half, P + half, y, y + 0.375f, wood);
                for (const float v : {-P, P}) Box(-P - half, v - half, P + half, v + half, y, y + 0.375f, wood);
            }
            // Braces on the two sides from the lower ring to the one under the deck.
            for (const float u : {-P, P}) Beam(u, -P, u, P, 0.125f, foot + 2.75f, foot + 3.25f, wood, (deck - 0.5f) - (foot + 2.75f));
            Box(-P - half, -P - half, P + half, P + half, deck - 0.125f, deck + 0.125f, planks);
            // A rail on 3 sides of the deck (the front is open to the road).
            for (const float v : {-P, P}) Box(-P - half, v - 0.125f, P + half, v + 0.125f, deck + 0.875f, deck + 1.125f, wood);
            Box(-P - 0.125f, -P - half, -P + 0.125f, P + half, deck + 0.875f, deck + 1.125f, wood);
            // The roof sits on the top ring; the bell hangs from the roof's king post on a chain.
            Pyramid(0.0f, 0.0f, 2.0f, H - 0.25f, 0.9f, cover);
            Box(-0.0625f, -0.0625f, 0.0625f, 0.0625f, H - 1.25f, H - 0.125f, m.iron);
            Box(-0.3125f, -0.3125f, 0.3125f, 0.3125f, deck + 0.25f, H - 1.25f, m.iron);
        }

        /// A Ledgerwards waystone (setting.md 5.7): a granite slab on a plinth with an iron tally plate; sometimes under a
        /// small gable roof on two posts (a shrine).
        void Builder::Waystone() {
            const bool shrine = R(1) < 0.45f;
            const float pu = shrine ? 1.0f : 0.75f, pv = shrine ? 1.25f : 0.75f;
            float lo = 0.0f, hi = 0.0f;
            Range(-pu, -pv, pu, pv, lo, hi);
            m_Site.ground = lo;
            const uint8_t granite = Or(m.granite, m.stone);
            const float plinth = hi + 0.375f;
            Box(-pu, -pv, pu, pv, lo - kFoundation, plinth, granite);
            const float top = plinth + 1.8f + 0.6f * R(2);
            Box(-0.25f, -0.5f, 0.25f, 0.5f, plinth, top, granite);
            Box(-0.375f, -0.625f, 0.375f, 0.625f, top, top + 0.25f, granite);
            Box(0.25f, -0.25f, 0.375f, 0.25f, top - 0.9f, top - 0.4f, m.iron);
            if (shrine) {
                const float eave = top + 0.5f;
                for (const float v : {-1.0f, 1.0f}) Box(-0.125f, v - 0.125f, 0.125f, v + 0.125f, plinth, eave + 0.625f, m.wood);
                Box(-0.125f, -1.25f, 0.125f, 1.25f, eave + 0.25f, eave + 0.625f, m.wood);   // the ridge beam on the posts
                Gable(0.0f, 0.0f, 1.0f, 1.25f, eave, 0.8f, R(3) < 0.5f ? Or(m.slate, m.planks) : Or(m.thatch, m.planks));
            }
        }

        /// An open-backed hill-fort tower on a slope (setting.md 5.1 "Fort"), its front downhill, with stubs of the
        /// curtain wall on both sides and a timber floor on two joists; the tops ragged.
        void Builder::HillTower() {
            const float S = 2.75f + 0.5f * R(1), t = 1.25f;
            const float L = 3.0f + 2.0f * R(2);
            float lo = 0.0f, hi = 0.0f;
            Range(-S, -S - L, S, S + L, lo, hi);
            m_Site.ground = lo;
            const float bottom = lo - kFoundation;
            const uint8_t granite = Or(m.granite, m.stoneDark), ashlar = Or(m.ashlar, m.stone);
            float tLo = 0.0f, tHi = 0.0f;
            Range(-S, -S, S, S, tLo, tHi);
            const float joist = tHi + 2.5f;
            const auto localTop = [&](float u0, float v0, float u1, float v1) {
                float a = 0.0f, b = 0.0f;
                Range(u0, v0, u1, v1, a, b);
                return b;
            };
            const float base = 3.5f + 3.0f * R(3);
            // The front, then the 2 sides back to the open back; side pieces stay over the joists they carry.
            Ruin(S - t, -S, S, S, bottom, tLo + 1.5f, ashlar, 0.0f, 0.0f, [&](float v, int k) {
                return localTop(S - t, v - 0.5f, S, v + 0.5f) + base * (0.7f + 0.3f * R(100u + uint32_t(k)));
            });
            for (const float side : {-1.0f, 1.0f}) {
                const float v0 = side < 0.0f ? -S : S - t, v1 = side < 0.0f ? -S + t : S;
                Ruin(-S, v0, S - t, v1, bottom, tLo + 1.5f, ashlar, 0.0f, 0.0f, [&](float u, int k) {
                    const float h = localTop(u - 0.5f, v0, u + 0.5f, v1) + base * (0.55f + 0.45f * R(200u + uint32_t(k) + (side > 0.0f ? 50u : 0u)));
                    return std::max(h, joist + 0.75f);
                });
                // The curtain stub, along the contour, its end lower.
                const float c0 = side < 0.0f ? -S - L : S, c1 = side < 0.0f ? -S : S + L;
                Ruin(-0.5f, c0, 0.5f, c1, bottom, tLo + 1.5f, m.stone, 0.0f, 0.0f, [&](float v, int k) {
                    const float out = std::abs(v) - S;
                    return localTop(-0.5f, v - 0.5f, 0.5f, v + 0.5f) + (1.5f + 2.0f * R(300u + uint32_t(k))) * (1.0f - 0.5f * out / L);
                });
                Box(S - t, v0, S, v1, bottom, tLo + base * 0.6f, granite);   // a quoin at each front corner
            }
            for (const float u : {-S + 0.75f, 0.5f}) Box(u - 0.1875f, -S + t - 0.25f, u + 0.1875f, S - t + 0.25f, joist, joist + 0.375f, m.wood);
            Box(-S + 0.5f, -S + t, 0.75f, 0.25f, joist + 0.375f, joist + 0.5f, m.planks);
        }

        /// A breached border wall (setting.md 7, the Border Centuries): faces of stone over a rubble core, stepping with
        /// the ground, a slumped breach and rubble mounds on both sides of it.
        void Builder::WallRun() {
            const float B = 5.0f + 3.0f * R(1), t = 1.0f + 0.25f * R(2);
            float lo = 0.0f, hi = 0.0f;
            Range(-0.5f * t, -B, 0.5f * t, B, lo, hi);
            m_Site.ground = lo;
            const float breach = (R(3) - 0.5f) * B, width = 2.5f + 1.5f * R(4);
            const float face = 0.375f;
            const uint8_t core = Or(m.rubbleCore, m.stone);
            const int pieces = int(std::floor(2.0f * B / kPiece + 0.5f));
            const float step = 2.0f * B / float(pieces);
            for (int k = 0; k < pieces; ++k) {
                const float v0 = -B + step * float(k), v1 = k + 1 == pieces ? B : v0 + step, v = 0.5f * (v0 + v1);
                float a = 0.0f, b = 0.0f;
                Range(-0.5f * t, v0, 0.5f * t, v1, a, b);
                const bool broken = std::abs(v - breach) < 0.5f * width;
                const float end = std::min(1.0f, (B - std::abs(v)) / 2.0f);   // the run's ends fall away
                const float h = broken ? 0.25f + 0.55f * R(10u + uint32_t(k)) : (1.0f + (1.5f + 1.2f * R(20u + uint32_t(k))) * end);
                const float coreTop = b + h, faceTop = broken ? coreTop - 0.25f : coreTop;
                Box(-0.5f * t, v0, -0.5f * t + face, v1, lo - kFoundation, faceTop, m.stone);
                Box(0.5f * t - face, v0, 0.5f * t, v1, lo - kFoundation, faceTop, m.stone);
                Box(-0.5f * t + face, v0, 0.5f * t - face, v1, lo - kFoundation, coreTop, core);
                if (!broken && h > 2.0f) Box(-0.5f * t, v0, 0.5f * t, v1, coreTop, coreTop + 0.125f, Or(m.moss, m.grass));
            }
            for (const float side : {-1.0f, 1.0f}) {
                Mound(side * (0.5f * t + 1.0f), breach, 0.875f, 0.5f * width, 0.35f + 0.4f * R(side > 0.0f ? 30u : 31u), core, m.stone);
            }
        }

        /// A roadside well (setting.md 5.7): a stone curb round a lined shaft with water at its foot, 2 posts, a crossbeam,
        /// a rope and bucket, and a small gable roof.
        void Builder::Well() {
            const float O = 1.0f, t = 0.375f, I = O - t;
            float lo = 0.0f, hi = 0.0f;
            Range(-O, -O, O, O, lo, hi);
            m_Site.ground = lo;
            const float curb = hi + 0.875f, shaft = lo - 4.0f;
            Box(-I, -I, I, I, shaft + 1.0f, curb + 4.0f, 0);
            Box(-I, -I, I, I, shaft, shaft + 0.5f, m.stone);
            Box(-I, -I, I, I, shaft + 0.5f, shaft + 1.0f, m.water);
            Box(-O, -O, O, -I, shaft, curb, m.stone);
            Box(-O, I, O, O, shaft, curb, m.stone);
            Box(-O, -I, -I, I, shaft, curb, m.stone);
            Box(I, -I, O, I, shaft, curb, m.stone);
            const float T = curb + 1.75f, mid = 0.5f * (I + O);
            for (const float v : {-mid, mid}) Box(-0.125f, v - 0.125f, 0.125f, v + 0.125f, curb, T, m.wood);
            Box(-0.125f, -O, 0.125f, O, T - 0.25f, T + 0.5f, m.wood);
            Gable(0.0f, 0.0f, 1.0f, 1.25f, T - 0.125f, 0.7f, R(1) < 0.5f ? Or(m.thatch, m.planks) : m.planks);
            Box(-0.0625f, -0.0625f, 0.0625f, 0.0625f, T - 1.25f, T - 0.25f, m.wood);
            Box(-0.25f, -0.25f, 0.25f, 0.25f, T - 1.625f, T - 1.25f, m.planks);
        }

        void Builder::Build(uint32_t hash) {
            m_Hash = hash;
            switch (m_Site.kind) {
            case LandStructureKind::Farmstead: Farmstead(1); break;
            case LandStructureKind::BurnedRow: Farmstead(2 + int(Hash32(hash + 3u) & 1u)); break;
            case LandStructureKind::TowerStump: TowerStump(); break;
            case LandStructureKind::ChimeTower: ChimeTower(); break;
            case LandStructureKind::Waystone: Waystone(); break;
            case LandStructureKind::HillTower: HillTower(); break;
            case LandStructureKind::WallRun: WallRun(); break;
            case LandStructureKind::Well: Well(); break;
            }
            const glm::vec2 lo = glm::vec2(float(m_Site.min.x), float(m_Site.min.z)) * vs - m_Site.at;
            const glm::vec2 hi = glm::vec2(float(m_Site.max.x), float(m_Site.max.z)) * vs - m_Site.at;
            const glm::vec2 far = glm::max(glm::abs(lo), glm::abs(hi));
            m_Site.radius = std::sqrt(far.x * far.x + far.y * far.y);
        }

        /// The fine grid's surface at map metres (Natural outside it).
        LandSurface FineSurface(const LandMap& map, float mx, float mz) {
            const LandFine& f = map.fine;
            const int i = int(std::floor(mx)) - f.x0, j = int(std::floor(mz)) - f.z0;
            if (i < 0 || j < 0 || i >= f.width || j >= f.height) return LandSurface::Natural;
            return f.surface[f.Index(i, j)];
        }
    }

    const char* LandStructureName(LandStructureKind kind) {
        switch (kind) {
        case LandStructureKind::Farmstead: return "farmstead";
        case LandStructureKind::BurnedRow: return "burned row";
        case LandStructureKind::TowerStump: return "tower stump";
        case LandStructureKind::ChimeTower: return "chime tower";
        case LandStructureKind::Waystone: return "waystone";
        case LandStructureKind::HillTower: return "hill tower";
        case LandStructureKind::WallRun: return "wall run";
        case LandStructureKind::Well: return "well";
        }
        return "?";
    }

    LandStructures PlanLandStructures(const LandMap& map, const LandGeneratorSettings& land) {
        LandStructures out;
        const LandFine& fine = map.fine;
        if (fine.Empty()) return out;
        const glm::vec2 origin = land.origin;
        const float vs = land.voxelSize;
        const auto ground = [&](glm::vec2 p) { return LandHeight(map, land, p.x, p.y); };

        // What stays clear: Hollowkeep's site with 30 m round its curtain, its approach (the field, the bridge, the siege's
        // attacking spawn) 100 m out from the gate and 50 m either side, and the points of interest.
        glm::vec2 keep(0.0f), gate(0.0f), axis(0.0f);
        float castle = 0.0f;
        if (map.site.placed) {
            keep = origin + glm::vec2(map.site.x, map.site.z);
            for (const float r : map.site.curtain) castle = std::max(castle, r);
            castle += 30.0f;
            gate = keep + LandSiteDirection(map.site.gate) * map.site.curtain[size_t(map.site.gate)];
            axis = LandSiteGateAxis(map.site);
        }
        struct Keepout {
            glm::vec2 at;
            float radius;
        };
        std::vector<Keepout> points;
        for (const LandPoint& p : map.points) {
            float r = 15.0f;
            switch (p.kind) {
            case LandPointKind::Village: r = 50.0f; break;
            case LandPointKind::Landing: r = 30.0f; break;
            case LandPointKind::Cave: r = 30.0f; break;
            case LandPointKind::Landmark: r = 20.0f; break;
            default: break;
            }
            points.push_back({origin + glm::vec2(p.x, p.z), r});
        }
        if (map.cave.placed)
            for (int k = 0; k < 2; ++k) points.push_back({origin + glm::vec2(map.cave.x[k], map.cave.z[k]), 30.0f});
        std::vector<glm::vec2> road;
        for (const LandRoad& r : map.roads)
            for (size_t k = 0; k < r.x.size(); ++k) road.push_back(origin + glm::vec2(r.x[k], r.z[k]));
        const auto nearestRoad = [&](glm::vec2 c, glm::vec2& at) {
            float best = 1.0e30f;
            for (const glm::vec2& p : road) {
                const glm::vec2 d = p - c;
                const float d2 = d.x * d.x + d.y * d.y;
                if (d2 < best) {
                    best = d2;
                    at = p;
                }
            }
            return std::sqrt(best);
        };

        const float fx0 = origin.x + float(fine.x0) + kEdge, fx1 = origin.x + float(fine.x0 + fine.width) - kEdge;
        const float fz0 = origin.y + float(fine.z0) + kEdge, fz1 = origin.y + float(fine.z0 + fine.height) - kEdge;
        const uint32_t seed = Hash32(land.seed * 0x9e3779b9u + 0x73747275u);
        std::array<int, kLandStructureKinds> placed{};

        const auto fits = [&](LandStructureKind kind, glm::vec2 c, int turn, float roadDistance) {
            const Rule& rule = kRules[size_t(kind)];
            if (roadDistance < rule.roadMin || roadDistance > rule.roadMax) return false;
            const float reach = std::sqrt(rule.halfU * rule.halfU + rule.halfV * rule.halfV);
            if (c.x - reach < fx0 || c.x + reach > fx1 || c.y - reach < fz0 || c.y + reach > fz1) return false;
            if (map.site.placed) {
                const glm::vec2 k = c - keep;
                if (std::sqrt(k.x * k.x + k.y * k.y) < castle + reach) return false;
                const glm::vec2 q = c - gate;
                const float along = q.x * axis.x + q.y * axis.y, across = q.x * -axis.y + q.y * axis.x;
                if (along > -20.0f - reach && along < 100.0f + reach && std::abs(across) < 50.0f + reach) return false;
            }
            for (const Keepout& p : points) {
                const glm::vec2 d = c - p.at;
                if (std::sqrt(d.x * d.x + d.y * d.y) < p.radius + reach) return false;
            }
            // Dry natural ground (no road, trail, site or stream bed) under the footprint and 1 m round it, no water within
            // 3 m, above the shore; the body's relief within the type's range.
            const glm::vec2 hu = glm::abs(Turned(turn, {rule.halfU, 0.0f})) + glm::abs(Turned(turn, {0.0f, rule.halfV}));
            const glm::vec2 bu = glm::abs(Turned(turn, {rule.bodyU, 0.0f})) + glm::abs(Turned(turn, {0.0f, rule.bodyV}));
            float lo = 1.0e30f, hi = -1.0e30f;
            for (float z = -bu.y; z <= bu.y + 0.01f; z += 1.0f)
                for (float x = -bu.x; x <= bu.x + 0.01f; x += 1.0f) {
                    const float g = ground(c + glm::vec2(x, z));
                    lo = std::min(lo, g);
                    hi = std::max(hi, g);
                    if (hi - lo > rule.reliefMax) return false;
                }
            if (lo < 2.0f || hi > 80.0f || hi - lo < rule.reliefMin) return false;
            // The fine grid's cells are 1 m and its water cells 4 m: these steps see every one.
            for (float z = -hu.y - 1.0f; z <= hu.y + 1.01f; z += 1.0f)
                for (float x = -hu.x - 1.0f; x <= hu.x + 1.01f; x += 1.0f)
                    if (FineSurface(map, c.x + x - origin.x, c.y + z - origin.y) != LandSurface::Natural) return false;
            for (float z = -hu.y - 3.0f; z <= hu.y + 3.01f; z += 2.0f)
                for (float x = -hu.x - 3.0f; x <= hu.x + 3.01f; x += 2.0f)
                    if (LandWaterLevel(map, land, c.x + x, c.y + z) > kNoLandWater) return false;
            return true;
        };

        // Places one structure at c: of the types in `allowed` that this spot's road distance admits, the least placed
        // first (ties by the hash), so the types vary.
        const auto tryPlace = [&](glm::vec2 c, uint32_t hk, uint32_t allowed) {
            c = glm::floor(c * 2.0f) * 0.5f;
            for (const LandStructureSite& s : out.sites) {
                const glm::vec2 d = s.at - c;
                if (d.x * d.x + d.y * d.y < kSpacing * kSpacing) return false;
            }
            glm::vec2 roadAt(0.0f);
            const float roadDistance = road.empty() ? 1.0e9f : nearestRoad(c, roadAt);
            std::array<int, kLandStructureKinds> order{};
            for (int t = 0; t < kLandStructureKinds; ++t) order[size_t(t)] = t;
            std::sort(order.begin(), order.end(), [&](int x, int y) {
                if (placed[size_t(x)] != placed[size_t(y)]) return placed[size_t(x)] < placed[size_t(y)];
                return Hash32(hk + uint32_t(x) * 0x2545f491u) < Hash32(hk + uint32_t(y) * 0x2545f491u);
            });
            for (const int t : order) {
                if ((allowed >> t & 1u) == 0) continue;
                const LandStructureKind kind = LandStructureKind(t);
                const Rule& rule = kRules[size_t(t)];
                if (roadDistance < rule.roadMin || roadDistance > rule.roadMax) continue;
                int turn = int(Hash32(hk + 9u) & 3u);
                if (rule.facing == Facing::Road) {
                    const glm::vec2 d = roadAt - c;
                    turn = std::abs(d.x) >= std::abs(d.y) ? (d.x > 0.0f ? 0 : 2) : (d.y > 0.0f ? 1 : 3);
                } else if (rule.facing == Facing::Downhill) {
                    const float e = ground(c + glm::vec2(6.0f, 0.0f)), w = ground(c - glm::vec2(6.0f, 0.0f));
                    const float so = ground(c + glm::vec2(0.0f, 6.0f)), no = ground(c - glm::vec2(0.0f, 6.0f));
                    turn = std::abs(e - w) >= std::abs(so - no) ? (e < w ? 0 : 2) : (so < no ? 1 : 3);
                }
                if (!fits(kind, c, turn, roadDistance)) continue;
                LandStructureSite site;
                site.kind = kind;
                site.at = c;
                site.turn = turn;
                Builder(map, land, site).Build(hk);
                out.sites.push_back(std::move(site));
                ++placed[size_t(t)];
                return true;
            }
            return false;
        };

        // First the roadside: every 60 m of road and trail a spot on either side, at the distance one of the roadside
        // types wants (setting.md 5.7: outposts mark the roads).
        constexpr uint32_t kRoadside = 1u << int(LandStructureKind::Farmstead) | 1u << int(LandStructureKind::BurnedRow) |
                                       1u << int(LandStructureKind::ChimeTower) | 1u << int(LandStructureKind::Waystone) |
                                       1u << int(LandStructureKind::Well);
        for (size_t n = 0; n < map.roads.size(); ++n) {
            const LandRoad& r = map.roads[n];
            for (size_t k = 30; k + 1 < r.x.size(); k += 30) {
                const uint32_t hk = Hash3(int(n), int(k), 0x726f, seed);
                const glm::vec2 p = origin + glm::vec2(r.x[k], r.z[k]);
                const glm::vec2 dir(r.x[k + 1] - r.x[k - 1], r.z[k + 1] - r.z[k - 1]);
                const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (len < 1.0e-3f) continue;
                const glm::vec2 normal(-dir.y / len, dir.x / len);
                // The distances of the roadside types (a waystone, a well, a chime tower, a house), nearest first.
                static constexpr float kOut[4] = {5.0f, 7.0f, 13.0f, 19.0f};
                bool done = false;
                for (int side = 0; side < 2 && !done; ++side) {
                    const float sign = ((Hash32(hk) >> side) & 1u) != 0u ? 1.0f : -1.0f;
                    for (int d = 0; d < 4 && !done; ++d) done = tryPlace(p + normal * (sign * kOut[d]), Hash32(hk + uint32_t(side * 4 + d)), kRoadside);
                }
            }
        }

        // Then a jittered 64 m grid over the fine grid, a few tries a cell, any type.
        const int i0 = int(std::floor(fx0 / kCell)), i1 = int(std::floor(fx1 / kCell));
        const int j0 = int(std::floor(fz0 / kCell)), j1 = int(std::floor(fz1 / kCell));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const uint32_t h = Hash3(i, j, 0x7374, seed);
                for (int k = 0; k < kTries; ++k) {
                    const uint32_t hk = Hash32(h + uint32_t(k) * 0x51ed27u);
                    const glm::vec2 c((float(i) + 0.15f + 0.7f * HashToFloat(Hash32(hk + 1u))) * kCell,
                                      (float(j) + 0.15f + 0.7f * HashToFloat(Hash32(hk + 2u))) * kCell);
                    if (tryPlace(c, hk, 0xffu)) break;
                }
            }

        // The index: each site under every cell its voxel bounds meet.
        out.bucket = std::max(1, int(std::floor(32.0f / vs)));
        if (!out.sites.empty()) {
            glm::ivec2 lo(INT32_MAX), hi(INT32_MIN);
            const auto cellOf = [&](int v) { return v >= 0 ? v / out.bucket : -((-v + out.bucket - 1) / out.bucket); };
            for (const LandStructureSite& s : out.sites) {
                lo = glm::min(lo, glm::ivec2(cellOf(s.min.x), cellOf(s.min.z)));
                hi = glm::max(hi, glm::ivec2(cellOf(s.max.x - 1), cellOf(s.max.z - 1)));
            }
            out.cellMin = lo;
            out.cells = hi - lo + 1;
            out.index.resize(size_t(out.cells.x) * size_t(out.cells.y));
            for (uint32_t n = 0; n < out.sites.size(); ++n) {
                const LandStructureSite& s = out.sites[n];
                for (int cz = cellOf(s.min.z); cz <= cellOf(s.max.z - 1); ++cz)
                    for (int cx = cellOf(s.min.x); cx <= cellOf(s.max.x - 1); ++cx)
                        out.index[size_t(cx - lo.x) + size_t(cz - lo.y) * size_t(out.cells.x)].push_back(n);
            }
        }
        return out;
    }

    uint64_t LandStructuresHash(const LandStructures& structures) {
        uint64_t h = Mix(0x77617973ull ^ structures.sites.size());
        const auto put = [&](uint64_t v) { h = Mix(h ^ v) + 0x9e3779b97f4a7c15ull; };
        for (const LandStructureSite& s : structures.sites) {
            put(uint64_t(s.kind) | uint64_t(uint32_t(s.turn)) << 8);
            for (const float f : {s.at.x, s.at.y, s.ground, s.radius}) put(std::bit_cast<uint32_t>(f));
            put(FortPlanHash(s.plan));
        }
        return h;
    }

    void LandStructuresIn(const LandStructures& structures, int x0, int z0, int x1, int z1, std::vector<uint32_t>& out) {
        out.clear();
        if (structures.index.empty()) return;
        const int b = structures.bucket;
        const auto cellOf = [b](int v) { return v >= 0 ? v / b : -((-v + b - 1) / b); };
        const int cx0 = std::max(cellOf(x0) - structures.cellMin.x, 0), cx1 = std::min(cellOf(x1 - 1) - structures.cellMin.x, structures.cells.x - 1);
        const int cz0 = std::max(cellOf(z0) - structures.cellMin.y, 0), cz1 = std::min(cellOf(z1 - 1) - structures.cellMin.y, structures.cells.y - 1);
        for (int cz = cz0; cz <= cz1; ++cz)
            for (int cx = cx0; cx <= cx1; ++cx)
                for (const uint32_t n : structures.index[size_t(cx) + size_t(cz) * size_t(structures.cells.x)]) {
                    const LandStructureSite& s = structures.sites[n];
                    if (s.max.x <= x0 || s.min.x >= x1 || s.max.z <= z0 || s.min.z >= z1) continue;
                    out.push_back(n);
                }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    bool LandStructureRows(const LandStructures& structures, int x0, int z0, int x1, int z1, int& lo, int& hi) {
        std::vector<uint32_t> near;
        LandStructuresIn(structures, x0, z0, x1, z1, near);
        bool any = false;
        for (const uint32_t n : near) {
            int a = 0, b = 0;
            if (!FortRows(structures.sites[n].plan, x0, z0, x1, z1, a, b)) continue;
            lo = any ? std::min(lo, a) : a;
            hi = any ? std::max(hi, b) : b;
            any = true;
        }
        return any;
    }

    bool NearLandStructure(const LandStructures& structures, float voxelSize, float x, float z, float margin) {
        if (structures.index.empty()) return false;
        const int vx0 = int(std::floor((x - margin) / voxelSize)), vx1 = int(std::floor((x + margin) / voxelSize)) + 1;
        const int vz0 = int(std::floor((z - margin) / voxelSize)), vz1 = int(std::floor((z + margin) / voxelSize)) + 1;
        std::vector<uint32_t> near;
        LandStructuresIn(structures, vx0, vz0, vx1, vz1, near);
        return !near.empty();
    }

    void StampLandStructures(const LandStructures& structures, const WorldMaterials& materials, float voxelSize, const glm::ivec3& coord,
                             const float* ground, Chunk& chunk) {
        std::vector<uint32_t> near;
        const glm::ivec3 c0 = coord * kChunkSize;
        LandStructuresIn(structures, c0.x, c0.z, c0.x + kChunkSize, c0.z + kChunkSize, near);
        for (const uint32_t n : near) {
            const LandStructureSite& s = structures.sites[n];
            if (s.max.y <= c0.y || s.min.y >= c0.y + kChunkSize) continue;
            StampFort(s.plan, materials, voxelSize, coord, ground, chunk);
        }
    }
}
