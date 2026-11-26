module;

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

export module vortex.sim:player;

import vortex.core;
import vortex.math;
import vortex.physics;
import vortex.voxel;
import :zones;
import :spell;

export namespace vortex::sim {

    /// @brief Buttons of a PlayerInput. They are **levels** (held or not); edges are derived from
    /// the levels of the input before (PressButtons), so a repeated input never makes one. Wire format.
    enum PlayerButton : uint8_t {
        PlayerButton_Jump = 1,   ///< edge: jump if on the ground
        PlayerButton_Run = 2,    ///< level: run speed
        PlayerButton_Cast = 4,   ///< edge: a cast, if the cooldown allows (the server captures it)
        /// edge: a crate (the game's Q key) dropped in front of the eye; a session server makes it
        /// (networking stage 6), nothing is predicted.
        PlayerButton_Crate = 8,
        PlayerButton_Use = 16,   ///< edge: E, a use (mechanisms.md); the crate moves to Q
        /// edge: RMB, the selected spell (PlayerInput::spell; magic.md "What a player carries"). It shares the
        /// cast's cooldown; a step with both edges takes the Cast and drops the Spell.
        PlayerButton_Spell = 32,
    };
    inline constexpr uint8_t kPlayerButtonMask = 63;

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
        uint8_t spell = 0;       ///< The selected SpellId; read on a Spell edge only.
        friend bool operator==(const PlayerInput&, const PlayerInput&) = default;
        /// @brief Equal apart from the tick.
        bool SameControls(const PlayerInput& o) const {
            return moveX == o.moveX && moveZ == o.moveZ && yaw == o.yaw && pitch == o.pitch && buttons == o.buttons && spell == o.spell;
        }
    };

    /// @brief PlayerState::status bits (docs/design/magic.md "PlayerState, StepPlayer and the physics call").
    enum PlayerStatus : uint8_t {
        PlayerStatus_Rooted = 1,
        PlayerStatus_Pinned = 2,
        PlayerStatus_Chilled = 4,
        PlayerStatus_Lifted = 8,   ///< set while in a Lift, cleared on the first step that ends on the ground
        PlayerStatus_Free = 16,
        PlayerStatus_Fresh = 32,
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
        /// External velocity in cm/s (knockback, a dash's carry). x and z are added to the wish and
        /// decay; y is a one-shot upward kick taken by the next step. Each axis within +-kPushLimitCmS.
        int16_t push[3] = {0, 0, 0};
        uint8_t status = 0;          ///< PlayerStatus bits
        uint8_t slowQ = 0;           ///< speed cut in sixteenths while slowLeft > 0
        uint8_t slowLeft = 0, rootLeft = 0, pinLeft = 0, ccGuardLeft = 0;   ///< ticks
        uint8_t dashLeft = 0;        ///< ticks of a dash still to run (StartDash)
        int8_t dashDir[2] = {0, 0};  ///< the dash's horizontal direction (x, z) in 1/127 units
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
        uint16_t pushDecayGround = 45;   ///< cm/s per tick: PlayerState::push x and z toward 0 on the ground
        uint16_t pushDecayAir = 1;       ///< cm/s per tick in the air
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
     * correcting each other. 1: the play area (WP1, 2026-11-19). 2: foliage (leaves, needles, reeds,
     * ferns, flowers) has no terrain collision: players walk through plants (land.md "Nature", 2026-11-25).
     * 3: push, the Impulse, Lift and Shove zones, the dash and the vertical parameter of
     * UpdateCharacter (magic.md "PlayerState, StepPlayer and the physics call", 2026-11-26).
     * 4: Chill, Root and Release zones, the control guard, Fresh and slick ground (magic.md, 2026-11-26).
     */
    inline constexpr uint32_t kPlayerRuleRevision = 4;

    /// @brief Each axis of PlayerState::push stays within +- this (cm/s).
    inline constexpr int32_t kPushLimitCmS = 1500;
    /// @brief A dash (docs/design/magic.md, spell 11): its length in ticks, its speed and the push it
    /// leaves along its direction on its last tick. The spell table takes these values.
    inline constexpr uint8_t kDashTicks = 18;
    inline constexpr uint16_t kDashSpeedCmS = 2670;
    inline constexpr uint16_t kDashEndPushCmS = 400;
    /// @brief The control guard (magic.md "PlayerState, StepPlayer and the physics call"): after a root or a
    /// pin ends, and after a respawn (with Fresh), Root and Pin zones do not apply for this many ticks.
    inline constexpr uint8_t kControlGuardTicks = 90;
    inline constexpr uint8_t kFreshGuardTicks = 120;
    /// @brief Slick ground: the wish is scaled by kSlickWishNum / 16 and the horizontal velocity moves
    /// toward it by at most kSlickAccelCmS2 (cm/s^2), its speed capped at kSlickTopSpeedNum / 16 of the
    /// run speed; a push decays by kPushDecaySlick (cm/s per tick) there.
    inline constexpr int32_t kSlickWishNum = 21;        // 1.3
    inline constexpr int32_t kSlickAccelCmS2 = 300;     // 3 m/s^2
    inline constexpr int32_t kSlickTopSpeedNum = 22;    // 1.4
    inline constexpr uint16_t kPushDecaySlick = 6;

    /// @brief What a step reads besides the player: the zones live at the stepped tick.
    struct PlayerZones {
        const ZoneSet* zones = nullptr;          ///< nullptr: no zones
        const voxel::World* world = nullptr;     ///< its voxel size gives q (nullptr: 0.125 m)
        uint32_t tick = 0;                       ///< the tick being stepped (a replay: that entry's own tick)
        uint8_t teamBit = 1;                     ///< 1u << the player's team (no team: team 0)
        /// A Spell edge may start the caster's own move (StartSpellMove): the stepping side found the
        /// player alive, with a charge and the spell's form ready, as far as it knows them. The server
        /// decides it from its own state; a predicting client from the owner block it last had.
        bool spellMoveAllowed = true;
    };

    /// @brief What one StepPlayer did.
    struct PlayerStepResult {
        uint8_t pressed = 0;     ///< Button edges of this input.
        bool held = false;       ///< The gate held the player: nothing moved.
        bool jumped = false;     ///< A jump was asked for on the ground.
        bool cast = false;       ///< A cast edge the cooldown accepted (the server captures it; a client shows it).
        bool spell = false;      ///< A Spell edge the cooldown accepted (no Cast edge in the same input).
        bool spellMove = false;  ///< ... and it started the caster's own move (StartSpellMove).
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
    /// @brief The cooldown a cast restarts: `castCooldownTicks * (16 + slowQ / 2) / 16` (at most 255).
    uint8_t CastCooldownFor(const PlayerState& state, const PlayerTuning& tuning);

    /**
     * @brief Starts a dash along the horizontal part of `aim`: `dashLeft` = kDashTicks and `dashDir`
     * its direction in 1/127 units. Refused (false, nothing changes) while rooted or pinned, or when
     * `aim` has no finite horizontal part.
     */
    bool StartDash(PlayerState& state, const glm::vec3& aim);
    /**
     * @brief The caster's own move of `spell`, started at its Spell edge inside StepPlayer at the
     * input's tick, on the server and on the predicting client alike (magic.md "PlayerState,
     * StepPlayer and the physics call" step 1): Dash calls StartDash. False for a spell without an
     * own move, or one the state refuses.
     */
    bool StartSpellMove(PlayerState& state, SpellId spell, const glm::vec3& aim);
    /// @brief A respawn's state (the server's teleport keeps it): Fresh, and the control guard for
    /// kFreshGuardTicks. While Fresh, Root, Pin, Impulse and Shove do not apply; a cast ends Fresh.
    void StartFreshLife(PlayerState& state);

    /**
     * @brief The voxel under a player (magic.md "PlayerState, StepPlayer and the physics call", step
     * 5): the voxel that holds the feet quantised to q, less half a voxel (8 q) in y; never the float
     * feet's floor, which flips at a surface.
     */
    glm::ivec3 GroundVoxel(const glm::vec3& feet, float voxelSize);
    /// @brief The ground voxel of `feet` holds a MaterialFlag_Slick id (false without a world or materials).
    bool OnSlickGround(const voxel::World* world, const glm::vec3& feet);

    /**
     * @brief The zones' part of a step (StepPlayer calls it after the gate), from the feet quantised
     * to q (round to nearest) at `zones.tick`, in (startTick, zoneId) order, for the zones whose
     * team mask has `zones.teamBit`.
     * @details Impulse (start tick only): away from the centre (a capsule: from the nearest point of
     * its axis; a cylinder: horizontally), `f = strength0 * (R - len) / R`, the horizontal part
     * `d * f / max(len, R / 8)`, plus `strength1 * (R - len) / R` up. Shove (start tick only, a
     * capsule): `f` along the axis's horizontal direction, the same lift. Lift (while live, a
     * y-cylinder): a vertical speed target ramped to 0 over the outer 0.25 m and eased linearly to
     * 0 at the top over 0.5 m or the coasting height of its strength under `gravity` (m/s^2, down),
     * whichever is longer, so a lifted player stops at the top and hovers; sets
     * PlayerStatus_Lifted. Chill (while live, inside): `slowLeft = max(slowLeft, strength1)`,
     * `slowQ = max(slowQ, strength0)`. Root (start tick, inside, no guard running): `rootLeft =
     * max(rootLeft, duration)`. Release (start tick, inside): root, pin and chill end, and the control
     * guard starts if a root or a pin did. While Fresh, Impulse and Shove do not apply either.
     * Integers in cm/s; `push` is clamped to +-kPushLimitCmS per axis.
     * @return The Lift's vertical target in m/s (the highest of the zones), or nothing.
     */
    std::optional<float> ApplyZones(PlayerState& state, const PlayerZones& zones, const PlayerTuning& tuning, float gravity = 9.81f);

    /**
     * @brief One tick of a player: the single movement rule of the server, the predicting client
     * and the offline game (docs/design/networking.md "Player movement").
     * @details In order: button edges (PressButtons), cooldown down by one, a cast edge
     * (TakeCast), else a Spell edge through the same TakeCast (when `zones.spellMoveAllowed`, its
     * own move: StartSpellMove along the input's look), then the gate: if `terrain.HasCollider(PlayerBounds(state.position))` is false
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
     * Zones and push (docs/design/magic.md "PlayerState, StepPlayer and the physics call"): after the
     * gate, ApplyZones. A dash (`dashLeft > 0`) replaces the input's wish with its own; the
     * horizontal push is added to the wish before the play-area cut, and an axis the cut reduced
     * loses its push; a positive vertical push and a Lift become UpdateCharacter's `vertical`. After
     * the physics call: a dash whose horizontal move was cut to under half its length ends, and a
     * dash's last tick leaves kDashEndPushCmS of push along it; on the ground PlayerStatus_Lifted is
     * cleared; the horizontal push decays toward 0 (pushDecayGround, kPushDecaySlick or pushDecayAir).
     * Statuses: a rooted player has no wish, no jump and no dash (a root ends one); a pinned player
     * 6/16 of the wish and no jump; a chilled one `(16 - slowQ) / 16` of it. On slick ground
     * (OnSlickGround, the state on the ground before the step) the horizontal velocity moves from the
     * last one toward the wish x 1.3 (plus the push) by at most 3 m/s^2, capped at 1.4 x the run speed.
     * Then the counters count down: a chill ends with slowQ 0; when the last of root and pin ends, the
     * control guard starts (kControlGuardTicks); Fresh ends with the guard or a cast.
     */
    PlayerStepResult StepPlayer(physics::PhysicsWorld& physics, const physics::TerrainColliders& terrain, physics::CharacterId character,
                                PlayerState& state, const PlayerInput& input, const PlayerTuning& tuning, const PlayArea& area,
                                const PlayerZones& zones, float dt);
    /// @brief The same step with no zones.
    PlayerStepResult StepPlayer(physics::PhysicsWorld& physics, const physics::TerrainColliders& terrain, physics::CharacterId character,
                                PlayerState& state, const PlayerInput& input, const PlayerTuning& tuning, const PlayArea& area, float dt);

    /// @brief Puts the character into `state` (physics::PhysicsWorld::SetCharacterState): before a replay.
    void RestorePlayer(physics::PhysicsWorld& physics, physics::CharacterId character, const PlayerState& state);
}
