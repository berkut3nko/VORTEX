module;

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <string>

export module vortex.sim:player;

import vortex.core;
import vortex.math;
import vortex.physics;

export namespace vortex::sim {

    /// @brief Buttons of a PlayerInput. They are **levels** (held or not); edges are derived from
    /// the levels of the input before (PressButtons), so a repeated input never makes one. Wire format.
    enum PlayerButton : uint8_t {
        PlayerButton_Jump = 1,   ///< edge: jump if on the ground
        PlayerButton_Run = 2,    ///< level: run speed
        PlayerButton_Cast = 4,   ///< edge: a cast, if the cooldown allows (the server captures it)
        /// edge: a crate (the game's E key) dropped in front of the eye; a session server makes it
        /// (networking stage 6), nothing is predicted.
        PlayerButton_Crate = 8,
        PlayerButton_Use = 16,   ///< edge: E, a use (mechanisms.md); the crate moves to Q
    };
    inline constexpr uint8_t kPlayerButtonMask = 15;

    /**
     * @brief What a player does in one tick (docs/design/networking.md "Player movement"): integers
     * only, so the server and the predicting client move with bit-equal inputs.
     */
    struct PlayerInput {
        uint32_t tick = 0;       ///< The server tick it is for (set by net::Client; ignored by StepPlayer).
        int8_t moveX = 0;        ///< Strafe, -127 (left) .. 127 (right); -128 counts as -127.
        int8_t moveZ = 0;        ///< Forward, -127 (back) .. 127 (forward).
        uint16_t yaw = 0;        ///< Look yaw, 2 pi / 65536 per unit (0 looks along -z, as the camera's).
        int16_t pitch = 0;       ///< Look pitch, (pi / 2) / 32767 per unit, positive up.
        uint8_t buttons = 0;     ///< PlayerButton levels.
        friend bool operator==(const PlayerInput&, const PlayerInput&) = default;
        /// @brief Equal apart from the tick.
        bool SameControls(const PlayerInput& o) const {
            return moveX == o.moveX && moveZ == o.moveZ && yaw == o.yaw && pitch == o.pitch && buttons == o.buttons;
        }
    };

    enum PlayerFlag : uint8_t {
        PlayerFlag_OnGround = 1,
        PlayerFlag_Held = 2,     ///< The last step was held: its terrain had no collider (StepPlayer's gate).
    };

    /**
     * @brief Everything that decides a player's next step besides its input and the world: what a
     * server sends its owner (unquantised) and what the owner restores before a replay.
     */
    struct PlayerState {
        glm::vec3 position{0.0f};    ///< Feet, world metres.
        glm::vec3 velocity{0.0f};
        uint8_t flags = 0;           ///< PlayerFlag
        uint8_t cooldownTicks = 0;   ///< Ticks until the next cast is accepted.
        uint8_t lastButtons = 0;     ///< Button levels of the last input: edges are derived against them.
        bool OnGround() const { return (flags & PlayerFlag_OnGround) != 0; }
        friend bool operator==(const PlayerState&, const PlayerState&) = default;
    };

    /**
     * @brief Movement and cast tuning. A session's comes from the server (net::Welcome), never from
     * the client: both sides must step with the same numbers.
     */
    struct PlayerTuning {
        float walkSpeed = 3.8f;          ///< m/s
        float runSpeed = 7.0f;           ///< m/s with PlayerButton_Run
        float jumpSpeed = 4.6f;          ///< m/s upwards at take-off
        float height = 1.8f;             ///< capsule, metres
        float radius = 0.3f;
        float eyeHeight = 1.62f;         ///< cast origin above the feet
        float maxSlopeDegrees = 50.0f;
        float stepHeight = 0.35f;
        uint8_t castCooldownTicks = 15;  ///< a cast is accepted at most every this many ticks
        float castRadius = 1.2f;         ///< metres (the game's left-click blast)
        float castPower = 6.0f;          ///< Material::hardness units
        float castRange = 60.0f;         ///< metres from the eye to the terrain it hits
        friend bool operator==(const PlayerTuning&, const PlayerTuning&) = default;
    };
    /**
     * @brief What a player's character collides with in a session, on the server and on every
     * client alike (physics::PhysicsConfig::characterLayers; ServerConfig and
     * net::Client::MakeSimulationConfig set it): terrain and Dynamic bodies (networking stage 6).
     * On the server those are its bodies (crates, replicated pieces); on a client its copies of them
     * are Dynamic only while they rest (Layer::Rubble while they move) and its own rubble is Debris
     * or Rubble, so nothing the server does not have blocks a predicted player, and a push or a
     * blocked step from a moving piece is a server result that arrives as a correction.
     */
    inline constexpr uint8_t kSessionCharacterLayers = physics::LayerBit(physics::Layer::Static) | physics::LayerBit(physics::Layer::Dynamic);

    /// @brief Finite and in range (network input: a client checks the server's before using it).
    bool IsValid(const PlayerTuning& tuning, std::string* why = nullptr);

    /**
     * @brief Where a player's feet may go, x and z in world metres (docs/design/networking.md
     * "Player movement", "The play area"). A session's is sim::PlayAreaOf(RegionDesc, tuning): the
     * play bounds less the capsule's radius, computed alike on the server and every client from
     * `Welcome`. The default is unbounded: the offline game has no play bounds.
     */
    struct PlayArea {
        glm::vec2 min{-std::numeric_limits<float>::infinity()};
        glm::vec2 max{std::numeric_limits<float>::infinity()};
        bool Bounded() const {
            return min.x > -std::numeric_limits<float>::max() || min.y > -std::numeric_limits<float>::max() ||
                   max.x < std::numeric_limits<float>::max() || max.y < std::numeric_limits<float>::max();
        }
        friend bool operator==(const PlayArea&, const PlayArea&) = default;
    };
    /// @brief Metres from `feet` (x, z) to the nearest edge of `area`: the smallest of the four
    /// distances, negative outside the area, +infinity for an unbounded area (offline). Presentation
    /// only (the HUD hint of [wp-5]): never an input of StepPlayer.
    float DistanceToPlayAreaEdge(const PlayArea& area, const glm::vec2& feet);
    /**
     * @brief Puts the walls of `area` into `physics` (PhysicsWorld::SetCharacterWalls at the area
     * grown by the capsule's radius: the play bounds), or removes them for an unbounded area. A
     * session server and every client call it when they build their simulation (StepPlayer relies
     * on them: they stop the capsule at the edge, down a steep outward face too).
     */
    void SetPlayAreaWalls(physics::PhysicsWorld& physics, const PlayArea& area, const PlayerTuning& tuning);
    /**
     * @brief The revision of the movement rule (StepPlayer and PlayAreaOf). Bump it whenever the
     * same inputs on the same world give another result: it is part of net::BuildHash, so a client
     * and a server of different revisions refuse each other ("another build") instead of
     * correcting each other. 1: the play area (WP1, 2026-11-19).
     */
    inline constexpr uint32_t kPlayerRuleRevision = 1;

    /// @brief What one StepPlayer did.
    struct PlayerStepResult {
        uint8_t pressed = 0;     ///< Button edges of this input.
        bool held = false;       ///< The gate held the player: nothing moved.
        bool jumped = false;     ///< A jump was asked for on the ground.
        bool cast = false;       ///< A cast edge the cooldown accepted (the server captures it; a client shows it).
        bool pushedBack = false; ///< The move ended past the play area's edge (float rounding at its walls): taken back along that axis.
    };

    /// @brief Radians <-> the integers of PlayerInput (round to nearest; yaw wraps, pitch clamps to +-pi/2).
    uint16_t QuantizeYaw(float radians);
    float YawRadians(uint16_t yaw);
    int16_t QuantizePitch(float radians);
    float PitchRadians(int16_t pitch);
    /// @brief -1 .. 1 -> -127 .. 127 (clamped; non-finite = 0).
    int8_t QuantizeAxis(float value);

    /// @brief The horizontal wish direction of an input, length at most 1 (diagonals are normalised).
    glm::vec3 MoveDirection(const PlayerInput& input);
    /// @brief Unit look vector of a yaw and pitch (the camera's Forward()).
    glm::vec3 LookDirection(uint16_t yaw, int16_t pitch);
    /// @brief The capsule's bounds at a feet position: what the gate tests.
    math::AABB PlayerBounds(const glm::vec3& feet, const PlayerTuning& tuning);
    /// @brief What one step from `feet` can touch: the capsule's bounds grown by 1 m (a fall of
    /// 60 m/s in one tick). A session brings these colliders up to date before every step
    /// (physics::TerrainColliders::FlushBox).
    math::AABB PlayerReach(const glm::vec3& feet, const PlayerTuning& tuning);
    /// @brief The character of a player at `feet`.
    physics::CharacterDesc PlayerCharacterDesc(const glm::vec3& feet, const PlayerTuning& tuning);

    /// @brief Derives the edges of `buttons` against `state.lastButtons` and makes them the new
    /// levels. A repeated input (same levels) never makes an edge.
    uint8_t PressButtons(PlayerState& state, uint8_t buttons);
    /// @brief A cast edge in `pressed` that the cooldown accepts: the cooldown restarts. Used by
    /// StepPlayer and by a server that receives a cast in an input for a tick it already simulated.
    bool TakeCast(PlayerState& state, uint8_t pressed, const PlayerTuning& tuning);

    /**
     * @brief One tick of a player: the single movement rule of the server, the predicting client
     * and the offline game (docs/design/networking.md "Player movement").
     * @details In order: button edges (PressButtons), cooldown down by one, a cast edge
     * (TakeCast), then the gate: if `terrain.HasCollider(PlayerBounds(state.position))` is false
     * the player is held (PlayerFlag_Held, nothing moves; the gate does not wait for re-cooks, see
     * HasCollider). Otherwise the character moves with the wish velocity (run or walk speed),
     * jumps on a Jump edge if on the ground, and `state` takes its new position, velocity and
     * ground flag. `character` must be in `state` already (created there, or RestorePlayer).
     * The play area (`area`; unbounded offline; its walls in `physics`, SetPlayAreaWalls): per
     * axis, the outward part of the wish is cut to what reaches the area's edge this tick, and the
     * walls stop whatever else carries the capsule out (a slide down an outward face, the body it
     * stands on), so it stops at the edge, slides along it and falls down it. What float rounding
     * leaves past the edge is taken back along that axis with the outward velocity zeroed
     * (`pushedBack`); a player outside the area (none should be) never goes further out. A move is
     * never undone. The gate never meets a chunk past the server's window (PlayAreaOf: the reach
     * stays a chunk inside it).
     * Integer inputs, fixed tick: what does not repeat bit for bit is Jolt's contact order, which
     * the reconciliation threshold absorbs.
     */
    PlayerStepResult StepPlayer(physics::PhysicsWorld& physics, const physics::TerrainColliders& terrain, physics::CharacterId character,
                                PlayerState& state, const PlayerInput& input, const PlayerTuning& tuning, const PlayArea& area, float dt);

    /// @brief Puts the character into `state` (physics::PhysicsWorld::SetCharacterState): before a replay.
    void RestorePlayer(physics::PhysicsWorld& physics, physics::CharacterId character, const PlayerState& state);
}
