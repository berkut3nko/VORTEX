module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.physics;

namespace vortex::sim {

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
        state.cooldownTicks = tuning.castCooldownTicks;
        return true;
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
        PlayerStepResult r;
        r.pressed = PressButtons(state, input.buttons);
        if (state.cooldownTicks > 0) --state.cooldownTicks;
        r.cast = TakeCast(state, r.pressed, tuning);

        // The gate: chunks loaded and a collider built at least once (never "re-cooked yet").
        if (!terrain.HasCollider(PlayerBounds(state.position, tuning))) {
            state.flags = static_cast<uint8_t>(state.flags | PlayerFlag_Held);
            r.held = true;
            return r;
        }
        const bool run = (input.buttons & PlayerButton_Run) != 0;
        glm::vec3 wish = MoveDirection(input) * (run ? tuning.runSpeed : tuning.walkSpeed);
        const bool bounded = area.Bounded() && dt > 0.0f;
        if (bounded) {
            // The play area: the outward wish stops at its edge, before the character moves.
            wish.x = LimitToArea(state.position.x, wish.x, area.min.x, area.max.x, dt);
            wish.z = LimitToArea(state.position.z, wish.z, area.min.y, area.max.y, dt);
        }
        const bool jump = (r.pressed & PlayerButton_Jump) != 0;
        r.jumped = jump && state.OnGround();
        physics.UpdateCharacter(character, dt, wish, jump ? tuning.jumpSpeed : 0.0f);
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
        return r;
    }

    void RestorePlayer(physics::PhysicsWorld& physics, physics::CharacterId character, const PlayerState& state) {
        physics.SetCharacterState(character, state.position, state.velocity);
    }
}
