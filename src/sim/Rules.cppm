module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

export module vortex.sim:rules;

import vortex.voxel;
import vortex.scene;
import :simulation;
import :worldop;
import :blast;
import :region;
import :player;
import :rope;

// docs/design/rules.md "The seam": what an action does, GPU-free; the server, the offline game and a
// client's presentation share it. [wp-6]
export namespace vortex::sim {

    /// @brief A team. 0 = none (the sandbox, offline). Teams are numbered from 1 ([wp-7]).
    using TeamId = uint8_t;
    inline constexpr TeamId kNoTeam = 0;

    /// @brief What a player uses. Internal, not a wire value: PlayerButton_Cast triggers Blast.
    enum class ActionSlot : uint8_t {
        Blast = 0,   ///< the left-click blast (PlayerTuning castRadius / castPower)
    };

    /// @brief Why an action did nothing. Values equal net::CastRejection's (net maps them, RejectionOf).
    enum class ActionRefusal : uint8_t {
        None = 0,
        Cooldown = 1,   ///< judged by sim::TakeCast in StepPlayer, never by the seam
        Missed = 2,     ///< the ray met nothing within AimRules::range
        Outside = 3,    ///< the sphere does not fit the region (sim::CastFitsRegion)
    };

    /// @brief One use of an action slot, as captured (the server's cast queue holds it).
    struct ActionRequest {
        uint8_t player = 0;                  ///< slot + 1 of the player; 0 = no player (server cast, offline)
        ActionSlot slot = ActionSlot::Blast;
        uint32_t inputTick = 0;              ///< the input whose edge made it (0 offline)
        glm::dvec3 eye{0.0};                 ///< ray origin, world metres
        glm::vec3 dir{0.0f, 0.0f, -1.0f};    ///< unit look direction
        uint8_t spell = 0;                   ///< [mag-2]; 0 now
    };

    /// @brief A player as the rules see it (the session server fills the list in a fight mode; [wp-7]).
    struct PlayerView {
        uint8_t player = 0;                  ///< slot + 1
        TeamId team = kNoTeam;
        glm::vec3 feet{0.0f};
        bool alive = true;
    };
    /// @brief How a ray becomes a blast (decision 5).
    struct AimRules {
        float range = 60.0f;                 ///< metres from the eye
        float terrainRadius = 1.2f;          ///< metres, when terrain takes the cast
        float bodyRadius = 1.2f;             ///< metres, when a body takes the cast
        float power = 6.0f;                  ///< Material::hardness units
        uint8_t bodyLayers = 0;              ///< physics::LayerBit mask of bodies that may take it; 0 = every layer
        bool replicatedBodiesOnly = false;   ///< a body must carry scene::NetId (a session server)
        /// Offline: the body ray runs the whole range and a body takes the cast only when strictly nearer
        /// than the terrain hit. Else the body ray stops at the terrain hit and any hit on it counts.
        bool strictBodyFirst = false;
        const RegionDesc* region = nullptr;  ///< set: CastFitsRegion decides Outside; null: no check
        /// The players' capsule (PlayerTuning radius / height; review of [wp-7]). Above 0, a living enemy of the actor
        /// in AimAction's `players` takes the cast where the ray meets its capsule, when nearer than the terrain and the
        /// bodies; the blast is then centred there with `terrainRadius`. 0: players never take a cast (offline).
        float playerRadius = 0.0f;
        float playerHeight = 0.0f;
    };
    /// @brief The session server's rules: castRange, castRadius on terrain and bodies, castPower, Dynamic | Rubble
    /// bodies with a NetId, the players' capsule, the region check. `region` must outlive the returned rules.
    AimRules SessionAimRules(const PlayerTuning& tuning, const RegionDesc& region);
    /// @brief The offline click's rules: 80 m, 1.2 m on terrain, 0.6 m on a body, power 6, every layer,
    /// strictBodyFirst, no region.
    AimRules OfflineAimRules();

    /// @brief Where an action lands.
    struct ActionAim {
        bool hit = false;
        bool atBody = false;                 ///< a body took it (counted by the server: castsAtBodies)
        uint8_t player = 0;                  ///< slot + 1 of the enemy whose capsule took it; 0 = none
        float distance = 0.0f;               ///< metres from the eye
        glm::dvec3 point{0.0};               ///< world metres
        glm::ivec3 voxel{0};                 ///< the voxel hit (World::WorldToVoxel of `point` for a body)
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        voxel::SphereQ sphere;               ///< the blast, quantised (1/16 voxel)
        uint16_t power = 0;                  ///< 1/16 hardness
        scene::Entity body = scene::kNoEntity;   ///< the body that took it (atBody); a mechanism takes a MechanismBlast
    };
    /// @brief Casts the request's ray on the simulation as it is now, and on the capsules of the living enemies of
    /// `request.player` in `players` (with AimRules::playerRadius above 0). Fills `out` (also on Outside: `hit`,
    /// `atBody`, `player`, `sphere`); returns None, Missed or Outside.
    ActionRefusal AimAction(Simulation& simulation, const ActionRequest& request, const AimRules& rules, ActionAim& out,
                            std::span<const PlayerView> players = {});

    /// @brief Health taken from a player (docs/design/fight.md "Damage"; [wp-7]).
    struct DamageEvent {
        uint8_t target = 0;                  ///< slot + 1
        uint8_t source = 0;                  ///< slot + 1 of the actor; 0 = none
        int16_t amount = 0;
        uint8_t cause = 0;                   ///< sim::DamageCause
    };
    /// @brief A velocity change of a player, cm/s per axis ([mag-1] fills; empty until then).
    struct PlayerImpulse {
        uint8_t target = 0;
        std::array<int16_t, 3> dvCm{};
    };
    enum class EffectKind : uint8_t {
        Blast = 1,   ///< a blast's flash and burst; centre and radius of its sphere
    };
    /// @brief Something to show, in the integers of the world operation it belongs to.
    struct EffectCue {
        EffectKind kind = EffectKind::Blast;
        glm::ivec3 centreQ{0};               ///< 1/16 voxel
        uint16_t radiusQ = 0;                ///< 1/16 voxel
        friend bool operator==(const EffectCue&, const EffectCue&) = default;
    };
    /// @brief Everything an action did. `blasts` hold the applied operations in order (the server emits them).
    struct ActionOutcome {
        ActionRefusal refusal = ActionRefusal::None;
        std::vector<BlastResult> blasts;
        std::vector<DamageEvent> damage;
        std::vector<PlayerImpulse> impulses;
        std::vector<EffectCue> effects;
        std::vector<RopeCutEvent> ropeCuts;   ///< ropes the cast's ray passed or its sphere reached ([mech-2])
    };

    /// @brief A blast's damage: `centre` at distance 0, falling linearly to 0 at `zeroMm` ([wp-7]). centre 0 = none.
    struct BlastDamageRule {
        int16_t centre = 0;
        uint16_t zeroMm = 0;
        friend bool operator==(const BlastDamageRule&, const BlastDamageRule&) = default;
    };
    /// @brief What ResolveAction may read besides the world.
    struct RulesContext {
        const PlayerTuning& tuning;
        std::span<const PlayerView> players{};
        uint32_t tick = 0;
        BlastDamageRule blastDamage{};
    };
    /// @brief Applies the action: a Blast slot runs sim::Blast(aim.sphere, aim.power) and adds its effect cue; with
    /// `context.blastDamage.centre` > 0 every living enemy of the actor in `context.players` (span order) within the
    /// rule's reach gets a DamageEvent (fight.md "Damage"). `aim.hit` false: refusal Missed, nothing applied.
    ActionOutcome ResolveAction(Simulation& simulation, const RulesContext& context, const ActionRequest& request, const ActionAim& aim);

    /// @brief The cue of a CarveSphere operation (a client's view of a cast it applied).
    EffectCue BlastCueOf(const WorldOp& carve);
    /// @brief Cue centre in world metres: dvec3(centreQ) * (1/16 * voxelSize), in that order of operations.
    glm::dvec3 CueCentreMetres(const EffectCue& cue, float voxelSize);
    /// @brief Cue radius in metres: float(double(radiusQ) * (1/16) * voxelSize).
    float CueRadiusMetres(const EffectCue& cue, float voxelSize);
}
