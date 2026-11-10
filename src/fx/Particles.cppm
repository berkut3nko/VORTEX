module;

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

export module vortex.fx:particles;

import vortex.core;

export namespace vortex::fx {

    /// @brief Behaviour flags of a particle.
    enum ParticleFlags : uint32_t {
        ParticleFlag_None         = 0,
        ParticleFlag_Collide      = 1u << 0,   ///< Bounces off solid voxels (see SolidQuery).
        ParticleFlag_DieOnCollide = 1u << 1,   ///< Removed on first contact (sparks hitting a wall, rain).
        ParticleFlag_Additive     = 1u << 2,   ///< Pure light: adds to the scene, never darkens it (fire, sparks, magic).
        ParticleFlag_SoftDisc     = 1u << 3,   ///< Rendered as a soft round sprite (smoke); default is a crisp square "voxel".
        ParticleFlag_Unlit        = 1u << 4,   ///< Ignores scene lighting.
    };

    /// @brief Everything needed to spawn one particle.
    struct ParticleDesc {
        glm::dvec3 position{0.0};             ///< World space, metres.
        glm::vec3 velocity{0.0f};             ///< Metres per second.
        glm::vec4 colorStart{1.0f};           ///< Linear RGB + opacity at birth.
        glm::vec4 colorEnd{1.0f, 1.0f, 1.0f, 0.0f};   ///< At death (linear interpolation over the lifetime).
        float sizeStart = 0.1f;               ///< Edge length in metres at birth.
        float sizeEnd = 0.1f;
        float lifetime = 1.0f;                ///< Seconds.
        float gravity = 1.0f;                 ///< Multiplier of world gravity (0 = floats, negative = rises).
        float drag = 0.0f;                    ///< Velocity damping per second (v *= exp(-drag * dt)).
        float bounce = 0.3f;                  ///< Restitution when colliding.
        float emissive = 0.0f;                ///< Emitted radiance multiplier (colour * emissive is added as light).
        uint32_t flags = ParticleFlag_None;
    };

    /// @brief Randomised recipe for particles; used for bursts and continuous emitters.
    struct EmitterDesc {
        ParticleDesc particle;                ///< Template; position / velocity are overwritten.
        glm::vec3 positionJitter{0.0f};       ///< Spawn box half extents around the emitter position.
        glm::vec3 direction{0.0f, 1.0f, 0.0f};///< Mean emission direction (normalised internally).
        float spread = 3.14159265f;           ///< Cone half angle in radians (pi = all directions).
        float speedMin = 1.0f;
        float speedMax = 2.0f;
        float lifetimeJitter = 0.25f;         ///< +- fraction of particle.lifetime.
        float sizeJitter = 0.25f;             ///< +- fraction of the sizes.
        float colorJitter = 0.08f;            ///< +- brightness variation.
        float rate = 0.0f;                    ///< Particles per second for continuous emitters.
    };

    /// @brief GPU particle (std430, 48 bytes). Mirrors `VxParticle` in the shaders.
    struct GpuParticle {
        glm::vec4 positionSize{0.0f};   ///< xyz = position relative to the render origin (m), w = half size (m).
        glm::vec4 color{1.0f};          ///< Linear rgb, a = opacity (0 for additive particles).
        glm::vec4 params{0.0f};         ///< x = emissive multiplier, y = shape (0 square, 1 soft disc), z = unlit (0/1), w = unused.
    };
    static_assert(sizeof(GpuParticle) == 48);

    struct EmitterTag {};
    using EmitterId = Handle<EmitterTag>;

    /// @brief Returns true if the given world position (metres) is inside solid matter.
    using SolidQuery = std::function<bool(const glm::dvec3&)>;

    /**
     * @brief CPU particle simulation: a fixed-capacity pool, simulated every frame.
     * @details Design goals: no per-particle allocation, O(alive) update, deterministic for a
     * given seed, collision against the voxel world through a callback (so the module has no
     * dependency on the world). Rendering consumes BuildRenderData().
     * When the pool is full new particles are dropped (never the oldest: no popping).
     */
    class ParticleSystem {
    public:
        explicit ParticleSystem(uint32_t capacity, uint64_t seed = 1);
        ~ParticleSystem();
        ParticleSystem(const ParticleSystem&) = delete;
        ParticleSystem& operator=(const ParticleSystem&) = delete;

        /// @brief Spawns one particle. Returns false if the pool is full.
        bool Emit(const ParticleDesc& desc);
        /// @brief Spawns `count` randomised particles at once. Returns the number actually spawned.
        uint32_t Burst(const EmitterDesc& desc, const glm::dvec3& position, uint32_t count);

        /// @brief Continuous emitter producing `desc.rate` particles per second until removed.
        EmitterId AddEmitter(const EmitterDesc& desc, const glm::dvec3& position);
        void SetEmitterPosition(EmitterId id, const glm::dvec3& position);
        void SetEmitterEnabled(EmitterId id, bool enabled);
        void RemoveEmitter(EmitterId id);
        bool IsEmitterValid(EmitterId id) const;

        /**
         * @brief Advances the simulation.
         * @param solid Optional collision query for particles flagged ParticleFlag_Collide.
         * @param gravity World gravity in m/s^2 (negative = down).
         */
        void Update(float dt, const SolidQuery& solid = {}, float gravity = -9.81f);

        uint32_t AliveCount() const;
        uint32_t Capacity() const;
        void Clear();

        /**
         * @brief Produces the instance data for rendering.
         * @param renderOrigin World position that render space is relative to.
         * @param cameraPosition World position of the camera; particles are sorted back to front
         * so alpha blending composes correctly.
         */
        void BuildRenderData(const glm::dvec3& renderOrigin, const glm::dvec3& cameraPosition,
                             std::vector<GpuParticle>& out) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
