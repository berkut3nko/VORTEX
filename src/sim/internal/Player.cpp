module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.physics;
import vortex.voxel;

namespace vortex::sim {

    float DistanceToPlayAreaEdge(const PlayArea& area, const glm::vec2& feet) {
        if (!area.Bounded()) return std::numeric_limits<float>::infinity();
        return std::min(std::min(feet.x - area.min.x, area.max.x - feet.x), std::min(feet.y - area.min.y, area.max.y - feet.y));
    }

    namespace {
        constexpr double kTwoPi = 2.0 * std::numbers::pi;
        constexpr double kHalfPi = 0.5 * std::numbers::pi;

        bool Finite(float v) { return std::isfinite(v); }

        /// The wish speed along one axis, its outward part cut to what reaches [lo, hi] in one tick.
        float LimitToArea(float p, float wish, float lo, float hi, float dt) {
            if (wish > 0.0f) return std::min(wish, std::max(0.0f, (hi - p) / dt));
            if (wish < 0.0f) return std::max(wish, std::min(0.0f, (lo - p) / dt));
            return wish;
        }

        int16_t ClampPush(int64_t v) { return static_cast<int16_t>(std::clamp<int64_t>(v, -kPushLimitCmS, kPushLimitCmS)); }

        void AddPush(PlayerState& state, int axis, int64_t v) { state.push[axis] = ClampPush(int64_t{state.push[axis]} + v); }

        int16_t Decay(int16_t v, uint16_t by) {
            if (v > 0) return static_cast<int16_t>(std::max<int32_t>(0, v - by));
            if (v < 0) return static_cast<int16_t>(std::min<int32_t>(0, v + by));
            return 0;
        }

        int64_t Dot(const glm::i64vec3& a, const glm::i64vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

        int64_t Length(const glm::i64vec3& d) { return static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(Dot(d, d)))); }

        /// The feet minus the nearest point of the segment a-b (q; the point truncated toward zero).
        glm::i64vec3 FromSegment(const glm::i64vec3& feet, const glm::i64vec3& a, const glm::i64vec3& b) {
            const glm::i64vec3 u = b - a;
            const int64_t uu = Dot(u, u);
            if (uu == 0) return feet - a;
            const int64_t t = std::clamp<int64_t>(Dot(feet - a, u), 0, uu);
            return feet - (a + u * t / uu);
        }

        glm::vec2 DashDirection(const PlayerState& state) {
            const glm::vec2 d(static_cast<float>(state.dashDir[0]), static_cast<float>(state.dashDir[1]));
            const float len = std::sqrt(d.x * d.x + d.y * d.y);
            return len > 0.0f ? d / len : glm::vec2(0.0f);
        }

        /// The status counters of one step (magic.md step 8) and the status bits that follow from them.
        void CountDownStatuses(PlayerState& state) {
            const bool held = state.rootLeft > 0 || state.pinLeft > 0;
            if (state.slowLeft > 0) --state.slowLeft;
            if (state.slowLeft == 0) state.slowQ = 0;
            if (state.rootLeft > 0) --state.rootLeft;
            if (state.pinLeft > 0) --state.pinLeft;
            if (state.ccGuardLeft > 0) --state.ccGuardLeft;
            if (held && state.rootLeft == 0 && state.pinLeft == 0) state.ccGuardLeft = kControlGuardTicks;
            uint8_t s = static_cast<uint8_t>(state.status & (PlayerStatus_Lifted | PlayerStatus_Fresh | PlayerStatus_Leaping));
            if (state.ccGuardLeft == 0) s = static_cast<uint8_t>(s & ~PlayerStatus_Fresh);
            if (state.rootLeft > 0) s |= PlayerStatus_Rooted;
            if (state.pinLeft > 0) s |= PlayerStatus_Pinned;
            if (state.slowLeft > 0) s |= PlayerStatus_Chilled;
            if (state.ccGuardLeft > 0) s |= PlayerStatus_Free;
            state.status = s;
        }

        /// Where a move from `from` to `to` along one axis may end: `to`, or the bound it passed
        /// (from inside the area), or `from` (a move that started outside and went further out).
        float AreaBound(float from, float to, float lo, float hi) {
            if (to > hi && to > from) return std::max(hi, from);
            if (to < lo && to < from) return std::min(lo, from);
            return to;
        }
    }

    bool IsValid(const PlayerTuning& t, std::string* why) {
        auto fail = [why](const char* what) {
            if (why) *why = what;
            return false;
        };
        for (const float v : {t.walkSpeed, t.runSpeed, t.jumpSpeed, t.height, t.radius, t.eyeHeight, t.maxSlopeDegrees, t.stepHeight,
                              t.castRadius, t.castPower, t.castRange})
            if (!Finite(v)) return fail("a player tuning value is not finite");
        if (t.walkSpeed < 0.0f || t.walkSpeed > 50.0f || t.runSpeed < 0.0f || t.runSpeed > 50.0f) return fail("speeds must be 0..50 m/s");
        if (t.jumpSpeed < 0.0f || t.jumpSpeed > 30.0f) return fail("jump speed must be 0..30 m/s");
        if (t.radius < 0.05f || t.radius > 2.0f || t.height < 2.0f * t.radius + 0.02f || t.height > 8.0f)
            return fail("the capsule must be 0.05..2 m wide and up to 8 m tall");
        if (t.eyeHeight < 0.0f || t.eyeHeight > t.height) return fail("the eye must be inside the capsule");
        if (t.maxSlopeDegrees < 0.0f || t.maxSlopeDegrees > 89.0f || t.stepHeight < 0.0f || t.stepHeight > 2.0f)
            return fail("slope 0..89 degrees, step 0..2 m");
        if (t.castRadius < 0.0f || t.castRadius > 9.0f || t.castPower < 0.0f || t.castPower > 4000.0f || t.castRange < 0.0f ||
            t.castRange > 1000.0f)
            return fail("cast radius 0..9 m, power 0..4000, range 0..1000 m");
        if (t.pushDecayGround > kPushLimitCmS || t.pushDecayAir > kPushLimitCmS) return fail("push decays 0..1500 cm/s per tick");
        return true;
    }

    uint16_t QuantizeYaw(float radians) {
        if (!Finite(radians)) return 0;
        double turns = static_cast<double>(radians) / kTwoPi;
        turns -= std::floor(turns);
        return static_cast<uint16_t>(static_cast<uint32_t>(std::llround(turns * 65536.0)) & 0xFFFFu);
    }

    float YawRadians(uint16_t yaw) { return static_cast<float>(static_cast<double>(yaw) * (kTwoPi / 65536.0)); }

    int16_t QuantizePitch(float radians) {
        if (!Finite(radians)) return 0;
        const double v = std::clamp(static_cast<double>(radians) / kHalfPi, -1.0, 1.0);
        return static_cast<int16_t>(std::llround(v * 32767.0));
    }

    float PitchRadians(int16_t pitch) {
        return static_cast<float>(static_cast<double>(std::max<int16_t>(pitch, -32767)) * (kHalfPi / 32767.0));
    }

    int8_t QuantizeAxis(float value) {
        if (!Finite(value)) return 0;
        return static_cast<int8_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 127.0f));
    }

    glm::vec3 MoveDirection(const PlayerInput& input) {
        const float mx = static_cast<float>(std::max<int8_t>(input.moveX, -127)) / 127.0f;
        const float mz = static_cast<float>(std::max<int8_t>(input.moveZ, -127)) / 127.0f;
        if (mx == 0.0f && mz == 0.0f) return glm::vec3(0.0f);
        const float yaw = YawRadians(input.yaw);
        const float s = std::sin(yaw), c = std::cos(yaw);
        const glm::vec3 forward(-s, 0.0f, -c), right(c, 0.0f, -s);
        glm::vec3 wish = forward * mz + right * mx;
        const float len2 = glm::dot(wish, wish);
        if (len2 > 1.0f) wish /= std::sqrt(len2);
        return wish;
    }

    glm::vec3 LookDirection(uint16_t yaw, int16_t pitch) {
        const float y = YawRadians(yaw), p = PitchRadians(pitch);
        return {-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
    }

    math::AABB PlayerBounds(const glm::vec3& feet, const PlayerTuning& t) {
        return math::AABB(feet - glm::vec3(t.radius, 0.0f, t.radius), feet + glm::vec3(t.radius, t.height, t.radius));
    }

    math::AABB PlayerReach(const glm::vec3& feet, const PlayerTuning& t) {
        math::AABB box = PlayerBounds(feet, t);
        box.Expand(1.0f);
        return box;
    }

    physics::CharacterDesc PlayerCharacterDesc(const glm::vec3& feet, const PlayerTuning& t) {
        physics::CharacterDesc desc;
        desc.position = feet;
        desc.height = t.height;
        desc.radius = t.radius;
        desc.maxSlopeDegrees = t.maxSlopeDegrees;
        desc.stepHeight = t.stepHeight;
        return desc;
    }

    uint8_t PressButtons(PlayerState& state, uint8_t buttons) {
        buttons &= kPlayerButtonMask;
        const uint8_t pressed = static_cast<uint8_t>(buttons & ~state.lastButtons);
        state.lastButtons = buttons;
        return pressed;
    }

    bool TakeCast(PlayerState& state, uint8_t pressed, const PlayerTuning& tuning) {
        if ((pressed & PlayerButton_Cast) == 0 || state.cooldownTicks > 0) return false;
        state.cooldownTicks = CastCooldownFor(state, tuning);
        return true;
    }

    uint8_t CastCooldownFor(const PlayerState& state, const PlayerTuning& tuning) {
        const uint32_t ticks = uint32_t{tuning.castCooldownTicks} * (16u + state.slowQ / 2u) / 16u;
        return static_cast<uint8_t>(std::min<uint32_t>(ticks, 255u));
    }

    bool StartDash(PlayerState& state, const glm::vec3& aim) {
        if (state.rootLeft > 0 || state.pinLeft > 0 || (state.status & (PlayerStatus_Rooted | PlayerStatus_Pinned)) != 0) return false;
        if (!Finite(aim.x) || !Finite(aim.z)) return false;
        const double x = aim.x, z = aim.z, len = std::sqrt(x * x + z * z);
        if (!(len > 1.0e-6)) return false;
        state.dashLeft = kDashTicks;
        state.dashDir[0] = static_cast<int8_t>(std::lround(x / len * 127.0));
        state.dashDir[1] = static_cast<int8_t>(std::lround(z / len * 127.0));
        return true;
    }

    bool LeapPushOf(const glm::vec3& aim, int16_t push[3]) {
        if (!Finite(aim.x) || !Finite(aim.y) || !Finite(aim.z)) return false;
        const double x = aim.x, y = aim.y, z = aim.z, h = std::sqrt(x * x + z * z);
        if (!(h > 1.0e-6)) return false;
        constexpr double kDegree = std::numbers::pi / 180.0;
        const double pitch = std::clamp(std::atan2(y, h), static_cast<double>(kLeapMinPitchDegrees) * kDegree,
                                        static_cast<double>(kLeapMaxPitchDegrees) * kDegree);
        const double speed = kLeapSpeedCmS, across = std::cos(pitch) * speed;
        push[0] = static_cast<int16_t>(std::lround(x / h * across));
        push[1] = static_cast<int16_t>(std::lround(std::sin(pitch) * speed));
        push[2] = static_cast<int16_t>(std::lround(z / h * across));
        return true;
    }

    bool StartLeap(PlayerState& state, const glm::vec3& aim) {
        if (state.rootLeft > 0 || state.pinLeft > 0 || (state.status & (PlayerStatus_Rooted | PlayerStatus_Pinned)) != 0) return false;
        int16_t push[3];
        if (!LeapPushOf(aim, push)) return false;
        for (int i = 0; i < 3; ++i) state.push[i] = push[i];
        state.dashLeft = 0;
        state.status = static_cast<uint8_t>(state.status | PlayerStatus_Leaping | PlayerStatus_Lifted);
        return true;
    }

    glm::vec3 LeapAimToward(const glm::vec3& feet, const glm::dvec3& target, float gravity) {
        const glm::dvec3 d = target - glm::dvec3(feet);
        const double dx = std::sqrt(d.x * d.x + d.z * d.z), dy = d.y;
        const double g = Finite(gravity) && gravity > 0.0f ? static_cast<double>(gravity) : 9.81;
        const double v = static_cast<double>(kLeapSpeedCmS) / 100.0;
        constexpr double kDegree = std::numbers::pi / 180.0;
        const double lo = static_cast<double>(kLeapMinPitchDegrees) * kDegree, hi = static_cast<double>(kLeapMaxPitchDegrees) * kDegree;
        const glm::dvec2 across = dx > 1.0e-6 ? glm::dvec2(d.x, d.z) / dx : glm::dvec2(0.0, -1.0);
        double pitch = 45.0 * kDegree;
        const double disc = v * v * v * v - g * (g * dx * dx + 2.0 * dy * v * v);
        if (dx > 1.0e-6 && disc >= 0.0) {
            const double low = std::atan((v * v - std::sqrt(disc)) / (g * dx)), high = std::atan((v * v + std::sqrt(disc)) / (g * dx));
            pitch = low >= lo ? low : std::clamp(high, lo, hi);
        } else if (dx <= 1.0e-6) {
            pitch = hi;
        }
        pitch = std::clamp(pitch, lo, hi);
        return glm::vec3(static_cast<float>(across.x * std::cos(pitch)), static_cast<float>(std::sin(pitch)),
                         static_cast<float>(across.y * std::cos(pitch)));
    }

    bool StartSpellMove(PlayerState& state, SpellId spell, const glm::vec3& aim) {
        if (spell == kDash) return StartDash(state, aim);
        if (spell == kMeteorLeap) return StartLeap(state, aim);
        return false;
    }

    void StartFreshLife(PlayerState& state) {
        state.status = static_cast<uint8_t>(state.status | PlayerStatus_Fresh | PlayerStatus_Free);
        state.ccGuardLeft = kFreshGuardTicks;
    }

    glm::ivec3 GroundVoxel(const glm::vec3& feet, float voxelSize) {
        const double q = 16.0 / static_cast<double>(voxelSize > 0.0f ? voxelSize : 0.125f);
        const auto Q = [q](float v) { return static_cast<int64_t>(std::llround(std::clamp(static_cast<double>(v) * q, -1.0e15, 1.0e15))); };
        const auto voxelOf = [](int64_t vq) { return static_cast<int32_t>(vq >= 0 ? vq / 16 : -((-vq + 15) / 16)); };
        return {voxelOf(Q(feet.x)), voxelOf(Q(feet.y) - 8), voxelOf(Q(feet.z))};
    }

    bool OnSlickGround(const voxel::World* world, const glm::vec3& feet) {
        if (world == nullptr || world->Materials() == nullptr) return false;
        const uint8_t id = world->GetVoxel(GroundVoxel(feet, world->VoxelSize()));
        return id != 0 && (world->Materials()->Get(voxel::kWorldPalette, id).flags & voxel::MaterialFlag_Slick) != 0;
    }

    std::optional<float> ApplyZones(PlayerState& state, const PlayerZones& zones, const PlayerTuning& tuning, float gravity) {
        std::optional<float> vertical;
        if (zones.zones == nullptr || zones.zones->Size() == 0) return vertical;
        const double q = 16.0 / static_cast<double>(zones.world ? zones.world->VoxelSize() : 0.125f);
        const glm::i64vec3 feet(std::llround(static_cast<double>(state.position.x) * q), std::llround(static_cast<double>(state.position.y) * q),
                                std::llround(static_cast<double>(state.position.z) * q));
        const math::AABB reach = PlayerReach(state.position, tuning);
        const auto lo = [q](float v) { return static_cast<int>(std::clamp(std::floor(static_cast<double>(v) * q), -2147483648.0, 2147483647.0)); };
        const auto hi = [q](float v) { return static_cast<int>(std::clamp(std::ceil(static_cast<double>(v) * q), -2147483648.0, 2147483647.0)); };
        const glm::ivec3 reachMin(lo(reach.min.x), lo(reach.min.y), lo(reach.min.z));
        const glm::ivec3 reachMax(hi(reach.max.x), hi(reach.max.y), hi(reach.max.z));
        const int64_t rampQ = std::llround(0.25 * q), hoverQ = std::llround(0.5 * q);
        const int64_t gravityCmS2 = Finite(gravity) ? std::max<int64_t>(std::llround(static_cast<double>(gravity) * 100.0), 1) : 981;

        zones.zones->ForEachLive(zones.tick, reachMin, reachMax, [&](const Zone& z) {
            if ((z.teamMask & zones.teamBit) == 0) return;
            const voxel::ShapeQ& sh = z.shape;
            const glm::i64vec3 a(sh.a), b(sh.b);
            const int64_t R = sh.radiusQ;
            if (R <= 0) return;
            switch (z.kind) {
            case ZoneKind::Impulse:
            case ZoneKind::Shove: {
                if (zones.tick != z.startTick || (state.status & PlayerStatus_Fresh) != 0) return;
                if (z.kind == ZoneKind::Shove && sh.kind != voxel::kShapeCapsule) return;
                glm::i64vec3 d;
                if (sh.kind == voxel::kShapeSphere) {
                    d = feet - a;
                } else if (sh.kind == voxel::kShapeCylinder) {
                    if (std::abs(feet.y - a.y) > int64_t{sh.halfHeightQ}) return;
                    d = glm::i64vec3(feet.x - a.x, 0, feet.z - a.z);
                } else if (sh.kind == voxel::kShapeCapsule) {
                    d = FromSegment(feet, a, b);
                } else {
                    return;
                }
                const int64_t len = Length(d);
                if (len >= R) return;
                const int64_t f = int64_t{z.strength[0]} * (R - len) / R;
                if (z.kind == ZoneKind::Impulse) {
                    // The dead radius keeps the push continuous through the centre: 0 there, never reversed by a millimetre.
                    const int64_t over = std::max<int64_t>(len, std::max<int64_t>(R / 8, 1));
                    AddPush(state, 0, d.x * f / over);
                    AddPush(state, 2, d.z * f / over);
                } else {
                    const glm::i64vec3 u = b - a;
                    const int64_t ul = static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(u.x * u.x + u.z * u.z)));
                    if (ul > 0) {
                        AddPush(state, 0, u.x * f / ul);
                        AddPush(state, 2, u.z * f / ul);
                    }
                }
                AddPush(state, 1, int64_t{z.strength[1]} * (R - len) / R);
                return;
            }
            case ZoneKind::Lift: {
                if (sh.kind != voxel::kShapeCylinder) return;
                const int64_t dx = feet.x - a.x, dz = feet.z - a.z;
                const int64_t dist = static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(dx * dx + dz * dz)));
                const int64_t bottom = a.y - int64_t{sh.halfHeightQ}, top = a.y + int64_t{sh.halfHeightQ};
                if (dist > R || feet.y < bottom || feet.y > top) return;
                // The target is a lower bound, so a player lifted at full strength coasts on: the ease starts at
                // least the coasting height (v^2 / 2g) below the top, and the player stops there and hovers.
                const int64_t s0 = std::max<int64_t>(z.strength[0], 0);
                const int64_t coastQ = std::llround(static_cast<double>(s0 * s0) / static_cast<double>(2 * gravityCmS2) / 100.0 * q);
                const int64_t easeQ = std::max(hoverQ, coastQ);
                const int64_t ramp = std::min(R - dist, rampQ), ease = std::min(std::max<int64_t>(top - feet.y, 0), easeQ);
                const int64_t target = rampQ > 0 && easeQ > 0 ? s0 * ramp * ease / (rampQ * easeQ) : s0;
                const float v = static_cast<float>(target) / 100.0f;
                vertical = vertical ? std::max(*vertical, v) : v;
                state.status = static_cast<uint8_t>(state.status | PlayerStatus_Lifted);
                return;
            }
            case ZoneKind::Chill: {
                if (!ZoneShapeHolds(z, glm::ivec3(feet))) return;
                state.slowLeft = std::max(state.slowLeft, static_cast<uint8_t>(std::clamp<int32_t>(z.strength[1], 0, 255)));
                state.slowQ = std::max(state.slowQ, static_cast<uint8_t>(std::clamp<int32_t>(z.strength[0], 0, 16)));
                return;
            }
            case ZoneKind::Root: {
                if (zones.tick != z.startTick || state.ccGuardLeft > 0 || !ZoneShapeHolds(z, glm::ivec3(feet))) return;
                state.rootLeft = std::max(state.rootLeft, static_cast<uint8_t>(std::min<uint32_t>(z.duration, 255)));
                return;
            }
            case ZoneKind::Pin: {
                if (state.ccGuardLeft > 0 || !ZoneShapeHolds(z, glm::ivec3(feet))) return;
                state.pinLeft = std::max(state.pinLeft, static_cast<uint8_t>(std::clamp<int32_t>(z.strength[1], 0, 255)));
                return;
            }
            case ZoneKind::Release: {
                if (zones.tick != z.startTick || !ZoneShapeHolds(z, glm::ivec3(feet))) return;
                const bool held = state.rootLeft > 0 || state.pinLeft > 0;
                state.rootLeft = state.pinLeft = state.slowLeft = 0;
                state.slowQ = 0;
                if (held) state.ccGuardLeft = kControlGuardTicks;
                return;
            }
            default:
                return;
            }
        });
        return vertical;
    }

    void SetPlayAreaWalls(physics::PhysicsWorld& physics, const PlayArea& area, const PlayerTuning& tuning) {
        if (!area.Bounded()) {
            physics.SetCharacterWalls(glm::vec2(0.0f), glm::vec2(0.0f));
            return;
        }
        physics.SetCharacterWalls(area.min - glm::vec2(tuning.radius), area.max + glm::vec2(tuning.radius));
    }

    PlayerStepResult StepPlayer(physics::PhysicsWorld& physics, const physics::TerrainColliders& terrain, physics::CharacterId character,
                                PlayerState& state, const PlayerInput& input, const PlayerTuning& tuning, const PlayArea& area, float dt) {
        return StepPlayer(physics, terrain, character, state, input, tuning, area, PlayerZones{}, dt);
    }

    PlayerStepResult StepPlayer(physics::PhysicsWorld& physics, const physics::TerrainColliders& terrain, physics::CharacterId character,
                                PlayerState& state, const PlayerInput& input, const PlayerTuning& tuning, const PlayArea& area,
                                const PlayerZones& zones, float dt) {
        PlayerStepResult r;
        r.pressed = PressButtons(state, input.buttons);
        if (state.cooldownTicks > 0) --state.cooldownTicks;
        r.cast = TakeCast(state, r.pressed, tuning);
        if ((r.pressed & PlayerButton_Spell) != 0 && (r.pressed & PlayerButton_Cast) == 0) {
            // The blast's cooldown is its own: a spell's gap and mana are the server's cast check.
            r.spell = true;
            if (zones.spellMoveAllowed) r.spellMove = StartSpellMove(state, input.spell, LookDirection(input.yaw, input.pitch));
        }
        const bool launched = r.spellMove && input.spell == kMeteorLeap;
        // A cast ends Fresh (the guard itself runs on).
        if (r.cast || r.spell) state.status = static_cast<uint8_t>(state.status & ~PlayerStatus_Fresh);

        // The gate: chunks loaded and a collider built at least once (never "re-cooked yet").
        if (!terrain.HasCollider(PlayerBounds(state.position, tuning))) {
            state.flags = static_cast<uint8_t>(state.flags | PlayerFlag_Held);
            r.held = true;
            return r;
        }
        std::optional<float> vertical = ApplyZones(state, zones, tuning, -physics.Gravity().y);
        const bool rooted = state.rootLeft > 0, pinned = state.pinLeft > 0;
        // A root ends a dash where it stands.
        if (rooted) state.dashLeft = 0;
        // The leap's caster is its projectile: no wish and no slick rule until it lands.
        const bool leaping = (state.status & PlayerStatus_Leaping) != 0;
        const bool slick = !rooted && !leaping && state.OnGround() && OnSlickGround(zones.world, state.position);

        const bool run = (input.buttons & PlayerButton_Run) != 0;
        glm::vec3 wish = MoveDirection(input) * (run ? tuning.runSpeed : tuning.walkSpeed);
        if (rooted) wish = glm::vec3(0.0f);
        else if (pinned) wish *= 6.0f / 16.0f;
        if (state.slowLeft > 0 && state.slowQ > 0) wish *= static_cast<float>(16 - std::min<int>(state.slowQ, 16)) / 16.0f;
        const bool dashing = state.dashLeft > 0;
        const glm::vec2 dashDir = DashDirection(state);
        const float dashSpeed = static_cast<float>(kDashSpeedCmS) / 100.0f * static_cast<float>(16 - std::min<int>(state.slowQ, 16)) / 16.0f;
        if (dashing) wish = glm::vec3(dashDir.x, 0.0f, dashDir.y) * dashSpeed;
        if (leaping) wish = glm::vec3(0.0f);
        if (slick) wish *= static_cast<float>(kSlickWishNum) / 16.0f;
        // Only a push that is there changes the wish: a step without one stays bit for bit as before.
        if (state.push[0] != 0) wish.x += static_cast<float>(state.push[0]) / 100.0f;
        if (state.push[2] != 0) wish.z += static_cast<float>(state.push[2]) / 100.0f;
        if (slick && dt > 0.0f) {
            // Momentum carries: the velocity turns toward the wish by a bounded acceleration.
            const glm::vec2 v(state.velocity.x, state.velocity.z);
            glm::vec2 change = glm::vec2(wish.x, wish.z) - v;
            const float most = static_cast<float>(kSlickAccelCmS2) / 100.0f * dt;
            const float length = std::sqrt(change.x * change.x + change.y * change.y);
            if (length > most) change *= most / length;
            glm::vec2 next = v + change;
            const float top = tuning.runSpeed * static_cast<float>(kSlickTopSpeedNum) / 16.0f;
            const float speed = std::sqrt(next.x * next.x + next.y * next.y);
            if (speed > top) next *= top / speed;
            wish.x = next.x;
            wish.z = next.y;
        }
        const bool bounded = area.Bounded() && dt > 0.0f;
        if (bounded) {
            // The play area: the outward wish stops at its edge, before the character moves. A push cut
            // there is spent: a knockback never carries a player past the play bounds.
            const glm::vec3 uncut = wish;
            wish.x = LimitToArea(state.position.x, wish.x, area.min.x, area.max.x, dt);
            wish.z = LimitToArea(state.position.z, wish.z, area.min.y, area.max.y, dt);
            if (wish.x != uncut.x) state.push[0] = 0;
            if (wish.z != uncut.z) state.push[2] = 0;
        }
        if (state.push[1] != 0) {
            if (state.push[1] > 0) {
                const float kick = static_cast<float>(state.push[1]) / 100.0f;
                vertical = vertical ? std::max(*vertical, kick) : kick;
            }
            state.push[1] = 0;
        }
        const bool jump = (r.pressed & PlayerButton_Jump) != 0 && !rooted && !pinned;
        r.jumped = jump && state.OnGround();
        const glm::vec3 before = state.position;
        physics.UpdateCharacter(character, dt, wish, jump ? tuning.jumpSpeed : 0.0f, vertical);
        physics::CharacterState cs = physics.GetCharacterState(character);
        if (bounded) {
            // The walls (SetPlayAreaWalls) stop the capsule at the edge; what float rounding leaves
            // past it is taken back along that axis. A player outside the area never goes further out.
            const float bx = AreaBound(state.position.x, cs.position.x, area.min.x, area.max.x);
            const float bz = AreaBound(state.position.z, cs.position.z, area.min.y, area.max.y);
            if (bx != cs.position.x || bz != cs.position.z) {
                if (bx != cs.position.x) cs.velocity.x = 0.0f;
                if (bz != cs.position.z) cs.velocity.z = 0.0f;
                cs.position.x = bx;
                cs.position.z = bz;
                physics.SetCharacterState(character, cs.position, cs.velocity);
                r.pushedBack = true;
            }
        }
        state.position = cs.position;
        state.velocity = cs.velocity;
        state.flags = cs.onGround ? PlayerFlag_OnGround : 0;

        const bool landed = cs.onGround && !launched;
        if (landed) state.status = static_cast<uint8_t>(state.status & ~PlayerStatus_Lifted);
        if (leaping && landed) {
            // The leap ends where it lands: its horizontal push goes with it.
            state.status = static_cast<uint8_t>(state.status & ~PlayerStatus_Leaping);
            state.push[0] = state.push[2] = 0;
        }
        const bool flying = leaping && !landed;
        const uint16_t decay = flying ? uint16_t{0}
                               : cs.onGround ? (OnSlickGround(zones.world, state.position) ? kPushDecaySlick : tuning.pushDecayGround)
                                             : tuning.pushDecayAir;
        state.push[0] = Decay(state.push[0], decay);
        state.push[2] = Decay(state.push[2], decay);
        if (dashing) {
            // A wall (or a Barricade) that cuts the move to under half its length ends the dash.
            const float dx = state.position.x - before.x, dz = state.position.z - before.z;
            const float moved = std::sqrt(dx * dx + dz * dz), planned = dashSpeed * dt;
            if (moved < 0.5f * planned)
                state.dashLeft = 0;
            else
                --state.dashLeft;
            if (state.dashLeft == 0) {
                AddPush(state, 0, std::lround(dashDir.x * static_cast<float>(kDashEndPushCmS)));
                AddPush(state, 2, std::lround(dashDir.y * static_cast<float>(kDashEndPushCmS)));
            }
        }
        CountDownStatuses(state);
        return r;
    }

    void RestorePlayer(physics::PhysicsWorld& physics, physics::CharacterId character, const PlayerState& state) {
        physics.SetCharacterState(character, state.position, state.velocity);
    }
}
