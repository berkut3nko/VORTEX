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

        /// One planned zone, and whether a Punch may end it.
        struct PlannedZone {
            Zone zone;
            bool dispellable = false;
        };
    }

    const char* ToString(SpellRefusal refusal) {
        switch (refusal) {
        case SpellRefusal::None: return "none";
        case SpellRefusal::NotBuilt: return "not built";
        case SpellRefusal::FormCooling: return "form cooling";
        case SpellRefusal::NotAttuned: return "not attuned";
        case SpellRefusal::Missed: return "missed";
        case SpellRefusal::Outside: return "outside";
        case SpellRefusal::TooMuchConjured: return "too much conjured";
        case SpellRefusal::TooManyZones: return "too many zones";
        case SpellRefusal::NearSpawn: return "near spawn";
        case SpellRefusal::Rooted: return "rooted";
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
        const glm::i64vec3 p(feetQ), a(sh.a), b(sh.b);
        const int64_t R = sh.radiusQ;
        switch (sh.kind) {
        case voxel::kShapeSphere: return Length(p - a) < R;
        case voxel::kShapeCapsule: return Length(FromSegment(p, a, b)) < R;
        case voxel::kShapeCylinder:
            return std::abs(p.y - a.y) <= int64_t{sh.halfHeightQ} && Length(glm::i64vec3(p.x - a.x, 0, p.z - a.z)) <= R;
        case voxel::kShapeBox: return glm::all(glm::greaterThanEqual(feetQ >> 4, sh.a)) && glm::all(glm::lessThan(feetQ >> 4, sh.b));
        default: return false;
        }
    }

    SpellRefusal CheckCapture(const CastContext& ctx, const SpellTable& table, const SpellCast& cast, uint32_t tick) {
        if (cast.spell >= 16 || !table.spells[cast.spell].built) return SpellRefusal::NotBuilt;
        CasterState caster;
        if (const auto it = ctx.casters.find(cast.player); it != ctx.casters.end()) caster = it->second;
        const uint8_t element = static_cast<uint8_t>(ElementOf(cast.spell));
        if (element != caster.elementA && element != caster.elementB) return SpellRefusal::NotAttuned;
        if (tick < caster.formReadyTick[static_cast<size_t>(FormOf(cast.spell))]) return SpellRefusal::FormCooling;
        return SpellRefusal::None;
    }

    CastSpellResult CastSpell(Simulation& simulation, const SpellTable& table, const SpellCast& cast, CastContext& ctx,
                              std::span<const SpellTarget> targets) {
        const auto started = std::chrono::steady_clock::now();
        CastSpellResult out;
        const uint32_t tRun = simulation.Tick();
        const uint32_t tZ = ZoneStartTick(table, tRun);
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

        // 1. Plan: the hit, the operator, the zones and the ops; nothing is applied yet.
        std::vector<PlannedZone> zones;
        std::vector<WorldOp> bandOps;
        std::vector<uint8_t> bandIndex;
        bool blow = false;
        WorldOp blowOp;
        bool blowHasVoxel = false;
        glm::ivec3 blowVoxel{0};
        glm::dvec2 dashDir(0.0);
        std::vector<std::pair<uint16_t, uint32_t>> cancels;

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

        if (cast.spell == kUpdraft || cast.spell == kBarricadeWave) {
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
                const FillBands fill = BarricadeFill(world, ctx.walls, strip, p.axes, def.rimHeight, def.bandCount, def.burstHalfWidth);
                if (fill.cells > 0) {
                    glm::ivec3 lo(std::numeric_limits<int32_t>::max()), hi(std::numeric_limits<int32_t>::min());
                    for (const auto& band : fill.bands)
                        for (const glm::ivec3& c : band) {
                            lo = glm::min(lo, c);
                            hi = glm::max(hi, c);
                        }
                    const FillGuard guard(simulation, ctx.tuning, targets, lo, hi);
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
                        bandOps.push_back(MapCellsOp(def.fillMap, kept));
                        bandIndex.push_back(static_cast<uint8_t>(b));
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
            shape.a = ToQ(cast.eye + d * (def.zone.nearQ / 16.0 * vs) - eyeToFeet, q);
            shape.b = ToQ(cast.eye + d * (def.zone.farQ / 16.0 * vs) - eyeToFeet, q);
            shape.radiusQ = def.zone.radiusQ;
            zones.push_back({zoneOf(ZoneKind::Shove, shape), false});
            // The dispel: enemy zones that last, live at T_z, centred near the fist.
            const int64_t reachQ = def.dispelRadiusQ;
            for (const Zone& z : simulation.Zones().All()) {
                const auto owner = ctx.zoneOwners.find(z.zoneId);
                if (owner == ctx.zoneOwners.end() || !owner->second.dispellable || owner->second.casterTeam == cast.team) continue;
                if (z.startTick > tZ || z.EndTick() <= tZ) continue;
                if (ZoneCentreDistanceSq(z, glm::i64vec3(out.hitQ)) > reachQ * reachQ) continue;
                cancels.emplace_back(z.zoneId, tZ);
            }
        } else {
            return finish(SpellRefusal::NotBuilt);
        }

        // 2. The spawn guard: no op box and no zone box may meet an enemy spawn area.
        for (const WorldOp& op : bandOps) {
            glm::ivec3 lo, hi;
            op.Bounds(lo, hi);
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

        // 3. The caps.
        if (out.stats.planned > 0 && ctx.registry.LiveVoxels(0) + out.stats.planned > kMaxConjuredPerRegion)
            return finish(SpellRefusal::TooMuchConjured);
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

        // 4. Apply.
        uint64_t opWork = 0;
        uint32_t bytes = kCastFxBytes;
        if (cast.spell == kDash && cast.casterState != nullptr) out.dashStarted = StartDash(*cast.casterState, cast.dir);
        if (!bandOps.empty()) {
            for (AppliedOp& applied : ctx.registry.MakeRoom(simulation, cast.player, out.stats.planned)) {
                ctx.structure.QueueFromEdits(applied.edits, materials);
                opWork += OpWork(applied.op);
                bytes += static_cast<uint32_t>(OpWireBytes(applied.op));
                out.groups.push_back(GroupOf(std::move(applied)));
            }
            KnitOwnerInfo info;
            info.layer = 0;
            info.expiryMap = def.expiryMap;
            info.caster = cast.player;
            info.dueTick = tRun + def.lifetimeTicks;
            const uint32_t owner = ctx.registry.NewOwner(info);
            for (size_t i = 0; i < bandOps.size(); ++i) {
                ScheduledAction a;
                a.tick = tRun + def.bandFirstTick + uint32_t{bandIndex[i]} * def.bandSpacingTicks;
                a.kind = ScheduledKind::Band;
                a.owner = owner;
                a.op = bandOps[i];
                a.spell = cast.spell;
                a.casterTeam = cast.team;
                a.caster = cast.player;
                opWork += OpWork(a.op);
                bytes += static_cast<uint32_t>(OpWireBytes(a.op));
                Schedule(ctx, std::move(a));
            }
        }
        if (blow) {
            ScheduledAction a;
            a.tick = tZ;
            a.kind = ScheduledKind::Blow;
            a.op = blowOp;
            a.spell = cast.spell;
            a.casterTeam = cast.team;
            a.caster = cast.player;
            a.hasHitVoxel = blowHasVoxel;
            a.hitVoxel = blowVoxel;
            opWork += OpWork(a.op);
            bytes += static_cast<uint32_t>(OpWireBytes(a.op));
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
            ScheduledAction a;
            a.tick = pz.zone.startTick;
            a.kind = ScheduledKind::ZoneTick;
            a.zoneId = pz.zone.zoneId;
            a.spell = cast.spell;
            a.casterTeam = cast.team;
            a.caster = cast.player;
            Schedule(ctx, std::move(a));
        }

        out.stats.ops = static_cast<uint32_t>(out.groups.size() + bandOps.size() + (blow ? 1u : 0u));
        out.stats.bytes = bytes;
        out.work.search = uint64_t{out.stats.visits} * kPropagateVisitWork + uint64_t{out.stats.chunksRead} * kColdChunkGraphWork;
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
            case ScheduledKind::ZoneTick: {
                const Zone* found = simulation.Zones().Find(action.zoneId);
                if (found == nullptr || found->startTick > tick || tick >= found->EndTick()) break;
                const Zone zone = *found;
                if (def.zoneDamage > 0) {
                    for (const SpellTarget& t : targets) {
                        if ((zone.teamMask & TeamBit(t.team)) == 0) continue;
                        if (!ZoneHoldsFeet(zone, ToQ(glm::dvec3(t.feet), q))) continue;
                        out.damage.push_back(SpellDamage{t.player, def.zoneDamage, action.caster, action.spell});
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
