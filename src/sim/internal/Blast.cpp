module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;

namespace vortex::sim {

    voxel::EditReport BlastResult::Report() const {
        voxel::EditReport out;
        for (const AppliedOp& applied : ops) {
            out.skipped.insert(out.skipped.end(), applied.result.report.skipped.begin(), applied.result.report.skipped.end());
            out.unsynced.insert(out.unsynced.end(), applied.result.report.unsynced.begin(), applied.result.report.unsynced.end());
        }
        return out;
    }

    glm::vec3 BlastPush(const glm::vec3& center, float radius, const glm::vec3& position) {
        const glm::vec3 d = position - center;
        const float dist = glm::length(d);
        const float reach = radius * 3.0f;
        if (!(dist <= reach) || !(reach > 0.0f)) return glm::vec3(0.0f);
        const float falloff = 1.0f - dist / reach;
        return (dist > 1e-3f ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f)) * (6.0f * falloff);
    }

    BlastResult Blast(Simulation& simulation, const BlastDesc& desc) {
        const double vs = static_cast<double>(simulation.World().VoxelSize());
        return Blast(simulation, voxel::QuantizeSphere(desc.center / vs, static_cast<double>(desc.radius) / vs),
                     voxel::QuantizePower(desc.power));
    }

    BlastResult Blast(Simulation& simulation, const voxel::SphereQ& sphere, uint16_t power) {
        voxel::World& world = simulation.World();
        voxel::MaterialLibrary& materials = simulation.Materials();
        physics::PhysicsWorld& physics = simulation.Physics();
        scene::Scene& scene = simulation.Scene();
        const double vs = static_cast<double>(world.VoxelSize());

        BlastResult result;
        constexpr double kInvQ = 1.0 / static_cast<double>(voxel::kQuantaPerVoxel);
        result.center = glm::dvec3(sphere.center) * (kInvQ * vs);
        result.radius = static_cast<float>(static_cast<double>(sphere.radius) * kInvQ * vs);
        const glm::vec3 center(result.center);
        const float radius = result.radius;

        // 1. Carve.
        {
            AppliedOp carve;
            carve.op = WorldOp::Carve(sphere, power);
            carve.result = ApplyOp(world, carve.op, &carve.edits);
            result.ops.push_back(std::move(carve));
        }

        // 2. Terrain that lost its support. The box is derived from the integer sphere only.
        const glm::ivec3 c = math::FloorDiv(sphere.center, voxel::kQuantaPerVoxel);
        const glm::ivec3 margin(static_cast<int32_t>(sphere.radius / voxel::kQuantaPerVoxel) + kIslandSearchMargin);
        std::vector<voxel::FloatingIsland> islands = voxel::ExtractFloatingIslands(
            world, &materials, c - margin, c + margin, kMaxIslandVoxels, false, voxel::MaterialFlag_Liquid, &result.islandsSearched);
        for (voxel::FloatingIsland& island : islands) {
            AppliedOp detach;
            const glm::vec3 corner(world.VoxelToWorld(island.origin));
            detach.op = WorldOp::Detach(island.origin, voxel::VoxelMask::FromVolume(island.volume), BlastPush(center, radius, corner));
            detach.result = ApplyOp(world, detach.op, &detach.edits);
            result.ops.push_back(std::move(detach));
        }

        // 3. Objects caught in the blast. The list is a snapshot: pieces made here are not hit twice.
        if (radius > 0.0f) {
            std::vector<scene::Entity> caught;
            scene.QueryBodies(math::AABB(center - glm::vec3(radius), center + glm::vec3(radius)), caught);
            for (const scene::Entity entity : caught) {
                scene::DamageResult damage = scene.Damage(entity, result.center, radius);
                if (damage.removed == 0) continue;
                ++result.objectsHit;
                result.objectVoxels += damage.removed;
                for (scene::DamageFragment& fragment : damage.fragments) {
                    fragment.linearVelocity += BlastPush(center, radius, fragment.pose.position);
                    result.fragments.push_back(std::move(fragment));
                }
            }
        }

        // 4. Push bodies outwards (the new pieces included) and wake what rested nearby.
        for (const auto [entity, body] : scene.Registry().view<const scene::RigidBody>().each()) {
            if (!physics.IsValid(body.body)) continue;
            const glm::vec3 push = BlastPush(center, radius, physics.GetState(body.body).position);
            if (push == glm::vec3(0.0f)) continue;
            physics.AddImpulse(body.body, push * physics.GetMass(body.body));
        }
        physics.ActivateInBox(math::AABB(center - glm::vec3(radius * 2.0f), center + glm::vec3(radius * 2.0f)));
        return result;
    }
}
