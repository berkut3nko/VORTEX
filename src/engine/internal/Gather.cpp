module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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
            out.push_back(gathered);
        }
    }
}
