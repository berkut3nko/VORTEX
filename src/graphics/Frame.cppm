module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <span>

export module vortex.graphics:frame;

import vortex.voxel;
import vortex.fx;

export namespace vortex::graphics {

    /**
     * @brief Per-frame uniform block. Mirrors `FrameUniforms` in shaders/include/common.glsl
     * member for member (std140). It only contains mat4 / vec4 / ivec4 so that the C++ layout
     * and the std140 layout are identical; a unit test compares the offsets with the SPIR-V.
     * See common.glsl for the meaning of every field.
     */
    struct FrameUniforms {
        glm::mat4 viewProj{1.0f};
        glm::mat4 viewProjNoJitter{1.0f};
        glm::mat4 prevViewProjNoJitter{1.0f};
        glm::vec4 cameraPos{0.0f};
        glm::vec4 cameraRight{0.0f};
        glm::vec4 cameraUp{0.0f};
        glm::vec4 cameraForward{0.0f};
        glm::vec4 cameraPosVoxel{0.0f};
        glm::vec4 renderSize{0.0f};
        glm::vec4 outputSize{0.0f};
        glm::vec4 jitter{0.0f};
        glm::vec4 sunDirection{0.0f};
        glm::vec4 sunRadiance{0.0f};
        glm::vec4 skyZenith{0.0f};
        glm::vec4 skyHorizon{0.0f};
        glm::vec4 skyGround{0.0f};
        glm::vec4 fogColor{0.0f};
        glm::vec4 fogParams{0.0f};
        glm::vec4 ambientParams{0.0f};
        glm::vec4 temporalParams{0.0f};
        glm::ivec4 gridSize{0};
        glm::ivec4 gridMinChunk{0};
        glm::ivec4 counts{0};
        glm::ivec4 settings{0};
    };
    static_assert(sizeof(FrameUniforms) == 3 * 64 + 21 * 16);

    /// @brief Feature bits of FrameUniforms::settings.x (VX_FEATURE_* in common.glsl).
    enum FeatureFlags : int {
        Feature_SunShadows       = 1,
        Feature_AmbientOcclusion = 2,
        Feature_EmissiveLight    = 4,
        Feature_LightShadows     = 8,
        Feature_Taa              = 16,
        Feature_Fog              = 32,
        Feature_SpatialFilter    = 64,
    };

    /// @brief FrameUniforms::settings.y (VX_DEBUG_* in common.glsl).
    enum class DebugView : int { None = 0, Albedo, Normal, Depth, Lighting, Steps, Motion, Material };

    /// @brief Point light as stored on the GPU (std430, 32 bytes). Mirrors `VxLight`.
    struct GpuLight {
        glm::vec4 positionRadius{0.0f};
        glm::vec4 colorSize{0.0f};
    };
    static_assert(sizeof(GpuLight) == 32);

    // ---------------------------------------------------------------------------------------------
    //  Scene description handed to the renderer every frame (world space, metres)
    // ---------------------------------------------------------------------------------------------

    /// @brief A camera. Right-handed, Y up, looking along its forward axis.
    struct Camera {
        glm::dvec3 position{0.0, 2.0, 5.0};
        float yaw = 0.0f;             ///< Radians around +Y; 0 looks down -Z.
        float pitch = 0.0f;           ///< Radians, positive looks up; clamp to about +-1.55.
        float fovY = 1.0472f;         ///< Vertical field of view in radians (60 degrees).
        float nearPlane = 0.05f;

        glm::vec3 Forward() const {
            return {-glm::sin(yaw) * glm::cos(pitch), glm::sin(pitch), -glm::cos(yaw) * glm::cos(pitch)};
        }
        glm::vec3 Right() const { return {glm::cos(yaw), 0.0f, -glm::sin(yaw)}; }
        glm::vec3 Up() const { return glm::cross(Right(), Forward()); }
    };

    struct SunLight {
        glm::vec3 direction{0.4f, 0.8f, 0.3f};   ///< Towards the sun (normalised by the renderer).
        glm::vec3 color{1.0f, 0.96f, 0.88f};
        float intensity = 3.0f;
        float angularRadius = 0.02f;             ///< Radians; larger = softer shadows.
    };

    struct SkySettings {
        glm::vec3 zenith{0.18f, 0.36f, 0.72f};
        glm::vec3 horizon{0.62f, 0.74f, 0.86f};
        glm::vec3 ground{0.20f, 0.19f, 0.18f};
    };

    struct FogSettings {
        glm::vec3 color{0.62f, 0.74f, 0.86f};
        float density = 0.006f;                  ///< Per metre.
        float heightFalloff = 0.03f;             ///< Per metre above baseHeight.
        float baseHeight = 0.0f;                 ///< World Y where the fog is densest.
        float startDistance = 8.0f;
        float maxOpacity = 1.0f;
    };

    struct PointLight {
        glm::dvec3 position{0.0};
        glm::vec3 color{1.0f, 0.8f, 0.6f};
        float intensity = 5.0f;
        float radius = 8.0f;                     ///< Range of influence in metres.
        float sourceRadius = 0.1f;               ///< Size of the emitter (soft shadows).
    };

    /// @brief One chunk of a dynamic voxel object to draw this frame.
    struct InstanceDesc {
        glm::mat4 localToWorld{1.0f};            ///< Chunk-local voxel units -> world metres.
        glm::mat4 prevLocalToWorld{1.0f};        ///< The same transform as rendered last frame.
        uint32_t slot = 0;                       ///< Chunk slot in the ChunkStore.
        glm::ivec3 boundsMin{0};                 ///< Tight voxel bounds inside the chunk, [min, max).
        glm::ivec3 boundsMax{32};
        voxel::PaletteId palette = 0;
    };

    /// @brief Everything the renderer needs to draw one frame. All pointers must stay valid during RenderFrame().
    struct FrameInput {
        Camera camera;
        SunLight sun;
        SkySettings sky;
        FogSettings fog;

        const voxel::World* world = nullptr;                 ///< May be null (objects only).
        voxel::ChunkStore* store = nullptr;                  ///< Required: source of chunk data; dirty slots are consumed.
        const voxel::MaterialLibrary* materials = nullptr;   ///< Required.

        std::span<const InstanceDesc> instances;
        std::span<const PointLight> lights;
        /// Particles relative to the render origin (see ParticleSystem::BuildRenderData and Renderer::RenderOrigin).
        std::span<const fx::GpuParticle> particles;

        double time = 0.0;          ///< Seconds since start.
        float deltaTime = 0.016f;
    };
}
