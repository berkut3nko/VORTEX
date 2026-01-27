module;

#include <vector>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h> 

export module vortex.graphics:scenemanager;

import :camera_struct;
import vortex.memory;
import vortex.voxel; 

namespace vortex::graphics {

    /**
     * @brief Represents a material in the scene.
     * @details Must match 'struct Material' in voxel.frag std430 layout. Total size: 64 bytes.
     */
    export struct SceneMaterial {
        // --- 16 bytes ---
        glm::vec4 color;
        
        // --- 16 bytes ---
        float density; 
        float friction; 
        float restitution; 
        float hardness;
        
        // --- 16 bytes ---
        float health; 
        float flammability; 
        float heatRes; 
        uint32_t flags;
        
        // --- 16 bytes ---
        uint32_t textureIndex; 
        uint32_t soundImpact; 
        uint32_t soundDestroy; 
        uint32_t _pad;
    };

    /**
     * @brief CPU-side representation of an object in the scene.
     */
    export struct SceneObject {
        glm::mat4 model{1.0f};
        glm::vec3 logicalCenter{0.0f};
        uint32_t voxelCount = 0;
        uint32_t chunkIndex = 0;
        uint32_t paletteOffset = 0;
        uint32_t flags = 0;
    };

    // Internal GPU structure must match shader
    struct GPUObject {
        glm::mat4 model;
        glm::mat4 invModel;
        uint32_t chunkIndex;
        uint32_t paletteOffset;
        uint32_t flags;
        uint32_t _pad;
    };

    /**
     * @brief Manages scene data buffers (Objects, Materials, Chunks, Camera).
     */
    export class SceneManager {
    public:
        SceneManager() = default;
        ~SceneManager();

        /**
         * @brief Initializes storage buffers.
         * @param allocator Memory allocator for buffer creation.
         */
        void Initialize(memory::MemoryAllocator* allocator);
        
        void Shutdown();

        /**
         * @brief Uploads all scene data to the GPU.
         * @param objects List of objects.
         * @param materials List of materials (using binary compatible PhysicalMaterial).
         * @param chunks List of chunks.
         */
        void UploadSceneData(const std::vector<SceneObject>& objects, 
                             const std::vector<vortex::voxel::PhysicalMaterial>& materials, 
                             const std::vector<vortex::voxel::Chunk>& chunks);
        
        /**
         * @brief Updates the camera uniform buffer.
         * @param camera The camera state.
         * @param width Viewport width.
         * @param height Viewport height.
         * @param frameCount Current frame counter (for jitter).
         * @param useJitter Whether to apply TAA jitter.
         */
        void UploadCameraBuffer(const Camera& camera, uint32_t width, uint32_t height, uint64_t frameCount, bool useJitter);
        
        /**
         * @brief Culls objects against the frustum and updates the visible object buffer.
         * @param camera The camera to cull against.
         * @param aspectRatio Aspect ratio of the viewport.
         * @return Number of visible objects.
         */
        size_t CullAndUpload(const Camera& camera, float aspectRatio);

        std::vector<SceneObject>& GetObjects() { return m_Objects; }
        
        /**
         * @brief Updates the transform of a specific object in the local cache.
         */
        void SetObjectTransform(int index, const glm::mat4& transform);

        const memory::AllocatedBuffer& GetCameraBuffer() const { return m_CameraUBO; }
        const memory::AllocatedBuffer& GetMaterialBuffer() const { return m_MaterialsSSBO; }
        const memory::AllocatedBuffer& GetObjectBuffer() const { return m_ObjectsSSBO; }
        const memory::AllocatedBuffer& GetChunkBuffer() const { return m_ChunksSSBO; }

    private:
        memory::MemoryAllocator* m_Allocator{nullptr};
        
        std::vector<SceneObject> m_Objects;
        std::vector<SceneObject> m_CachedObjects;
        
        memory::AllocatedBuffer m_CameraUBO;
        memory::AllocatedBuffer m_MaterialsSSBO;
        memory::AllocatedBuffer m_ObjectsSSBO;
        memory::AllocatedBuffer m_ChunksSSBO;

        void* m_MappedObjectBuffer{nullptr};
        glm::mat4 m_PrevViewProj{1.0f};
        std::vector<GPUObject> m_VisibleGPUObjects;
    };
}