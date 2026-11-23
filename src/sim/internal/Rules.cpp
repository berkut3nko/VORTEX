module;

#include <glm/glm.hpp>

#include <span>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;

// docs/design/rules.md "The seam" and "Aim rules": the blast of a player's cast, aimed and resolved here for the
// session server and the offline game. [wp-6]
namespace vortex::sim {

    AimRules SessionAimRules(const PlayerTuning& tuning, const RegionDesc& region) {
        AimRules rules;
        rules.range = tuning.castRange;
        rules.terrainRadius = tuning.castRadius;
        rules.bodyRadius = tuning.castRadius;
        rules.power = tuning.castPower;
        rules.bodyLayers = physics::LayerBit(physics::Layer::Dynamic) | physics::LayerBit(physics::Layer::Rubble);
        rules.replicatedBodiesOnly = true;
        rules.strictBodyFirst = false;
        rules.region = &region;
        return rules;
    }

    AimRules OfflineAimRules() {
        AimRules rules;
        rules.range = 80.0f;
        rules.terrainRadius = 1.2f;
        rules.bodyRadius = 0.6f;
        rules.power = 6.0f;
        rules.bodyLayers = 0;
        rules.replicatedBodiesOnly = false;
        rules.strictBodyFirst = true;
        rules.region = nullptr;
        return rules;
    }

    ActionRefusal AimAction(Simulation& simulation, const ActionRequest& request, const AimRules& rules, ActionAim& out) {
        out = ActionAim{};
        const double vs = static_cast<double>(simulation.World().VoxelSize());
        const voxel::WorldRayHit terrain = simulation.World().Raycast(request.eye, request.dir, rules.range);
        // The session's body ray stops at the terrain (a replicated body in front of the terrain takes the cast);
        // offline it runs the whole range and the nearer hit wins.
        const float bodyMax = rules.strictBodyFirst ? rules.range : (terrain.hit ? terrain.distance : rules.range);
        const physics::RayHit body = rules.bodyLayers != 0
                                         ? simulation.Physics().Raycast(glm::vec3(request.eye), request.dir, bodyMax, rules.bodyLayers)
                                         : simulation.Physics().Raycast(glm::vec3(request.eye), request.dir, bodyMax);
        bool bodyTakes = body.hit;
        if (rules.replicatedBodiesOnly)
            bodyTakes = bodyTakes && simulation.Scene().TryGet<scene::NetId>(simulation.Scene().EntityFromUserData(body.userData)) != nullptr;
        if (rules.strictBodyFirst) bodyTakes = bodyTakes && (!terrain.hit || body.distance < terrain.distance);
        float radius = 0.0f;
        if (bodyTakes) {
            out.hit = true;
            out.atBody = true;
            out.point = glm::dvec3(body.position);
            out.distance = body.distance;
            out.normal = body.normal;
            out.voxel = simulation.World().WorldToVoxel(out.point);
            radius = rules.bodyRadius;
        } else if (terrain.hit) {
            out.hit = true;
            out.point = terrain.position;
            out.distance = terrain.distance;
            out.normal = terrain.normal;
            out.voxel = terrain.voxel;
            radius = rules.terrainRadius;
        } else {
            return ActionRefusal::Missed;
        }
        // The centre is the hit point in fractional voxels, as sim::Blast(Simulation&, const BlastDesc&) quantises it.
        out.sphere = voxel::QuantizeSphere(out.point / vs, static_cast<double>(radius) / vs);
        out.power = voxel::QuantizePower(rules.power);
        if (rules.region != nullptr && !CastFitsRegion(*rules.region, simulation.World(), out.sphere)) return ActionRefusal::Outside;
        return ActionRefusal::None;
    }

    ActionOutcome ResolveAction(Simulation& simulation, const RulesContext& context, const ActionRequest& request, const ActionAim& aim) {
        ActionOutcome outcome;
        if (!aim.hit) {
            outcome.refusal = ActionRefusal::Missed;
            return outcome;
        }
        switch (request.slot) {
            case ActionSlot::Blast:
                outcome.blasts.push_back(Blast(simulation, aim.sphere, aim.power));
                outcome.effects.push_back(EffectCue{EffectKind::Blast, aim.sphere.center, aim.sphere.radius});
                break;
        }
        // Damage: [wp-7] reads context.players and context.blastDamage here (rules.md "Outcome").
        (void)context;
        return outcome;
    }

    EffectCue BlastCueOf(const WorldOp& carve) { return EffectCue{EffectKind::Blast, carve.sphere.center, carve.sphere.radius}; }

    namespace {
        constexpr double kInvQ = 1.0 / static_cast<double>(voxel::kQuantaPerVoxel);
    }

    glm::dvec3 CueCentreMetres(const EffectCue& cue, float voxelSize) {
        return glm::dvec3(cue.centreQ) * (kInvQ * static_cast<double>(voxelSize));
    }

    float CueRadiusMetres(const EffectCue& cue, float voxelSize) {
        return static_cast<float>(static_cast<double>(cue.radiusQ) * kInvQ * static_cast<double>(voxelSize));
    }
}
