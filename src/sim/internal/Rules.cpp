module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
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
        rules.playerRadius = tuning.radius;
        rules.playerHeight = tuning.height;
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

    namespace {
        /// The ray's first meeting with a sphere (centre `c`, radius `r`) at t in [0, maxT]; -1 if none. `d` is unit.
        double RaySphere(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& c, double r, double maxT) {
            const glm::dvec3 m = o - c;
            const double b = glm::dot(m, d), cc = glm::dot(m, m) - r * r;
            if (cc <= 0.0) return 0.0;                                  // the eye is inside
            const double disc = b * b - cc;
            if (b > 0.0 || disc < 0.0) return -1.0;
            const double t = -b - std::sqrt(disc);
            return t <= maxT ? t : -1.0;
        }

        /// The ray's first meeting with an upright capsule: feet at `feet`, radius `r`, height `h` (h >= 2r); -1 if
        /// none within maxT. `d` is unit.
        double RayCapsule(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& feet, double r, double h, double maxT) {
            const double y0 = feet.y + r, y1 = feet.y + std::max(h - r, r);
            double best = -1.0;
            auto take = [&best](double t) {
                if (t >= 0.0 && (best < 0.0 || t < best)) best = t;
            };
            // The side: a circle in x / z, between the two centres' heights.
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
            // The two caps.
            take(RaySphere(o, d, glm::dvec3(feet.x, y0, feet.z), r, maxT));
            take(RaySphere(o, d, glm::dvec3(feet.x, y1, feet.z), r, maxT));
            return best;
        }
    }

    ActionRefusal AimAction(Simulation& simulation, const ActionRequest& request, const AimRules& rules, ActionAim& out,
                            std::span<const PlayerView> players) {
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
        // Review of [wp-7]: a player is a CharacterVirtual with no body, so the rays above pass through it; the living
        // enemies' capsules are met here (server only: the aim is never predicted).
        uint8_t playerTakes = 0;
        double playerT = 0.0;
        if (rules.playerRadius > 0.0f && request.player != 0 && !players.empty()) {
            TeamId own = kNoTeam;
            for (const PlayerView& v : players)
                if (v.player == request.player) own = v.team;
            const glm::dvec3 d = glm::normalize(glm::dvec3(request.dir));
            double nearest = terrain.hit ? static_cast<double>(terrain.distance) : static_cast<double>(rules.range);
            if (bodyTakes) nearest = std::min(nearest, static_cast<double>(body.distance));
            for (const PlayerView& v : players) {
                if (!v.alive || v.player == request.player || (own != kNoTeam && v.team == own)) continue;
                const double t = RayCapsule(request.eye, d, glm::dvec3(v.feet), static_cast<double>(rules.playerRadius),
                                            static_cast<double>(rules.playerHeight), nearest);
                if (t >= 0.0 && t < nearest) {
                    nearest = t;
                    playerTakes = v.player;
                    playerT = t;
                }
            }
        }
        float radius = 0.0f;
        if (playerTakes != 0) {
            out.hit = true;
            out.player = playerTakes;
            out.point = request.eye + glm::normalize(glm::dvec3(request.dir)) * playerT;
            out.distance = static_cast<float>(playerT);
            out.normal = -request.dir;
            out.voxel = simulation.World().WorldToVoxel(out.point);
            radius = rules.terrainRadius;
        } else if (bodyTakes) {
            out.hit = true;
            out.atBody = true;
            out.body = simulation.Scene().EntityFromUserData(body.userData);
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
                // Ropes the ray passes before its hit are cut; they never stop the ray ([mech-2]).
                CutRopesAlongRay(simulation, request.eye, request.dir, aim.distance, simulation.Tick(), &outcome.ropeCuts);
                // A cast that resolved on a mechanism ([mech-1] Decision 11): its own damage rule, no terrain
                // operation and no explosion burst (there is no carve to present).
                if (aim.atBody && aim.body != scene::kNoEntity && simulation.Scene().TryGet<scene::Mechanism>(aim.body)) {
                    outcome.blasts.push_back(MechanismBlast(simulation, aim.body, aim.sphere, aim.power));
                    break;
                }
                outcome.blasts.push_back(Blast(simulation, aim.sphere, aim.power));
                outcome.effects.push_back(EffectCue{EffectKind::Blast, aim.sphere.center, aim.sphere.radius});
                CutRopesBySphere(simulation, aim.sphere, simulation.Tick(), &outcome.ropeCuts);
                break;
        }
        // Damage (fight.md "Damage"): every living enemy of the actor within the rule's reach, in span order.
        if (context.blastDamage.centre > 0 && request.player != 0 && request.slot == ActionSlot::Blast) {
            const PlayerView* actor = nullptr;
            for (const PlayerView& v : context.players)
                if (v.player == request.player) actor = &v;
            if (actor != nullptr) {
                const float voxelSize = simulation.World().VoxelSize();
                for (const PlayerView& v : context.players) {
                    if (!v.alive || v.player == request.player) continue;
                    if (actor->team != kNoTeam && v.team == actor->team) continue;
                    const int16_t amount = BlastDamageAt(context.blastDamage, aim.sphere.center, voxelSize, v.feet, context.tuning);
                    if (amount > 0) outcome.damage.push_back(DamageEvent{v.player, request.player, amount, static_cast<uint8_t>(DamageCause::Blast)});
                }
            }
        }
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
