module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <tuple>
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
        constexpr uint64_t kPropagateVisitWork = 1;
        constexpr uint32_t kCastFxBytes = 32;
        constexpr uint32_t kZoneCancelBytes = 7;

        int64_t Dot(const glm::i64vec3& a, const glm::i64vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

        int64_t Length(const glm::i64vec3& d) { return static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(Dot(d, d)))); }

        /// The point minus the nearest point of the segment a-b (q; as ApplyZones computes it).
        glm::i64vec3 FromSegment(const glm::i64vec3& p, const glm::i64vec3& a, const glm::i64vec3& b) {
            const glm::i64vec3 u = b - a;
            const int64_t uu = Dot(u, u);
            if (uu == 0) return p - a;
            const int64_t t = std::clamp<int64_t>(Dot(p - a, u), 0, uu);
            return p - (a + u * t / uu);
        }

        double QPerMetre(const voxel::World& world) { return 16.0 / static_cast<double>(world.VoxelSize()); }

        glm::ivec3 ToQ(const glm::dvec3& metres, double q) {
            return {static_cast<int32_t>(std::llround(metres.x * q)), static_cast<int32_t>(std::llround(metres.y * q)),
                    static_cast<int32_t>(std::llround(metres.z * q))};
        }

        bool InsideWalls(const OperatorWalls& walls, const glm::ivec3& v) {
            return glm::all(glm::greaterThanEqual(v, walls.min)) && glm::all(glm::lessThan(v, walls.max));
        }

        bool BoxesMeet(const glm::ivec3& aMin, const glm::ivec3& aMax, const glm::ivec3& bMin, const glm::ivec3& bMax) {
            return glm::all(glm::lessThan(aMin, bMax)) && glm::all(glm::lessThan(bMin, aMax));
        }

        /// The voxel box [min, max) that holds a zone's shape.
        void ZoneVoxelBox(const Zone& zone, glm::ivec3& min, glm::ivec3& max) {
            glm::ivec3 lo, hi;
            ZoneBoundsQ(zone, lo, hi);
            min = lo >> 4;
            max = (hi >> 4) + glm::ivec3(1);
        }

        uint8_t TeamBit(TeamId team) { return team < 8 ? static_cast<uint8_t>(1u << team) : uint8_t{0}; }

        /// The squared distance (q^2) from `p` to a zone's centre: a sphere's centre, the segment of a
        /// capsule, the axis of a y-cylinder (a tall Updraft column is dispelled from anywhere along it),
        /// the box of a box.
        int64_t ZoneCentreDistanceSq(const Zone& zone, const glm::i64vec3& p) {
            const voxel::ShapeQ& s = zone.shape;
            glm::i64vec3 d;
            switch (s.kind) {
            case voxel::kShapeCapsule: d = FromSegment(p, glm::i64vec3(s.a), glm::i64vec3(s.b)); break;
            case voxel::kShapeCylinder: {
                const glm::i64vec3 half(0, s.halfHeightQ, 0);
                d = FromSegment(p, glm::i64vec3(s.a) - half, glm::i64vec3(s.a) + half);
                break;
            }
            case voxel::kShapeBox: d = p - glm::clamp(p, glm::i64vec3(s.a) * int64_t{16}, glm::i64vec3(s.b) * int64_t{16}); break;
            default: d = p - glm::i64vec3(s.a); break;
            }
            return Dot(d, d);
        }

        /// What a zone does to a body at `p` (q), cm/s, with the distance used to order the bodies. False: outside.
        bool ZoneKick(const Zone& zone, const glm::i64vec3& p, int64_t rampQ, glm::i64vec3& kick, int64_t& len) {
            const voxel::ShapeQ& sh = zone.shape;
            const glm::i64vec3 a(sh.a), b(sh.b);
            const int64_t R = sh.radiusQ;
            if (R <= 0) return false;
            kick = glm::i64vec3(0);
            if (zone.kind == ZoneKind::Lift) {
                if (sh.kind != voxel::kShapeCylinder) return false;
                const int64_t dx = p.x - a.x, dz = p.z - a.z;
                len = static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(dx * dx + dz * dz)));
                if (len > R || std::abs(p.y - a.y) > int64_t{sh.halfHeightQ}) return false;
                const int64_t s0 = std::max<int64_t>(zone.strength[0], 0);
                const int64_t ramp = std::min(R - len, rampQ);
                kick.y = rampQ > 0 ? s0 * ramp / rampQ : s0;
                return true;
            }
            if (zone.kind != ZoneKind::Impulse && zone.kind != ZoneKind::Shove) return false;
            glm::i64vec3 d;
            if (sh.kind == voxel::kShapeSphere) {
                d = p - a;
            } else if (sh.kind == voxel::kShapeCylinder) {
                if (std::abs(p.y - a.y) > int64_t{sh.halfHeightQ}) return false;
                d = glm::i64vec3(p.x - a.x, 0, p.z - a.z);
            } else if (sh.kind == voxel::kShapeCapsule) {
                d = FromSegment(p, a, b);
            } else {
                return false;
            }
            len = Length(d);
            if (len >= R) return false;
            const int64_t f = int64_t{zone.strength[0]} * (R - len) / R;
            if (zone.kind == ZoneKind::Impulse) {
                const int64_t over = std::max<int64_t>(len, std::max<int64_t>(R / 8, 1));
                kick.x = d.x * f / over;
                kick.z = d.z * f / over;
            } else if (sh.kind == voxel::kShapeCapsule) {
                const glm::i64vec3 u = b - a;
                const int64_t ul = static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(u.x * u.x + u.z * u.z)));
                if (ul > 0) {
                    kick.x = u.x * f / ul;
                    kick.z = u.z * f / ul;
                }
            }
            kick.y = int64_t{zone.strength[1]} * (R - len) / R;
            return true;
        }

        /// The ray's first meeting with an upright capsule (feet, radius r, height h); -1 if none within maxT.
        double RaySphere(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& c, double r, double maxT) {
            const glm::dvec3 m = o - c;
            const double b = glm::dot(m, d), cc = glm::dot(m, m) - r * r;
            if (cc > 0.0 && b > 0.0) return -1.0;
            const double disc = b * b - cc;
            if (disc < 0.0) return -1.0;
            const double t = std::max(0.0, -b - std::sqrt(disc));
            return t <= maxT ? t : -1.0;
        }

        double RayCapsule(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& feet, double r, double h, double maxT) {
            const double y0 = feet.y + r, y1 = feet.y + std::max(h - r, r);
            double best = -1.0;
            const auto take = [&best](double t) {
                if (t >= 0.0 && (best < 0.0 || t < best)) best = t;
            };
            const double mx = o.x - feet.x, mz = o.z - feet.z;
            const double a = d.x * d.x + d.z * d.z;
            const double cc = mx * mx + mz * mz - r * r;
            if (a > 1.0e-12) {
                const double b = mx * d.x + mz * d.z;
                const double disc = b * b - a * cc;
                if (disc >= 0.0) {
                    const double t = cc <= 0.0 ? 0.0 : (-b - std::sqrt(disc)) / a;
                    const double y = o.y + t * d.y;
                    if (t <= maxT && y >= y0 && y <= y1) take(t);
                }
            }
            take(RaySphere(o, d, glm::dvec3(feet.x, y0, feet.z), r, maxT));
            take(RaySphere(o, d, glm::dvec3(feet.x, y1, feet.z), r, maxT));
            return best;
        }

        /// A voxel a player's capsule meets: not air, not a liquid, not foliage (TerrainColliders' rule);
        /// the operator's walls count as solid.
        bool BlocksPlayers(const voxel::MaterialLibrary& materials, uint8_t id) {
            if (id == VoxelCache::kWallId) return true;
            if (id == 0) return false;
            return (materials.Get(voxel::kWorldPalette, id).flags & (voxel::MaterialFlag_Liquid | voxel::MaterialFlag_Foliage)) == 0;
        }

        /// The voxels whose box overlaps `box` grown by `grow` metres.
        bool VoxelInBox(const glm::ivec3& v, double vs, const math::AABB& box, double grow) {
            for (int i = 0; i < 3; ++i) {
                const double lo = v[i] * vs, hi = (v[i] + 1) * vs;
                if (hi <= static_cast<double>(box.min[i]) - grow || lo >= static_cast<double>(box.max[i]) + grow) return false;
            }
            return true;
        }

        /// What a fill or a band must leave free at this tick: the players' capsules grown by one voxel and
        /// the boxes of the rigid bodies near `cellsMin`..`cellsMax` (voxels, inclusive).
        struct FillGuard {
            std::vector<math::AABB> players;
            std::vector<math::AABB> bodies;
            double vs = 0.125;

            FillGuard(Simulation& simulation, const PlayerTuning& tuning, std::span<const SpellTarget> targets, const glm::ivec3& cellsMin,
                      const glm::ivec3& cellsMax) {
                vs = static_cast<double>(simulation.World().VoxelSize());
                for (const SpellTarget& t : targets) players.push_back(PlayerBounds(t.feet, tuning));
                const math::AABB box(glm::vec3(glm::dvec3(cellsMin) * vs), glm::vec3(glm::dvec3(cellsMax + glm::ivec3(1)) * vs));
                std::vector<scene::Entity> near;
                simulation.Scene().QueryBodies(box, near);
                physics::PhysicsWorld& physics = simulation.Physics();
                for (const scene::Entity e : near) {
                    const scene::RigidBody* rigid = simulation.Scene().TryGet<scene::RigidBody>(e);
                    if (rigid != nullptr && physics.IsValid(rigid->body)) bodies.push_back(physics.GetBounds(rigid->body));
                }
            }

            bool Takes(const glm::ivec3& v) const {
                for (const math::AABB& p : players)
                    if (VoxelInBox(v, vs, p, vs)) return true;
                for (const math::AABB& b : bodies)
                    if (VoxelInBox(v, vs, b, 0.0)) return true;
                return false;
            }
        };

        enum class SpawnCell : uint8_t { Free, Own, Enemy };

        SpawnCell SpawnCellOf(std::span<const SpawnArea> spawns, TeamId team, const glm::ivec3& v) {
            SpawnCell cell = SpawnCell::Free;
            for (const SpawnArea& s : spawns) {
                if (!glm::all(glm::greaterThanEqual(v, s.min)) || !glm::all(glm::lessThan(v, s.max))) continue;
                if (s.team != team) return SpawnCell::Enemy;
                cell = SpawnCell::Own;
            }
            return cell;
        }

        bool MeetsEnemySpawn(std::span<const SpawnArea> spawns, TeamId team, const glm::ivec3& min, const glm::ivec3& max) {
            if (!glm::all(glm::lessThan(min, max))) return false;
            for (const SpawnArea& s : spawns)
                if (s.team != team && BoxesMeet(min, max, s.min, s.max)) return true;
            return false;
        }

        uint32_t ZoneStartBytes(const Zone& zone) {
            if (zone.shape.kind == voxel::kShapeCapsule) return 42;
            if (zone.shape.kind == voxel::kShapeCylinder) return 32;
            return 30;
        }

        uint16_t TakeZoneId(CastContext& ctx, const ZoneSet& zones) {
            for (uint32_t tries = 0; tries < 65535u; ++tries) {
                const uint16_t id = ctx.nextZoneId == 0 ? uint16_t{1} : ctx.nextZoneId;
                ctx.nextZoneId = static_cast<uint16_t>(id == 65535 ? 1 : id + 1);
                if (zones.Find(id) == nullptr && ctx.zoneOwners.find(id) == ctx.zoneOwners.end()) return id;
            }
            return 0;
        }

        void Schedule(CastContext& ctx, ScheduledAction action) {
            action.seq = ctx.nextSeq++;
            const auto at = std::upper_bound(ctx.schedule.begin(), ctx.schedule.end(), action, [](const ScheduledAction& a, const ScheduledAction& b) {
                return std::tie(a.tick, a.seq) < std::tie(b.tick, b.seq);
            });
            ctx.schedule.insert(at, std::move(action));
        }

        BlastResult GroupOf(AppliedOp applied) {
            BlastResult group;
            group.islandsSearched = true;
            group.ops.push_back(std::move(applied));
            return group;
        }

        /// The ray of a cast that needs a hit face: terrain, or a Dynamic / Rubble body in front of it.
        struct FaceHit {
            bool hit = false;
            glm::dvec3 point{0.0};
            glm::ivec3 normal{0, 1, 0};
            glm::ivec3 seed{0};   ///< s0: the cell in front of the hit face
        };

        glm::ivec3 AxisNormal(const glm::vec3& n) {
            const glm::vec3 a = glm::abs(n);
            if (a.x >= a.y && a.x >= a.z) return {n.x < 0.0f ? -1 : 1, 0, 0};
            if (a.y >= a.z) return {0, n.y < 0.0f ? -1 : 1, 0};
            return {0, 0, n.z < 0.0f ? -1 : 1};
        }

        FaceHit CastFaceRay(Simulation& simulation, const SpellCast& cast, float range) {
            FaceHit out;
            const voxel::World& world = simulation.World();
            const voxel::WorldRayHit terrain = world.Raycast(cast.eye, cast.dir, range);
            const float bodyMax = terrain.hit ? terrain.distance : range;
            const uint8_t layers = physics::LayerBit(physics::Layer::Dynamic) | physics::LayerBit(physics::Layer::Rubble);
            const physics::RayHit body = simulation.Physics().Raycast(glm::vec3(cast.eye), glm::normalize(cast.dir), bodyMax, layers);
            if (body.hit && (!terrain.hit || body.distance < terrain.distance)) {
                out.hit = true;
                out.point = glm::dvec3(body.position);
                out.normal = AxisNormal(body.normal);
                const double vs = static_cast<double>(world.VoxelSize());
                out.seed = world.WorldToVoxel(out.point + glm::dvec3(body.normal) * (0.5 * vs));
                return out;
            }
            if (!terrain.hit) return out;
            out.hit = true;
            out.point = terrain.position;
            out.normal = AxisNormal(terrain.normal);
            out.seed = terrain.voxel + out.normal;
            return out;
        }

        uint8_t FaceOf(const glm::ivec3& n) {
            for (int axis = 0; axis < 3; ++axis)
                if (n[axis] != 0) return static_cast<uint8_t>(axis * 2 + (n[axis] < 0 ? 1 : 0));
            return 0;
        }

        glm::ivec3 AimOf(const glm::vec3& dir) {
            const glm::dvec3 d = glm::normalize(glm::dvec3(dir));
            return {static_cast<int32_t>(std::lround(d.x * 16384.0)), static_cast<int32_t>(std::lround(d.y * 16384.0)),
                    static_cast<int32_t>(std::lround(d.z * 16384.0))};
        }

        uint8_t DispelKinds(ZoneKind kind) { return kind == ZoneKind::Lift || kind == ZoneKind::Chill ? 1u : 0u; }

        bool IsHeld(const PlayerState& state) {
            return state.rootLeft > 0 || state.pinLeft > 0 || (state.status & (PlayerStatus_Rooted | PlayerStatus_Pinned)) != 0;
        }

        /// The Dash's path from `feet` along `dir` (unit, horizontal): the capsule (lifted by the step
        /// height, which the move climbs) in steps of `stepM`, at most `maxSteps`; the steps kept before
        /// the first that meets a collider.
        uint32_t TraceDash(const voxel::World& world, const OperatorWalls& walls, const voxel::MaterialLibrary& materials,
                           const PlayerTuning& tuning, const glm::dvec3& feet, const glm::dvec2& dir, double stepM, uint32_t maxSteps,
                           uint32_t& visits, uint32_t& chunksRead) {
            VoxelCache cache(world, walls);
            const double vs = static_cast<double>(world.VoxelSize());
            const double r = tuning.radius;
            const double y0 = feet.y + static_cast<double>(tuning.stepHeight) + r;
            const double y1 = std::max(y0, feet.y + static_cast<double>(tuning.height) - r);
            uint32_t kept = 0;
            for (uint32_t k = 1; k <= maxSteps; ++k) {
                ++visits;
                const double cx = feet.x + dir.x * stepM * k, cz = feet.z + dir.y * stepM * k;
                const glm::ivec3 lo(static_cast<int>(std::floor((cx - r) / vs)), static_cast<int>(std::floor((y0 - r) / vs)),
                                    static_cast<int>(std::floor((cz - r) / vs)));
                const glm::ivec3 hi(static_cast<int>(std::floor((cx + r) / vs)), static_cast<int>(std::floor((y1 + r) / vs)),
                                    static_cast<int>(std::floor((cz + r) / vs)));
                bool meets = false;
                for (int z = lo.z; z <= hi.z && !meets; ++z)
                    for (int y = lo.y; y <= hi.y && !meets; ++y)
                        for (int x = lo.x; x <= hi.x && !meets; ++x) {
                            const double dx = std::max({x * vs - cx, 0.0, cx - (x + 1) * vs});
                            const double dz = std::max({z * vs - cz, 0.0, cz - (z + 1) * vs});
                            const double dy = std::max({y * vs - y1, 0.0, y0 - (y + 1) * vs});
                            if (dx * dx + dy * dy + dz * dz >= r * r) continue;
                            meets = BlocksPlayers(materials, cache.Get({x, y, z}));
                        }
                if (meets) break;
                kept = k;
            }
            chunksRead = cache.ChunksRead();
            return kept;
        }

        /// One planned zone, whether a Punch may end it, and its damage at its tick.
        struct PlannedZone {
            Zone zone;
            bool dispellable = false;
            uint16_t damage = 0;
        };

        /// An op applied at T_run, and the registry owner (an index into the planned owners) that records it.
        struct PlannedOp {
            WorldOp op;
            int owner = -1;
        };

        /// A band of conjured matter, run at T_run + max(flight + `along`, `minTicks`) + `extra` (the guard runs again
        /// on its tick), and the registry owner (an index into the planned owners) that records it.
        struct PlannedBand {
            WorldOp op;
            uint32_t along = 0;
            uint32_t minTicks = 0;
            uint32_t extra = 0;
            int owner = -1;
        };

        /// A voxel the operators stop at and a fill rests on: not open (a wall reads as support).
        bool IsSupportId(uint8_t id) { return !IsOpenId(id); }

        /// The voxel box [min, max) of `cells` (empty for none).
        void CellBox(std::span<const glm::ivec3> cells, glm::ivec3& min, glm::ivec3& max) {
            if (cells.empty()) {
                min = max = glm::ivec3(0);
                return;
            }
            min = glm::ivec3(std::numeric_limits<int32_t>::max());
            max = glm::ivec3(std::numeric_limits<int32_t>::min());
            for (const glm::ivec3& c : cells) {
                min = glm::min(min, c);
                max = glm::max(max, c + glm::ivec3(1));
            }
        }

        /// Ticks to travel `distQ` (q) at `speedMs`: ceil(dist * 60 / speed); 0 for a speed of 0.
        uint32_t TravelTicks(const SpellTable& table, uint64_t distQ, uint32_t speedMs) {
            if (speedMs == 0) return 0;
            const uint64_t per = uint64_t{speedMs} * std::max<uint32_t>(table.metreQ, 1);
            return static_cast<uint32_t>(std::min<uint64_t>((distQ * 60 + per - 1) / per, 0xFFFFFFu));
        }

        int64_t SegmentQ(const glm::ivec3& a, const glm::ivec3& b) {
            const glm::i64vec3 d = glm::i64vec3(b) - glm::i64vec3(a);
            return static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(Dot(d, d))));
        }

        /// The point `num / den` of the way from a to b (q, rounded toward a).
        glm::ivec3 Along(const glm::ivec3& a, const glm::ivec3& b, int64_t num, int64_t den) {
            const glm::i64vec3 d = glm::i64vec3(b) - glm::i64vec3(a);
            return a + glm::ivec3(d * num / std::max<int64_t>(den, 1));
        }

        /// The polyline without repeated points, cut at `maxQ` of length and, when shorter than `minQ`, its last
        /// segment extended to reach it. `lengthQ`: the length of what is returned (floored segment lengths).
        std::vector<glm::ivec3> ClampPolyline(std::span<const glm::ivec3> points, uint32_t minQ, uint32_t maxQ, uint32_t& lengthQ) {
            std::vector<glm::ivec3> out;
            lengthQ = 0;
            int64_t total = 0;
            for (const glm::ivec3& p : points) {
                if (!out.empty() && p == out.back()) continue;
                if (out.empty()) {
                    out.push_back(p);
                    continue;
                }
                const int64_t len = SegmentQ(out.back(), p);
                if (maxQ > 0 && total + len > int64_t{maxQ}) {
                    const glm::ivec3 cut = Along(out.back(), p, int64_t{maxQ} - total, len);
                    if (cut != out.back()) {
                        total += SegmentQ(out.back(), cut);
                        out.push_back(cut);
                    }
                    break;
                }
                out.push_back(p);
                total += len;
            }
            if (out.size() >= 2 && total < int64_t{minQ}) {
                const glm::ivec3 a = out[out.size() - 2], b = out.back();
                const int64_t len = SegmentQ(a, b);
                if (len > 0) {
                    out.back() = Along(a, b, len + int64_t{minQ} - total, len);
                    total += SegmentQ(a, out.back()) - len;
                }
            }
            lengthQ = static_cast<uint32_t>(std::max<int64_t>(total, 0));
            return out;
        }

        /// A voxel that is not air within `radiusQ` of the point `p` (q): the 27 voxels round it first.
        bool NearSupport(VoxelCache& cache, const glm::ivec3& p, uint32_t radiusQ) {
            const glm::ivec3 v = p >> 4;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (IsSupportId(cache.Get(v + glm::ivec3(dx, dy, dz)))) return true;
            const int32_t reach = static_cast<int32_t>((radiusQ + 15) / 16);
            const int64_t r2 = int64_t{radiusQ} * radiusQ;
            for (int dz = -reach; dz <= reach; ++dz)
                for (int dy = -reach; dy <= reach; ++dy)
                    for (int dx = -reach; dx <= reach; ++dx) {
                        const glm::ivec3 c = v + glm::ivec3(dx, dy, dz);
                        int64_t d2 = 0;
                        for (int a = 0; a < 3; ++a) {
                            const int64_t lo = int64_t{c[a]} * 16, hi = lo + 16;
                            const int64_t d = std::max<int64_t>({lo - p[a], 0, int64_t{p[a]} - hi});
                            d2 += d * d;
                        }
                        if (d2 <= r2 && IsSupportId(cache.Get(c))) return true;
                    }
            return false;
        }

        /// One column of a drawn shape: its ground voxel and how far along the line it lies (q).
        struct DrawnColumn {
            glm::ivec3 ground{0};
            size_t segment = 0;
        };

        /**
         * The columns of a shape drawn along `line` (q): samples every half voxel along each segment, and across it
         * (horizontally) every voxel within `halfWidthQ`; each column once, for the first segment that reaches it.
         * Its ground: the topmost solid voxel under the sample point, searched up `upVoxels` from the point's voxel
         * when that is solid, else down at most `downVoxels`. A column with no ground in reach is left out.
         */
        std::vector<DrawnColumn> DrawnColumns(VoxelCache& cache, std::span<const glm::ivec3> line, uint32_t halfWidthQ, int32_t upVoxels,
                                              int32_t downVoxels, uint32_t& visits) {
            std::vector<DrawnColumn> out;
            std::set<std::pair<int32_t, int32_t>> taken;
            glm::i64vec3 lastPerp(1, 0, 0);
            int64_t lastPerpLen = 1;
            for (size_t s = 0; s + 1 < line.size(); ++s) {
                const glm::ivec3 a = line[s], b = line[s + 1];
                const int64_t len = SegmentQ(a, b);
                glm::i64vec3 perp(-(int64_t{b.z} - a.z), 0, int64_t{b.x} - a.x);
                int64_t perpLen = static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(perp.x * perp.x + perp.z * perp.z)));
                if (perpLen == 0) {
                    perp = lastPerp;
                    perpLen = lastPerpLen;
                }
                lastPerp = perp;
                lastPerpLen = perpLen;
                const int64_t samples = std::max<int64_t>(1, (len + 7) / 8);
                const int64_t hw = halfWidthQ;
                for (int64_t k = 0; k <= samples; ++k) {
                    const glm::ivec3 p = Along(a, b, k, samples);
                    for (int64_t o = hw >= 8 ? -hw + 8 : 0; o <= std::max<int64_t>(hw - 8, 0); o += 16) {
                        const glm::ivec3 q = p + glm::ivec3(perp * o / perpLen);
                        const glm::ivec3 v = q >> 4;
                        if (!taken.emplace(v.x, v.z).second) continue;
                        ++visits;
                        glm::ivec3 g = v;
                        bool found = false;
                        if (IsSupportId(cache.Get(g))) {
                            for (int32_t i = 0; i < upVoxels && IsSupportId(cache.Get(g + glm::ivec3(0, 1, 0))); ++i) ++g.y;
                            found = true;
                        } else {
                            for (int32_t i = 0; i < downVoxels && !found; ++i) {
                                --g.y;
                                found = IsSupportId(cache.Get(g));
                            }
                        }
                        if (found && cache.Get(g) != VoxelCache::kWallId) out.push_back({g, s});
                    }
                }
            }
            return out;
        }
    }

    const char* ToString(SpellRefusal refusal) {
        switch (refusal) {
        case SpellRefusal::None: return "none";
        case SpellRefusal::NotBuilt: return "not built";
        case SpellRefusal::TooSoon: return "too soon";
        case SpellRefusal::NotAttuned: return "not attuned";
        case SpellRefusal::Missed: return "missed";
        case SpellRefusal::Outside: return "outside";
        case SpellRefusal::TooMuchConjured: return "too much conjured";
        case SpellRefusal::TooManyZones: return "too many zones";
        case SpellRefusal::NearSpawn: return "near spawn";
        case SpellRefusal::Rooted: return "rooted";
        case SpellRefusal::NoMana: return "no mana";
        case SpellRefusal::BadGesture: return "bad gesture";
        }
        return "?";
    }

    uint32_t ZoneStartTick(const SpellTable& table, uint32_t runTick) { return runTick + table.zoneLeadTicks; }

    uint8_t ZoneTeamMask(ZoneTarget target, TeamId casterTeam) {
        switch (target) {
        case ZoneTarget::Everyone: return 0xFF;
        case ZoneTarget::Allies: return TeamBit(casterTeam);
        case ZoneTarget::Enemies: break;
        }
        return static_cast<uint8_t>(0xFF & ~TeamBit(casterTeam));
    }

    bool ZoneHoldsFeet(const Zone& zone, const glm::ivec3& feetQ) {
        glm::i64vec3 kick;
        int64_t len = 0;
        const voxel::ShapeQ& sh = zone.shape;
        if (zone.kind == ZoneKind::Impulse || zone.kind == ZoneKind::Shove || zone.kind == ZoneKind::Lift) {
            if (zone.kind == ZoneKind::Shove && sh.kind != voxel::kShapeCapsule) return false;
            return ZoneKick(zone, glm::i64vec3(feetQ), 0, kick, len);
        }
        // Kinds with no push: inside the shape.
        return ZoneShapeHolds(zone, feetQ);
    }

    SpellRefusal CheckCapture(const CastContext& ctx, const SpellTable& table, const SpellCast& cast, uint32_t tick) {
        if (cast.spell >= 16 || !table.spells[cast.spell].built) return SpellRefusal::NotBuilt;
        CasterState caster;
        if (const auto it = ctx.casters.find(cast.player); it != ctx.casters.end()) caster = it->second;
        const uint8_t element = static_cast<uint8_t>(ElementOf(cast.spell));
        if (element != caster.elementA && element != caster.elementB) return SpellRefusal::NotAttuned;
        if (tick < caster.spellReadyTick) return SpellRefusal::TooSoon;
        return SpellRefusal::None;
    }

    void TakeCapture(CastContext& ctx, const SpellTable& table, const SpellCast& cast, uint32_t tick) {
        ctx.casters[cast.player].spellReadyTick = tick + table.spellGapTicks;
    }

    uint16_t ManaAt(const SpellTable& table, const CasterState& caster, uint32_t tick) {
        const uint64_t elapsed = tick > caster.manaTick ? tick - caster.manaTick : 0u;
        return static_cast<uint16_t>(std::min<uint64_t>(uint64_t{caster.mana} + elapsed * table.manaPerTick, table.manaPool));
    }

    void RefillMana(const SpellTable& table, CasterState& caster, uint32_t tick) {
        caster.mana = table.manaPool;
        caster.manaTick = tick;
    }

    uint32_t DrawnLengthQ(const SpellTable& table, SpellId spell, const SpellGesture& gesture) {
        if (spell >= kSpellCount || !table.spells[spell].drawsLine || gesture.kind != GestureKind::Line || gesture.pointsQ.size() < 2) return 0;
        const SpellDef& def = table.spells[spell];
        uint32_t length = 0;
        ClampPolyline(gesture.pointsQ, def.drawMinQ, def.drawMaxQ, length);
        return length;
    }

    uint32_t SpellManaCost(const SpellTable& table, SpellId spell, const SpellGesture& gesture) {
        if (spell >= kSpellCount) return 0;
        const uint32_t length = DrawnLengthQ(table, spell, gesture);
        if (length == 0) return table.spells[spell].manaCost;
        const uint32_t metre = std::max<uint32_t>(table.metreQ, 1);
        return table.drawBaseMana + static_cast<uint32_t>(uint64_t{table.drawManaPerMetre} * length / metre);
    }

    SpellRefusal CheckGesture(const voxel::World& world, const OperatorWalls& walls, const SpellTable& table, const glm::ivec3& eyeQ,
                              const SpellGesture& gesture, uint32_t* chunksRead) {
        const std::vector<glm::ivec3>& p = gesture.pointsQ;
        if (p.size() > kMaxGesturePoints || gesture.form > kAutoForm || static_cast<uint8_t>(gesture.kind) > kMaxGestureKind)
            return SpellRefusal::BadGesture;
        const int64_t reach = table.gestureReachQ, step = table.gestureStepQ;
        for (size_t i = 0; i < p.size(); ++i) {
            const glm::i64vec3 fromEye = glm::i64vec3(p[i]) - glm::i64vec3(eyeQ);
            if (Dot(fromEye, fromEye) > reach * reach) return SpellRefusal::BadGesture;
            if (i == 0) continue;
            const glm::i64vec3 d = glm::i64vec3(p[i]) - glm::i64vec3(p[i - 1]);
            if (Dot(d, d) > step * step) return SpellRefusal::BadGesture;
        }
        if (GesturePathQ(p) > table.gesturePathQ) return SpellRefusal::BadGesture;
        const size_t supported = gesture.kind == GestureKind::Line ? p.size() : gesture.kind == GestureKind::Rise ? std::min<size_t>(p.size(), 1) : 0;
        if (supported == 0) return SpellRefusal::None;
        VoxelCache cache(world, walls);
        bool ok = true;
        for (size_t i = 0; i < supported && ok; ++i) ok = NearSupport(cache, p[i], table.gestureSupportQ);
        if (chunksRead != nullptr) *chunksRead += cache.ChunksRead();
        return ok ? SpellRefusal::None : SpellRefusal::BadGesture;
    }

    uint8_t FlightTicks(const SpellTable& table, const glm::ivec3& fromQ, const glm::ivec3& toQ, uint32_t speedMs, bool horizontal) {
        if (speedMs == 0) return 0;
        glm::i64vec3 d = glm::i64vec3(toQ) - glm::i64vec3(fromQ);
        if (horizontal) d.y = 0;
        return static_cast<uint8_t>(std::min<uint32_t>(TravelTicks(table, math::ISqrt64(static_cast<uint64_t>(Dot(d, d))), speedMs), table.maxFlightTicks));
    }

    CastSpellResult CastSpell(Simulation& simulation, const SpellTable& table, const SpellCast& request, CastContext& ctx,
                              std::span<const SpellTarget> targets) {
        const auto started = std::chrono::steady_clock::now();
        CastSpellResult out;
        const uint32_t tRun = simulation.Tick();
        // The zone tick waits for the flight too: set once the target is known (every zone takes it then).
        uint32_t tZ = ZoneStartTick(table, tRun);
        SpellCast cast = request;
        const SpellGesture& gesture = request.gesture;
        out.runTick = tRun;
        out.zoneTick = tZ;
        const auto finish = [&out, started](SpellRefusal refusal) {
            out.refusal = refusal;
            out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            return std::move(out);
        };
        if (cast.spell >= 16 || !table.spells[cast.spell].built) return finish(SpellRefusal::NotBuilt);

        voxel::World& world = simulation.World();
        const voxel::MaterialLibrary& materials = simulation.Materials();
        const SpellDef& def = table.spells[cast.spell];
        const double vs = static_cast<double>(world.VoxelSize());
        const double q = QPerMetre(world);
        const float range = ctx.tuning.castRange;
        const glm::dvec3 eyeToFeet(0.0, static_cast<double>(ctx.tuning.eyeHeight), 0.0);


        // 0. The gesture: its form is the spell's, its points pass the checks. A drawn shape follows its line; any other
        //    spell with points casts at the release point, as a tap (a Dash keeps the look its own move took).
        const glm::ivec3 eyeQ = ToQ(cast.eye, q);
        const bool hasPoints = !gesture.pointsQ.empty();
        if (hasPoints && FormOfGesture(gesture) != FormOf(cast.spell)) return finish(SpellRefusal::BadGesture);
        uint32_t gestureChunks = 0;
        if (CheckGesture(world, ctx.walls, table, eyeQ, gesture, &gestureChunks) != SpellRefusal::None) return finish(SpellRefusal::BadGesture);
        out.stats.chunksRead += gestureChunks;
        const bool line = gesture.kind == GestureKind::Line && gesture.pointsQ.size() >= 2;
        const bool drawn = line && def.drawsLine;
        if (hasPoints && !drawn && cast.spell != kDash) {
            const glm::dvec3 release = glm::dvec3(gesture.pointsQ.back()) / q;
            if (glm::length(release - cast.eye) > 1.0e-6) cast.dir = glm::vec3(glm::normalize(release - cast.eye));
        }
        std::vector<glm::ivec3> drawnLine;
        if (drawn) {
            drawnLine = ClampPolyline(gesture.pointsQ, def.drawMinQ, def.drawMaxQ, out.stats.drawnLengthQ);
            out.drawnPointsQ = drawnLine;
        }
        // A drawn shape's segment s lands its line's length before it later, at lineSpeedMs (ceil ticks).
        std::vector<uint32_t> segmentDelay;
        uint64_t alongQ = 0;
        for (size_t s = 0; s + 1 < drawnLine.size(); ++s) {
            segmentDelay.push_back(TravelTicks(table, alongQ, table.lineSpeedMs));
            alongQ += static_cast<uint64_t>(SegmentQ(drawnLine[s], drawnLine[s + 1]));
        }

        // 1. Plan: the hit, the operator, the zones and the ops; nothing is applied yet.
        std::vector<PlannedZone> zones;
        std::vector<PlannedBand> bands;
        bool blow = false;
        WorldOp blowOp;
        bool blowHasVoxel = false;
        glm::ivec3 blowVoxel{0};
        glm::dvec2 dashDir(0.0);
        std::vector<std::pair<uint16_t, uint32_t>> cancels;
        std::vector<KnitOwnerInfo> owners;
        std::vector<PlannedOp> nowOps;
        bool bore = false;
        BlastSite boreSite;
        uint64_t boreSliceWork = 0;
        bool shatter = false;
        voxel::SphereQ shatterSphere;
        bool turns = false;
        std::vector<SpellDamage> direct;
        const auto zoneFrom = [&](const ZoneDef& zd, const voxel::ShapeQ& shape) {
            Zone z;
            z.kind = zd.kind;
            z.teamMask = ZoneTeamMask(zd.target, cast.team);
            z.shape = shape;
            z.startTick = tZ;
            z.duration = zd.durationTicks;
            z.strength[0] = zd.strength[0];
            z.strength[1] = zd.strength[1];
            return z;
        };
        const auto newOwner = [&](uint8_t layer, uint8_t expiryMap, uint16_t lifetime, const voxel::ShapeQ* shape) {
            KnitOwnerInfo info;
            info.layer = layer;
            info.expiryMap = expiryMap;
            info.caster = cast.player;
            info.dueTick = tRun + lifetime;
            if (shape != nullptr) {
                info.hasShape = true;
                info.shape = *shape;
            }
            owners.push_back(info);
            return static_cast<int>(owners.size() - 1);
        };
        // The cells of a fill a player or a body may keep and none in a spawn area: false when one lies in an
        // enemy's (the cast is refused).
        const auto guardCells = [&](std::span<const glm::ivec3> cells, std::vector<glm::ivec3>& kept) {
            if (cells.empty()) return true;
            glm::ivec3 lo, hi;
            CellBox(cells, lo, hi);
            const FillGuard guard(simulation, ctx.tuning, targets, lo, hi - glm::ivec3(1));
            for (const glm::ivec3& c : cells) {
                const SpawnCell spawn = SpawnCellOf(ctx.spawns, cast.team, c);
                if (spawn == SpawnCell::Enemy) return false;
                if (spawn == SpawnCell::Free && !guard.Takes(c)) kept.push_back(c);
            }
            return true;
        };

        const auto zoneOf = [&](ZoneKind kind, const voxel::ShapeQ& shape) {
            Zone z;
            z.kind = kind;
            z.teamMask = ZoneTeamMask(def.zone.target, cast.team);
            z.shape = shape;
            z.startTick = tZ;
            z.duration = def.zone.durationTicks;
            z.strength[0] = def.zone.strength[0];
            z.strength[1] = def.zone.strength[1];
            return z;
        };

        if (drawn) {
            // A shape drawn along the polyline, on the ground under it; segment s lands segmentDelay[s] after the flight.
            out.hitQ = drawnLine.front();
            out.face = 2;
            VoxelCache cache(world, ctx.walls);
            const int32_t metreVoxels = std::max<int32_t>(1, table.metreQ / 16);
            const std::vector<DrawnColumn> columns = DrawnColumns(cache, drawnLine, def.drawHalfWidthQ, metreVoxels, 2 * metreVoxels, out.stats.visits);
            out.stats.front = static_cast<uint32_t>(columns.size());
            const size_t segments = drawnLine.size() - 1;
            std::vector<std::vector<glm::ivec3>> fill(segments), floe(segments);
            const uint8_t water = simulation.WorldMaterialIds().water;
            for (const DrawnColumn& c : columns) {
                if (cast.spell == kBarricadeWave) {
                    for (int32_t j = 1; j <= def.drawHeight; ++j) {
                        const glm::ivec3 cell = c.ground + glm::ivec3(0, j, 0);
                        if (!IsOpenId(cache.Get(cell))) break;
                        fill[c.segment].push_back(cell);
                    }
                } else if (cache.Get(c.ground) == water) {
                    // Over water the floe is the bridge: the surface voxel and the one under it.
                    floe[c.segment].push_back(c.ground);
                    if (cache.Get(c.ground - glm::ivec3(0, 1, 0)) == water) floe[c.segment].push_back(c.ground - glm::ivec3(0, 1, 0));
                } else if (IsOpenId(cache.Get(c.ground + glm::ivec3(0, 1, 0)))) {
                    fill[c.segment].push_back(c.ground + glm::ivec3(0, 1, 0));
                }
            }
            out.stats.chunksRead += cache.ChunksRead();
            int fillOwner = -1, floeOwner = -1;
            // A rising wall keeps its ghost's lead; ice lands where the line reaches.
            const uint32_t minTicks = cast.spell == kBarricadeWave ? def.bandFirstTick : 0u;
            for (size_t s = 0; s < segments; ++s) {
                std::vector<glm::ivec3> kept, keptFloe;
                if (!guardCells(fill[s], kept) || !guardCells(floe[s], keptFloe)) return finish(SpellRefusal::NearSpawn);
                if (!kept.empty()) {
                    if (fillOwner < 0) fillOwner = newOwner(0, def.expiryMap, def.lifetimeTicks, nullptr);
                    out.stats.planned += static_cast<uint32_t>(kept.size());
                    bands.push_back({MapCellsOp(def.fillMap, kept), segmentDelay[s], minTicks, 0, fillOwner});
                }
                if (!keptFloe.empty()) {
                    if (floeOwner < 0) floeOwner = newOwner(0, voxel::Map_Unknit, def.floeLifetimeTicks, nullptr);
                    out.stats.planned += static_cast<uint32_t>(keptFloe.size());
                    bands.push_back({MapCellsOp(voxel::Map_FreezeWater, keptFloe), segmentDelay[s], 0, 0, floeOwner});
                }
            }
            out.stats.path = out.stats.planned;
            if (def.zone.kind != ZoneKind::None && def.drawZoneRadiusQ > 0) {
                voxel::ShapeQ shape;
                shape.kind = voxel::kShapeCapsule;
                shape.a = drawnLine.front();
                shape.b = drawnLine.back();
                shape.radiusQ = def.drawZoneRadiusQ;
                zones.push_back({zoneFrom(def.zone, shape), DispelKinds(def.zone.kind) != 0, def.zoneDamage});
                if (def.zone2.kind != ZoneKind::None) zones.push_back({zoneFrom(def.zone2, shape), DispelKinds(def.zone2.kind) != 0, 0});
            }
        } else if (cast.spell == kUpdraft || cast.spell == kBarricadeWave) {
            const FaceHit hit = CastFaceRay(simulation, cast, range);
            if (!hit.hit) return finish(SpellRefusal::Missed);
            out.hitQ = ToQ(hit.point, q);
            out.face = FaceOf(hit.normal);
            if (!InsideWalls(ctx.walls, out.hitQ >> 4)) return finish(SpellRefusal::Outside);

            if (cast.spell == kUpdraft) {
                if (!IsOpenId(world.GetVoxel(hit.seed))) return finish(SpellRefusal::Missed);
                const PlumeColumn column = PlumeSingleColumn(world, ctx.walls, hit.seed, def.riseMax);
                out.stats.visits += column.visits;
                out.stats.chunksRead += column.chunksRead;
                out.stats.path = static_cast<uint32_t>(column.top.y - column.bottom.y);
                out.stats.front = 1;
                // One voxel under the seed cell: feet standing on the hit face rest about 2 cm in it
                // (the character's padding), and the Lift tests the quantised feet.
                const int32_t bottom = 16 * (column.bottom.y - 1), top = 16 * (column.top.y + 1);
                voxel::ShapeQ shape;
                shape.kind = voxel::kShapeCylinder;
                shape.a = {16 * hit.seed.x + 8, (bottom + top) / 2, 16 * hit.seed.z + 8};
                shape.radiusQ = def.zone.radiusQ;
                shape.halfHeightQ = static_cast<uint16_t>((top - bottom) / 2);
                zones.push_back({zoneOf(ZoneKind::Lift, shape), true});
            } else {
                BurstParams p;
                p.seed = hit.seed;
                p.axes = FaceAxesOf(hit.normal);
                p.aim = AimOf(cast.dir);
                p.halfWidth = def.burstHalfWidth;
                p.halfThickness = def.burstHalfThickness;
                p.stepMax = def.stepMax;
                p.pops = def.burstPops;
                p.visitCap = def.burstVisitCap;
                p.plane = def.burstPlane;
                p.normalMin = def.burstNormalMin;
                p.normalMax = def.burstNormalMax;
                const BurstResult strip = BurstStrip(world, ctx.walls, p);
                out.stats.visits += strip.visits;
                out.stats.chunksRead += strip.chunksRead;
                out.stats.front = static_cast<uint32_t>(strip.cells.size());
                const FillBands fill = BarricadeFill(world, ctx.walls, strip, p.axes, def.rimHeight, def.bandCount, def.burstHalfWidth);
                if (fill.cells > 0) {
                    glm::ivec3 lo(std::numeric_limits<int32_t>::max()), hi(std::numeric_limits<int32_t>::min());
                    for (const auto& band : fill.bands)
                        for (const glm::ivec3& c : band) {
                            lo = glm::min(lo, c);
                            hi = glm::max(hi, c);
                        }
                    const FillGuard guard(simulation, ctx.tuning, targets, lo, hi);
                    int bandOwner = -1;
                    for (size_t b = 0; b < fill.bands.size(); ++b) {
                        std::vector<glm::ivec3> kept;
                        for (const glm::ivec3& c : fill.bands[b]) {
                            // A fill cell in an enemy spawn refuses the cast; one in the own spawn is left out.
                            const SpawnCell spawn = SpawnCellOf(ctx.spawns, cast.team, c);
                            if (spawn == SpawnCell::Enemy) return finish(SpellRefusal::NearSpawn);
                            if (spawn == SpawnCell::Free && !guard.Takes(c)) kept.push_back(c);
                        }
                        if (kept.empty()) continue;
                        out.stats.planned += static_cast<uint32_t>(kept.size());
                        if (bandOwner < 0) bandOwner = newOwner(0, def.expiryMap, def.lifetimeTicks, nullptr);
                        bands.push_back({MapCellsOp(def.fillMap, kept), 0, def.bandFirstTick, static_cast<uint32_t>(b) * def.bandSpacingTicks, bandOwner});
                    }
                }
            }
        } else if (cast.spell == kDash) {
            PlayerState probe;
            if (!StartDash(probe, cast.dir)) return finish(SpellRefusal::Missed);
            if (cast.casterState != nullptr && IsHeld(*cast.casterState)) return finish(SpellRefusal::Rooted);
            dashDir = glm::normalize(glm::dvec2(probe.dashDir[0], probe.dashDir[1]));
            const glm::dvec3 feet = cast.eye - eyeToFeet;
            if (!InsideWalls(ctx.walls, ToQ(feet, q) >> 4)) return finish(SpellRefusal::Outside);
            const double stepM = def.dashStepQ / 16.0 * vs;
            const uint32_t maxSteps = def.dashStepQ > 0 ? def.dashRangeQ / def.dashStepQ : 0u;
            uint32_t chunksRead = 0;
            const uint32_t kept = TraceDash(world, ctx.walls, materials, ctx.tuning, feet, dashDir, stepM, maxSteps, out.stats.visits, chunksRead);
            out.stats.chunksRead += chunksRead;
            out.stats.dashSteps = kept;
            out.stats.path = kept;
            const glm::dvec3 end = feet + glm::dvec3(dashDir.x, 0.0, dashDir.y) * (stepM * kept);
            out.hitQ = ToQ(end, q);
            voxel::ShapeQ shape;
            shape.kind = voxel::kShapeCapsule;
            shape.a = ToQ(feet, q);
            shape.b = out.hitQ;
            shape.radiusQ = def.zone.radiusQ;
            zones.push_back({zoneOf(ZoneKind::Impulse, shape), false});
        } else if (cast.spell == kPunch) {
            const glm::dvec3 d = glm::normalize(glm::dvec3(cast.dir));
            const double reach = def.fistRangeQ / 16.0 * vs;
            const voxel::WorldRayHit terrain = world.Raycast(cast.eye, cast.dir, static_cast<float>(reach));
            double nearest = terrain.hit ? static_cast<double>(terrain.distance) : reach;
            bool atPlayer = false;
            for (const SpellTarget& t : targets) {
                if (t.player == cast.player || t.team == cast.team) continue;
                const double hitT = RayCapsule(cast.eye, d, glm::dvec3(t.feet), ctx.tuning.radius, ctx.tuning.height, nearest);
                if (hitT >= 0.0 && hitT < nearest) {
                    nearest = hitT;
                    atPlayer = true;
                }
            }
            const glm::dvec3 fist = terrain.hit && !atPlayer ? terrain.position : cast.eye + d * nearest;
            if (terrain.hit && !atPlayer) {
                blowHasVoxel = true;
                blowVoxel = terrain.voxel;
                out.face = FaceOf(AxisNormal(terrain.normal));
            }
            out.hitQ = ToQ(fist, q);
            if (!InsideWalls(ctx.walls, out.hitQ >> 4)) return finish(SpellRefusal::Outside);
            blow = true;
            blowOp = WorldOp::Carve(voxel::SphereQ{out.hitQ, def.blowRadiusQ}, def.blowPower);
            voxel::ShapeQ shape;
            shape.kind = voxel::kShapeCapsule;
            // A Line points the shove along its last segment.
            glm::dvec3 shove = d;
            if (line) {
                const glm::dvec3 last = glm::dvec3(glm::ivec3(gesture.pointsQ.back()) - gesture.pointsQ[gesture.pointsQ.size() - 2]);
                if (glm::length(last) > 0.0) shove = glm::normalize(last);
            }
            shape.a = ToQ(cast.eye + shove * (def.zone.nearQ / 16.0 * vs) - eyeToFeet, q);
            shape.b = ToQ(cast.eye + shove * (def.zone.farQ / 16.0 * vs) - eyeToFeet, q);
            shape.radiusQ = def.zone.radiusQ;
            zones.push_back({zoneOf(ZoneKind::Shove, shape), false, def.zoneDamage});
            if (def.release.kind == ZoneKind::Release) zones.push_back({zoneFrom(def.release, shape), false, 0});
            if (def.shatterRadiusQ > 0) {
                shatter = true;
                shatterSphere = voxel::SphereQ{out.hitQ, def.shatterRadiusQ};
            }
            // The dispel: enemy zones that last, live at T_z, centred near the fist.
            const int64_t reachQ = def.dispelRadiusQ;
            for (const Zone& z : simulation.Zones().All()) {
                const auto owner = ctx.zoneOwners.find(z.zoneId);
                if (owner == ctx.zoneOwners.end() || !owner->second.dispellable || owner->second.casterTeam == cast.team) continue;
                if (z.startTick > tZ || z.EndTick() <= tZ) continue;
                if (ZoneCentreDistanceSq(z, glm::i64vec3(out.hitQ)) > reachQ * reachQ) continue;
                cancels.emplace_back(z.zoneId, tZ);
            }
        } else if (cast.spell == kShatterSpike) {
            const glm::dvec3 d = glm::normalize(glm::dvec3(cast.dir));
            const voxel::WorldRayHit terrain = world.Raycast(cast.eye, cast.dir, range);
            double nearest = terrain.hit ? static_cast<double>(terrain.distance) : static_cast<double>(range);
            uint8_t victim = 0;
            for (const SpellTarget& t : targets) {
                if (t.player == cast.player || t.team == cast.team) continue;
                const double hitT = RayCapsule(cast.eye, d, glm::dvec3(t.feet), ctx.tuning.radius, ctx.tuning.height, nearest);
                if (hitT >= 0.0 && hitT < nearest) {
                    nearest = hitT;
                    victim = t.player;
                }
            }
            if (!terrain.hit && victim == 0) return finish(SpellRefusal::Missed);
            const glm::dvec3 point = victim != 0 ? cast.eye + d * nearest : terrain.position;
            out.hitQ = ToQ(point, q);
            if (!InsideWalls(ctx.walls, out.hitQ >> 4)) return finish(SpellRefusal::Outside);
            if (victim != 0) {
                direct.push_back(SpellDamage{victim, def.directDamage, cast.player, cast.spell});
            } else {
                out.face = FaceOf(AxisNormal(terrain.normal));
                PierceParams pp;
                pp.hitQ = out.hitQ;
                pp.aim = AimOf(cast.dir);
                pp.radiusQ = def.pierceRadiusQ;
                pp.power = def.piercePower;
                pp.budget = def.pierceBudget;
                pp.maxSlices = def.pierceMaxSlices;
                pp.boreCost = &table.boreCost;
                pp.materials = &materials;
                const PierceResult bored = Pierce(world, ctx.walls, pp);
                out.stats.visits += bored.visits;
                out.stats.chunksRead += bored.chunksRead;
                out.stats.path = bored.paidVoxels;
                out.stats.front = bored.frontVoxels;
                out.stats.boreSlices = bored.slices;
                boreSliceWork = uint64_t{bored.scanned} * kPierceSliceWork;
                if (bored.bored) {
                    bore = true;
                    boreSite = CapsuleSite(world, bored.capsule, def.piercePower, out.hitQ);
                    // The Rime capsule from the hit to the bore's end: the bore's rim and the matter past its end.
                    voxel::ShapeQ rime = bored.capsule;
                    rime.a = out.hitQ;
                    rime.radiusQ = def.rimeRadiusQ;
                    nowOps.push_back({WorldOp::Map(voxel::Map_Rime, rime), newOwner(1, voxel::Map_Thaw, def.rimeLifetimeTicks, &rime)});
                    turns = true;
                }
            }
            voxel::ShapeQ shape;
            shape.kind = voxel::kShapeCylinder;
            shape.a = out.hitQ;
            shape.radiusQ = def.zone.radiusQ;
            shape.halfHeightQ = def.zone.halfHeightQ;
            zones.push_back({zoneFrom(def.zone, shape), DispelKinds(def.zone.kind) != 0, 0});
        } else if (cast.spell == kFlashFreeze) {
            const FaceHit hit = CastFaceRay(simulation, cast, range);
            if (!hit.hit) return finish(SpellRefusal::Missed);
            out.hitQ = ToQ(hit.point, q);
            out.face = FaceOf(hit.normal);
            if (!InsideWalls(ctx.walls, out.hitQ >> 4)) return finish(SpellRefusal::Outside);
            const uint8_t water = simulation.WorldMaterialIds().water;
            const glm::ivec3 hitVoxel = hit.seed - hit.normal;
            if (world.GetVoxel(hitVoxel) == water) {
                // The floe slab: the surface's water voxel and the one under it, over the cylinder.
                voxel::ShapeQ slab;
                slab.kind = voxel::kShapeCylinder;
                slab.a = glm::ivec3(16 * hitVoxel.x + 8, 16 * hitVoxel.y, 16 * hitVoxel.z + 8);
                slab.radiusQ = def.floeRadiusQ;
                slab.halfHeightQ = 8;
                nowOps.push_back({WorldOp::Map(voxel::Map_FreezeWater, slab), newOwner(0, voxel::Map_Unknit, def.floeLifetimeTicks, &slab)});
            }
            BurstParams p;
            p.seed = hit.seed;
            p.axes = FaceAxesOf(hit.normal);
            p.aim = AimOf(cast.dir);
            p.stepMax = def.stepMax;
            p.pops = def.burstPops;
            p.visitCap = def.burstVisitCap;
            p.plane = def.burstPlane;
            p.normalMin = def.burstNormalMin;
            p.normalMax = def.burstNormalMax;
            p.discRadius = def.burstDiscRadius;
            const BurstResult disc = BurstStrip(world, ctx.walls, p);
            out.stats.visits += disc.visits;
            out.stats.chunksRead += disc.chunksRead;
            std::vector<glm::ivec3> film;
            for (size_t i = 0; i < disc.cells.size(); ++i) {
                if (disc.front[i] != 0) {
                    ++out.stats.front;
                    continue;
                }
                ++out.stats.path;
                // Over water the floe is the bridge: the film covers dry ground only.
                if (world.GetVoxel(disc.cells[i] - p.axes.n) != water) film.push_back(disc.cells[i]);
            }
            std::vector<glm::ivec3> kept;
            if (!guardCells(film, kept)) return finish(SpellRefusal::NearSpawn);
            if (!kept.empty()) {
                out.stats.planned += static_cast<uint32_t>(kept.size());
                nowOps.push_back({MapCellsOp(def.fillMap, kept), newOwner(0, def.expiryMap, def.lifetimeTicks, nullptr)});
            }
            voxel::ShapeQ shape;
            shape.kind = voxel::kShapeCylinder;
            shape.a = 16 * hit.seed + glm::ivec3(8);
            shape.radiusQ = def.zone.radiusQ;
            shape.halfHeightQ = def.zone.halfHeightQ;
            zones.push_back({zoneFrom(def.zone, shape), DispelKinds(def.zone.kind) != 0, def.zoneDamage});
            if (def.zone2.kind != ZoneKind::None) zones.push_back({zoneFrom(def.zone2, shape), DispelKinds(def.zone2.kind) != 0, 0});
        } else if (cast.spell == kPermafrostClimb) {
            const FaceHit hit = CastFaceRay(simulation, cast, range);
            if (!hit.hit) return finish(SpellRefusal::Missed);
            out.hitQ = ToQ(hit.point, q);
            out.face = FaceOf(hit.normal);
            if (!InsideWalls(ctx.walls, out.hitQ >> 4)) return finish(SpellRefusal::Outside);
            if (!IsOpenId(world.GetVoxel(hit.seed))) return finish(SpellRefusal::Missed);
            PlumeParams pp;
            pp.seed = hit.seed;
            pp.axes = FaceAxesOf(hit.normal);
            pp.seedRadius = def.plumeSeedRadius;
            pp.riseMax = def.riseMax;
            pp.runPops = def.plumeRunPops;
            pp.lateral = def.plumeLateral;
            pp.visitCap = def.plumeVisitCap;
            const PlumeResult plume = PlumeOperator(world, ctx.walls, pp);
            out.stats.visits += plume.visits;
            out.stats.chunksRead += plume.chunksRead;
            out.stats.path = static_cast<uint32_t>(plume.rise.size() + plume.run.size());
            out.stats.front = static_cast<uint32_t>(plume.front.size());
            if (plume.front.empty()) return finish(SpellRefusal::Missed);
            // The ice: Rise cells near support, then Front cells that are skin or join the ice, in order.
            static constexpr std::array<glm::ivec3, 6> kFaces = {glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 1, 0),
                                                                 glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)};
            VoxelCache cache(world, ctx.walls);
            std::set<std::tuple<int32_t, int32_t, int32_t>> taken;
            std::vector<glm::ivec3> ice;
            const auto take = [&](const glm::ivec3& c) {
                if (taken.emplace(c.x, c.y, c.z).second) ice.push_back(c);
            };
            for (const glm::ivec3& c : plume.rise) {
                bool near = false;
                for (const glm::ivec3& f : kFaces)
                    for (int k = 1; k <= def.supportReach && !near; ++k) near = IsSupportId(cache.Get(c + f * k));
                if (near) take(c);
            }
            for (const glm::ivec3& c : plume.front) {
                bool joins = false;
                for (const glm::ivec3& f : kFaces) {
                    const glm::ivec3 n = c + f;
                    joins = joins || IsSupportId(cache.Get(n)) || taken.contains({n.x, n.y, n.z});
                }
                if (joins) take(c);
            }
            out.stats.chunksRead += cache.ChunksRead();
            std::vector<glm::ivec3> kept;
            if (!guardCells(ice, kept)) return finish(SpellRefusal::NearSpawn);
            if (!kept.empty()) {
                out.stats.planned += static_cast<uint32_t>(kept.size());
                nowOps.push_back({MapCellsOp(def.fillMap, kept), newOwner(0, def.expiryMap, def.lifetimeTicks, nullptr)});
            }
            // The Rime box over the face: the plume's cells' box, grown into the face by rimeDepth.
            std::vector<glm::ivec3> all(plume.rise.begin(), plume.rise.end());
            all.insert(all.end(), plume.run.begin(), plume.run.end());
            all.insert(all.end(), plume.front.begin(), plume.front.end());
            glm::ivec3 lo, hi;
            CellBox(all, lo, hi);
            voxel::ShapeQ rime;
            rime.kind = voxel::kShapeBox;
            rime.a = lo;
            rime.b = hi;
            for (int a = 0; a < 3; ++a) {
                if (hit.normal[a] > 0) rime.a[a] -= def.rimeDepth;
                if (hit.normal[a] < 0) rime.b[a] += def.rimeDepth;
            }
            nowOps.push_back({WorldOp::Map(voxel::Map_Rime, rime), newOwner(1, voxel::Map_Thaw, def.rimeLifetimeTicks, &rime)});
            turns = true;
            // Root, then Chill, over the plume's box grown sideways and down to the feet of those near it.
            voxel::ShapeQ box;
            box.kind = voxel::kShapeBox;
            box.a = lo - glm::ivec3(def.zone.radiusQ, def.zone.halfHeightQ, def.zone.radiusQ);
            box.b = hi + glm::ivec3(def.zone.radiusQ, 0, def.zone.radiusQ);
            zones.push_back({zoneFrom(def.zone, box), DispelKinds(def.zone.kind) != 0, def.zoneDamage});
            if (def.zone2.kind != ZoneKind::None) zones.push_back({zoneFrom(def.zone2, box), DispelKinds(def.zone2.kind) != 0, 0});
        } else {
            return finish(SpellRefusal::NotBuilt);
        }

        // 2. The flight: the effects land on arrival, a zone at max(the arrival, the lead)
        //    (magic.md "Casting: mana, gestures and projectiles").
        //    Punch and Dash fly nothing; a drawn shape's projectile flies to the line's first point.
        if (!def.noProjectile) {
            const Form form = FormOf(cast.spell);
            out.flightTicks = FlightTicks(table, eyeQ, out.hitQ, table.forms[static_cast<size_t>(form)].speedMs, form == Form::Flow);
        }
        const uint32_t arrival = tRun + out.flightTicks;
        tZ = std::max(arrival, ZoneStartTick(table, tRun));
        out.zoneTick = tZ;
        for (PlannedZone& pz : zones) pz.zone.startTick = tZ;

        // 3. The spawn guard: no op box and no zone box may meet an enemy spawn area.
        for (const PlannedOp& planned : nowOps) {
            glm::ivec3 lo, hi;
            planned.op.Bounds(lo, hi);
            if (MeetsEnemySpawn(ctx.spawns, cast.team, lo, hi)) return finish(SpellRefusal::NearSpawn);
        }
        if (bore) {
            glm::ivec3 lo, hi;
            boreSite.carve.Bounds(lo, hi);
            if (MeetsEnemySpawn(ctx.spawns, cast.team, lo, hi)) return finish(SpellRefusal::NearSpawn);
        }
        if (shatter) {
            glm::ivec3 lo, hi;
            voxel::SphereVoxelBounds(shatterSphere, lo, hi);
            if (MeetsEnemySpawn(ctx.spawns, cast.team, lo, hi)) return finish(SpellRefusal::NearSpawn);
        }
        for (const PlannedBand& band : bands) {
            glm::ivec3 lo, hi;
            band.op.Bounds(lo, hi);
            if (MeetsEnemySpawn(ctx.spawns, cast.team, lo, hi)) return finish(SpellRefusal::NearSpawn);
        }
        if (blow) {
            glm::ivec3 lo, hi;
            blowOp.Bounds(lo, hi);
            if (MeetsEnemySpawn(ctx.spawns, cast.team, lo, hi)) return finish(SpellRefusal::NearSpawn);
        }
        for (const PlannedZone& pz : zones) {
            glm::ivec3 lo, hi;
            ZoneVoxelBox(pz.zone, lo, hi);
            if (MeetsEnemySpawn(ctx.spawns, cast.team, lo, hi)) return finish(SpellRefusal::NearSpawn);
        }

        // 4. The caps.
        if (out.stats.planned > 0 && ctx.registry.LiveVoxels(0) + out.stats.planned > kMaxConjuredPerRegion)
            return finish(SpellRefusal::TooMuchConjured);
        if (turns && ctx.registry.LiveVoxels(1) >= kMaxTurnedPerRegion) return finish(SpellRefusal::TooMuchConjured);
        if (!zones.empty()) {
            ZoneSet& set = simulation.Zones();
            if (set.LiveOrPendingAt(tRun) + zones.size() > ctx.maxZones || set.Size() + zones.size() > kMaxZoneRecords)
                return finish(SpellRefusal::TooManyZones);
            if (cast.player != 0) {
                size_t own = 0;
                for (const auto& [id, owner] : ctx.zoneOwners) {
                    if (owner.caster != cast.player) continue;
                    const Zone* z = set.Find(id);
                    if (z != nullptr && z->EndTick() > tRun) ++own;
                }
                if (own + zones.size() > ctx.maxZonesPerPlayer) return finish(SpellRefusal::TooManyZones);
            }
        }

        // 5. Mana, after every other check: a refusal costs nothing. A script (player 0) pays none.
        if (cast.player != 0) {
            const uint32_t cost = SpellManaCost(table, cast.spell, drawn ? gesture : SpellGesture{});
            CasterState& caster = ctx.casters[cast.player];
            const uint16_t have = ManaAt(table, caster, tRun);
            if (have < cost) return finish(SpellRefusal::NoMana);
            caster.mana = static_cast<uint16_t>(have - cost);
            caster.manaTick = tRun;
            out.manaCost = static_cast<uint16_t>(cost);
        }

        // 6. Apply what lands now, schedule what lands later.
        uint64_t opWork = 0;
        uint32_t bytes = kCastFxBytes + 2u + 12u * static_cast<uint32_t>(out.drawnPointsQ.size());
        std::vector<uint32_t> ownerIds(owners.size(), 0);
        std::vector<bool> ownerMade(owners.size(), false);
        const auto ownerId = [&](int index) {
            const size_t i = static_cast<size_t>(index);
            if (!ownerMade[i]) {
                ownerIds[i] = ctx.registry.NewOwner(owners[i]);
                ownerMade[i] = true;
            }
            return ownerIds[i];
        };
        const auto scheduled = [&](ScheduledKind kind, uint32_t tick) {
            ScheduledAction a;
            a.tick = tick;
            a.kind = kind;
            a.spell = cast.spell;
            a.casterTeam = cast.team;
            a.caster = cast.player;
            return a;
        };
        if (cast.spell == kDash && cast.casterState != nullptr) out.dashStarted = StartDash(*cast.casterState, cast.dir);
        // The per-player cap: a script (player 0) is no player.
        if (out.stats.planned > 0 && cast.player != 0) {
            for (AppliedOp& applied : ctx.registry.MakeRoom(simulation, cast.player, out.stats.planned)) {
                ctx.structure.QueueFromEdits(applied.edits, materials);
                opWork += OpWork(applied.op);
                bytes += static_cast<uint32_t>(OpWireBytes(applied.op));
                out.groups.push_back(GroupOf(std::move(applied)));
            }
        }
        if (bore && arrival == tRun) {
            // The bore's island search and load check run now, as a blast's, on the cast's work.
            BlastResult carved = Blast(simulation, boreSite);
            const CastWork w = CastWorkOf(carved);
            out.work.search += w.search;
            out.work.neck += w.neck;
            opWork += w.ops;
            for (const AppliedOp& applied : carved.ops) bytes += static_cast<uint32_t>(OpWireBytes(applied.op));
            out.stats.released += static_cast<uint32_t>(
                std::count_if(carved.ops.begin(), carved.ops.end(), [](const AppliedOp& a) { return a.op.kind == WorldOpKind::Detach; }));
            out.groups.push_back(std::move(carved));
        } else if (bore) {
            // On arrival: its search is not known now and is paid from the structure credit then, as a blow's.
            ScheduledAction a = scheduled(ScheduledKind::Bore, arrival);
            a.site = boreSite;
            opWork += OpWork(boreSite.carve);
            bytes += static_cast<uint32_t>(OpWireBytes(boreSite.carve));
            Schedule(ctx, std::move(a));
        }
        for (PlannedOp& planned : nowOps) {
            opWork += OpWork(planned.op);
            bytes += static_cast<uint32_t>(OpWireBytes(planned.op));
            if (arrival == tRun) {
                AppliedOp applied;
                applied.op = std::move(planned.op);
                applied.result = ApplyOp(world, applied.op, &applied.edits);
                if (planned.owner >= 0) ctx.registry.Record(ownerId(planned.owner), applied.edits, materials);
                ctx.structure.QueueFromEdits(applied.edits, materials);
                out.groups.push_back(GroupOf(std::move(applied)));
            } else {
                // A fill (a mask of conjured cells) is guarded again on its tick, as a band: never entomb who walked in.
                const bool fill = planned.op.kind == WorldOpKind::MapMask && planned.owner >= 0;
                ScheduledAction a = scheduled(fill ? ScheduledKind::Band : ScheduledKind::Op, arrival);
                a.hasOwner = planned.owner >= 0;
                if (a.hasOwner) a.owner = ownerId(planned.owner);
                a.op = std::move(planned.op);
                Schedule(ctx, std::move(a));
            }
        }
        for (PlannedBand& band : bands) {
            ScheduledAction a = scheduled(ScheduledKind::Band, tRun + std::max(out.flightTicks + band.along, band.minTicks) + band.extra);
            a.owner = ownerId(band.owner);
            a.op = std::move(band.op);
            opWork += OpWork(a.op);
            bytes += static_cast<uint32_t>(OpWireBytes(a.op));
            Schedule(ctx, std::move(a));
        }
        if (blow) {
            ScheduledAction a = scheduled(ScheduledKind::Blow, tZ);
            a.op = blowOp;
            a.hasHitVoxel = blowHasVoxel;
            a.hitVoxel = blowVoxel;
            opWork += OpWork(a.op);
            bytes += static_cast<uint32_t>(OpWireBytes(a.op));
            Schedule(ctx, std::move(a));
        }
        uint64_t shatterWork = 0;
        if (shatter) {
            // Charged at its cap now: the flood itself runs at T_z on the voxels of that tick.
            ScheduledAction a = scheduled(ScheduledKind::Shatter, tZ);
            a.op.sphere = shatterSphere;
            glm::ivec3 lo, hi;
            voxel::SphereVoxelBounds(shatterSphere, lo, hi);
            const glm::ivec3 chunks = glm::max(voxel::VoxelToChunk(hi - glm::ivec3(1)) - voxel::VoxelToChunk(lo) + glm::ivec3(1), glm::ivec3(0));
            shatterWork = uint64_t{def.shatterVisitCap} * kPropagateVisitWork +
                          static_cast<uint64_t>(chunks.x) * static_cast<uint64_t>(chunks.y) * static_cast<uint64_t>(chunks.z) * kColdChunkGraphWork;
            opWork += kOpWork;
            Schedule(ctx, std::move(a));
        }
        for (const auto& [id, endTick] : cancels) {
            simulation.Zones().Cancel(id, endTick);
            out.zonesCancelled.emplace_back(id, endTick);
            bytes += kZoneCancelBytes;
        }
        for (PlannedZone& pz : zones) {
            pz.zone.zoneId = TakeZoneId(ctx, simulation.Zones());
            if (pz.zone.zoneId == 0 || !simulation.Zones().Insert(pz.zone)) continue;
            ctx.zoneOwners[pz.zone.zoneId] = ZoneOwner{cast.team, cast.player, pz.dispellable};
            out.zonesStarted.push_back(pz.zone);
            bytes += ZoneStartBytes(pz.zone);
            ScheduledAction a = scheduled(ScheduledKind::ZoneTick, pz.zone.startTick);
            a.zoneId = pz.zone.zoneId;
            a.damage = pz.damage;
            Schedule(ctx, std::move(a));
        }
        if (arrival == tRun) {
            out.damage.insert(out.damage.end(), direct.begin(), direct.end());
        } else {
            for (const SpellDamage& d : direct) {
                ScheduledAction a = scheduled(ScheduledKind::Hit, arrival);
                a.target = d.player;
                a.damage = d.amount;
                Schedule(ctx, std::move(a));
            }
        }

        const size_t later = (bore && arrival != tRun ? 1u : 0u) + (arrival != tRun ? nowOps.size() : 0u);
        out.stats.ops = static_cast<uint32_t>(out.groups.size() + later + bands.size() + (blow ? 1u : 0u) + (shatter ? 1u : 0u));
        out.stats.bytes = bytes;
        out.work.search += uint64_t{out.stats.visits} * kPropagateVisitWork + uint64_t{out.stats.chunksRead} * kColdChunkGraphWork +
                           boreSliceWork + shatterWork;
        out.work.ops = opWork;
        return finish(SpellRefusal::None);
    }

    ScheduledResult RunScheduled(Simulation& simulation, const SpellTable& table, CastContext& ctx, uint32_t tick,
                                 std::span<const SpellTarget> targets) {
        ScheduledResult out;
        voxel::World& world = simulation.World();
        const voxel::MaterialLibrary& materials = simulation.Materials();
        const double q = QPerMetre(world);

        // 1. Due follow-ups, in (tick, seq) order.
        size_t due = 0;
        while (due < ctx.schedule.size() && ctx.schedule[due].tick <= tick) ++due;
        std::vector<ScheduledAction> actions(std::make_move_iterator(ctx.schedule.begin()),
                                             std::make_move_iterator(ctx.schedule.begin() + static_cast<std::ptrdiff_t>(due)));
        ctx.schedule.erase(ctx.schedule.begin(), ctx.schedule.begin() + static_cast<std::ptrdiff_t>(due));
        const size_t firstGroup = out.groups.size();
        for (ScheduledAction& action : actions) {
            const SpellDef& def = table.spells[action.spell < kSpellCount ? action.spell : 0];
            switch (action.kind) {
            case ScheduledKind::Band: {
                glm::ivec3 lo, hi;
                action.op.Bounds(lo, hi);
                if (!glm::all(glm::lessThan(lo, hi))) break;
                // Players and bodies of this tick keep their cells: a band never entombs.
                const FillGuard guard(simulation, ctx.tuning, targets, lo, hi - glm::ivec3(1));
                voxel::VoxelMask mask = action.op.mask;
                const glm::ivec3 size = mask.Size();
                for (int z = 0; z < size.z; ++z)
                    for (int y = 0; y < size.y; ++y)
                        for (int x = 0; x < size.x; ++x)
                            if (mask.Get({x, y, z}) && guard.Takes(action.op.min + glm::ivec3(x, y, z))) mask.Set({x, y, z}, false);
                if (mask.Count() == 0) break;
                AppliedOp applied;
                applied.op = WorldOp::MapMasked(action.op.map, action.op.min, std::move(mask));
                applied.result = ApplyOp(world, applied.op, &applied.edits);
                ctx.registry.Record(action.owner, applied.edits, materials);
                out.groups.push_back(GroupOf(std::move(applied)));
                break;
            }
            case ScheduledKind::Blow: {
                const uint8_t hitMaterial = action.hasHitVoxel ? world.GetVoxel(action.hitVoxel) : uint8_t{0};
                BlastResult blast = BlastAt(simulation, action.op.sphere, action.op.power, hitMaterial);
                // The blow's search could not be known at T_run: it is paid from the structure credit.
                const CastWork work = CastWorkOf(blast);
                ctx.structure.credit -= static_cast<int64_t>(work.search + work.neck);
                out.groups.push_back(std::move(blast));
                break;
            }
            case ScheduledKind::Op: {
                AppliedOp applied;
                applied.op = std::move(action.op);
                applied.result = ApplyOp(world, applied.op, &applied.edits);
                if (action.hasOwner) ctx.registry.Record(action.owner, applied.edits, materials);
                out.groups.push_back(GroupOf(std::move(applied)));
                break;
            }
            case ScheduledKind::Bore: {
                BlastResult carved = Blast(simulation, action.site);
                const CastWork work = CastWorkOf(carved);
                ctx.structure.credit -= static_cast<int64_t>(work.search + work.neck);
                out.groups.push_back(std::move(carved));
                break;
            }
            case ScheduledKind::Hit:
                out.damage.push_back(SpellDamage{action.target, action.damage, action.caster, action.spell});
                break;
            case ScheduledKind::Shatter: {
                const ShatterResult flood = ShatterFlood(world, ctx.walls, action.op.sphere, def.shatterDepth, def.shatterVisitCap);
                if (flood.cells.empty()) break;
                AppliedOp applied;
                applied.op = MapCellsOp(voxel::Map_Shatter, flood.cells);
                applied.result = ApplyOp(world, applied.op, &applied.edits);
                out.groups.push_back(GroupOf(std::move(applied)));
                break;
            }
            case ScheduledKind::ZoneTick: {
                const Zone* found = simulation.Zones().Find(action.zoneId);
                if (found == nullptr || found->startTick > tick || tick >= found->EndTick()) break;
                const Zone zone = *found;
                if (action.damage > 0) {
                    for (const SpellTarget& t : targets) {
                        if ((zone.teamMask & TeamBit(t.team)) == 0) continue;
                        if (!ZoneHoldsFeet(zone, ToQ(glm::dvec3(t.feet), q))) continue;
                        out.damage.push_back(SpellDamage{t.player, action.damage, action.caster, action.spell});
                    }
                }
                if (def.maxBodies == 0) break;
                physics::PhysicsWorld& physics = simulation.Physics();
                scene::Scene& scene = simulation.Scene();
                struct Candidate {
                    int64_t len;
                    uint32_t id;
                    physics::BodyId body;
                    glm::i64vec3 kick;
                };
                std::vector<Candidate> bodies;
                glm::ivec3 boxMin, boxMax;
                ZoneBoundsQ(zone, boxMin, boxMax);
                for (const auto [entity, rigid] : scene.Registry().view<const scene::RigidBody>().each()) {
                    if (rigid.motion != physics::MotionType::Dynamic || !physics.IsValid(rigid.body)) continue;
                    if (scene.Registry().any_of<scene::Held, scene::Mechanism>(entity)) continue;
                    const glm::ivec3 p = ToQ(glm::dvec3(physics.GetState(rigid.body).position), q);
                    if (glm::any(glm::lessThan(p, boxMin)) || glm::any(glm::greaterThan(p, boxMax))) continue;
                    glm::i64vec3 kick;
                    int64_t len = 0;
                    if (!ZoneKick(zone, glm::i64vec3(p), def.zone.rampQ, kick, len)) continue;
                    bodies.push_back({len, static_cast<uint32_t>(entity), rigid.body, kick});
                }
                std::sort(bodies.begin(), bodies.end(), [](const Candidate& a, const Candidate& b) { return std::tie(a.len, a.id) < std::tie(b.len, b.id); });
                if (bodies.size() > def.maxBodies) bodies.resize(def.maxBodies);
                const float cap = static_cast<float>(def.maxBodyKickCmS) / 100.0f;
                for (const Candidate& c : bodies) {
                    glm::vec3 kick = glm::vec3(c.kick) / 100.0f;
                    const float len = glm::length(kick);
                    if (len > cap && len > 0.0f) kick *= cap / len;
                    physics.AddImpulse(c.body, kick * physics.GetMass(c.body));
                    physics.Activate(c.body);
                    ++out.bodiesPushed;
                }
                break;
            }
            }
        }
        // Every op of step 1 that turned a collider into a non-collider gets a site check.
        for (size_t i = firstGroup; i < out.groups.size(); ++i)
            for (const AppliedOp& applied : out.groups[i].ops) ctx.structure.QueueFromEdits(applied.edits, materials);

        // 2. Expiries of conjured matter.
        for (AppliedOp& applied : ctx.registry.RunExpiries(simulation, tick)) {
            ctx.structure.QueueFromEdits(applied.edits, materials);
            out.groups.push_back(GroupOf(std::move(applied)));
        }

        // 3. Site checks on the structure credit.
        out.sitesRun = ctx.structure.Run(simulation, out.groups);

        // 4. Ended zones go; so do their owners.
        simulation.Zones().PruneEndedBefore(tick);
        std::erase_if(ctx.zoneOwners, [&simulation](const auto& entry) { return simulation.Zones().Find(entry.first) == nullptr; });
        return out;
    }

    CastWork CastWorkOf(const CastSpellResult& spell) { return spell.work; }

    std::vector<SpawnArea> SpawnAreasOf(std::span<const std::pair<TeamId, glm::dvec3>> spawns, const voxel::World& world) {
        std::vector<SpawnArea> out;
        const double vs = static_cast<double>(world.VoxelSize());
        const int32_t half = static_cast<int32_t>(std::lround(6.0 / vs));
        const int32_t rowMin = world.WindowMinChunk().y * voxel::kChunkSize;
        const int32_t rowMax = (world.WindowMinChunk().y + world.WindowSize().y) * voxel::kChunkSize;
        for (const auto& [team, feet] : spawns) {
            const int32_t x = static_cast<int32_t>(std::floor(feet.x / vs)), z = static_cast<int32_t>(std::floor(feet.z / vs));
            SpawnArea area;
            area.team = team;
            area.min = {x - half, rowMin, z - half};
            area.max = {x + half + 1, rowMax, z + half + 1};
            out.push_back(area);
        }
        return out;
    }
}
