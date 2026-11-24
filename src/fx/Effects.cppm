module;

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

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

    /// @brief Debris + dust for a destroyed voxel of the given linear colour. `dust` false leaves out
    /// the dust ([wp-8] card 5).
    void SpawnVoxelBreak(ParticleSystem& particles, const glm::dvec3& voxelCenter, const glm::vec3& color,
                         float voxelSize, const glm::vec3& impulse = glm::vec3(0.0f), bool dust = true);

    /// @brief An unshadowed effect light (blast flash) as the merge sees it. World metres.
    struct EffectLightIn {
        glm::dvec3 position{0.0};
        glm::vec3 color{1.0f};
        float intensity = 0.0f;
        float radius = 0.0f;
        glm::dvec3 clipMin{0.0}, clipMax{0.0};   ///< world box the light reaches (from the axis rays)
    };

    /// @brief At most this many effect lights reach the GPU in one frame ([wp-8]).
    inline constexpr uint32_t kMaxEffectLights = 8;

    /// @brief Merges `lights` into at most kMaxEffectLights (fx-budget.md "Effect lights"; [wp-8]).
    /// @details Inputs in score order (intensity * radius^2 / max(camera distance, 1 m)^2, ties by
    /// input index). An input within half the larger radius of a slot merges into that slot; else it
    /// takes a new slot while fewer than kMaxEffectLights exist, else it merges into the nearest slot.
    /// A merge weights position and colour by intensity, adds the intensities, keeps the larger
    /// radius and takes the union of the clip boxes. Deterministic; nothing is dropped.
    void MergeEffectLights(std::span<const EffectLightIn> lights, const glm::dvec3& camera,
                           std::vector<EffectLightIn>& out);
}
