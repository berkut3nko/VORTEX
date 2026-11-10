module;

#include <glm/glm.hpp>

#include <cstdint>

export module vortex.fx:effects;

import :particles;

export namespace vortex::fx {

    /**
     * @brief Ready-made particle recipes. Each returns an EmitterDesc that can be tweaked,
     * used with ParticleSystem::Burst, or attached as a continuous emitter.
     * Sizes are in metres and assume voxels of roughly 0.1 m.
     */
    namespace effects {
        /// @brief Small tumbling cubes in the material's colour: what flies off when voxels break.
        EmitterDesc Debris(const glm::vec3& color, float voxelSize = 0.1f);
        /// @brief Slow soft puff in the material's colour.
        EmitterDesc Dust(const glm::vec3& color);
        /// @brief Dark rising smoke.
        EmitterDesc Smoke();
        /// @brief Fast bright fireball particles (additive).
        EmitterDesc FireBurst();
        /// @brief Sparks: tiny, fast, bouncing, additive.
        EmitterDesc Sparks(const glm::vec3& color = glm::vec3(1.0f, 0.75f, 0.35f));
        /// @brief Continuous flame for torches / campfires (set `rate`).
        EmitterDesc Flame();
        /// @brief Glowing motes for spells and pickups.
        EmitterDesc Magic(const glm::vec3& color);
        /// @brief Falling rain or snow around a point (continuous).
        EmitterDesc Precipitation(bool snow);
    }

    /// @brief Complete explosion at `position`: fireball, sparks, smoke. `radius` in metres.
    void SpawnExplosion(ParticleSystem& particles, const glm::dvec3& position, float radius);

    /// @brief Debris + dust for a destroyed voxel of the given linear colour.
    void SpawnVoxelBreak(ParticleSystem& particles, const glm::dvec3& voxelCenter, const glm::vec3& color,
                         float voxelSize, const glm::vec3& impulse = glm::vec3(0.0f));
}
