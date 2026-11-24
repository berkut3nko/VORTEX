module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <vector>

module vortex.engine;

// What the renderer draws of a scene. The scene itself is GPU-free (Vortex::Sim) and does not
// know these types; both functions only read its public registry.

namespace vortex {

    void GatherInstances(const scene::Scene& scene, std::vector<graphics::InstanceDesc>& out) {
        const entt::registry& registry = scene.Registry();
        for (const auto [entity, transform, voxels] : registry.view<const scene::Transform, const scene::VoxelBody>().each()) {
            if (!voxels.visible || !voxels.model) continue;
            const glm::mat4 current = transform.current.ToMatrix();
            const glm::mat4 previous = transform.previous.ToMatrix();
            for (const voxel::ModelPart& part : voxels.model->Parts()) {
                const glm::mat4 offset = glm::translate(glm::mat4(1.0f), glm::vec3(part.offset));
                graphics::InstanceDesc instance;
                instance.localToWorld = current * offset;
                instance.prevLocalToWorld = previous * offset;
                instance.slot = part.slot;
                instance.boundsMin = part.boundsMin;
                instance.boundsMax = part.boundsMax;
                instance.palette = voxels.model->Palette();
                out.push_back(instance);
            }
        }
    }

    namespace {
        /// One link instance from `a` to `b` (world metres), `thickness` metres wide.
        void AddLink(const voxel::VoxelModel& link, const glm::dvec3& a, const glm::dvec3& b, float thickness,
                     std::vector<graphics::InstanceDesc>& out) {
            if (link.Parts().empty()) return;
            const glm::vec3 d = glm::vec3(b - a);
            const float length = glm::length(d);
            if (!(length > 1.0e-4f)) return;
            const glm::vec3 z = d / length;
            const glm::vec3 helper = std::abs(z.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 x = glm::normalize(glm::cross(helper, z));
            const glm::vec3 y = glm::cross(z, x);
            const glm::ivec3 size = link.Volume().Size();
            glm::mat4 m(1.0f);
            m[0] = glm::vec4(x * (thickness / static_cast<float>(size.x)), 0.0f);
            m[1] = glm::vec4(y * (thickness / static_cast<float>(size.y)), 0.0f);
            m[2] = glm::vec4(z * (length / static_cast<float>(size.z)), 0.0f);
            m[3] = glm::vec4(glm::vec3(a), 1.0f);
            m = m * glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f * static_cast<float>(size.x), -0.5f * static_cast<float>(size.y), 0.0f));
            for (const voxel::ModelPart& part : link.Parts()) {
                graphics::InstanceDesc instance;
                instance.localToWorld = m * glm::translate(glm::mat4(1.0f), glm::vec3(part.offset));
                instance.prevLocalToWorld = instance.localToWorld;
                instance.slot = part.slot;
                instance.boundsMin = part.boundsMin;
                instance.boundsMax = part.boundsMax;
                instance.palette = link.Palette();
                out.push_back(instance);
            }
        }
    }

    void GatherRopes(const sim::Simulation& simulation, const voxel::VoxelModel& rope, const voxel::VoxelModel& chain,
                     std::vector<graphics::InstanceDesc>& out, uint32_t maxInstances) {
        const scene::Scene& scene = simulation.Scene();
        const float thickness = sim::kRopeRadius * 2.0f;
        uint32_t drawn = 0;
        for (const scene::Entity e : sim::RopesInOrder(scene)) {
            if (drawn >= maxInstances) break;
            const scene::Rope& r = scene.Registry().get<scene::Rope>(e);
            const bool isChain = (r.flags & scene::RopeFlag_Chain) != 0;
            const voxel::VoxelModel& link = isChain ? chain : rope;
            const glm::dvec3 anchor = sim::RopeAnchorPoint(simulation.World(), r.anchorVoxel);
            if (r.cut) {
                const double stub = static_cast<double>(r.cutFraction * r.length);
                if (stub > 0.05) AddLink(link, anchor, anchor - glm::dvec3(0.0, stub, 0.0), thickness, out);
            } else {
                glm::dvec3 a, b;
                if (sim::RopeEnds(simulation, e, a, b)) AddLink(link, a, b, isChain ? thickness * 1.5f : thickness, out);
            }
            ++drawn;
        }
    }

    void GatherLights(const scene::Scene& scene, std::vector<graphics::PointLight>& out) {
        const entt::registry& registry = scene.Registry();
        for (const auto [entity, transform, light] : registry.view<const scene::Transform, const scene::PointLight>().each()) {
            if (!(light.intensity > 0.0f)) continue;
            graphics::PointLight gathered;
            gathered.position = glm::dvec3(transform.current.position + transform.current.rotation * light.offset);
            gathered.color = light.color;
            gathered.intensity = light.intensity;
            gathered.radius = light.radius;
            gathered.sourceRadius = light.sourceRadius;
            gathered.effect = light.effect;
            if (light.effect) {
                gathered.clipMin = gathered.position + glm::dvec3(light.clipMin);
                gathered.clipMax = gathered.position + glm::dvec3(light.clipMax);
            }
            out.push_back(gathered);
        }
    }
}
