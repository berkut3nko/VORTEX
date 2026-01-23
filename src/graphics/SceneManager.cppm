module;

#include <vulkan/vulkan.h>
#include <vector>
#include <glm/glm.hpp>

export module vortex.graphics:scene_manager;

import vortex.memory;
import vortex.voxel;

namespace vortex::graphics {
    // Forward declarations or re-definitions if not exported from a pure type module
    export struct SceneObject { glm::mat4 model; uint32_t materialIdx; };
    export struct SceneMaterial { glm::vec4 color; };
    
    struct GPUObject {
        glm::mat4 model;
        glm::mat4 invModel;
        uint32_t chunkIndex;
        uint32_t paletteOffset;
        uint32_t flags;
        uint32_t pad;
    };

    struct CameraUBO {
        glm::mat4 viewInverse;
        glm::mat4 projInverse;
        glm::vec4 position;
        glm::vec4 direction;
        uint32_t objectCount;
        float _pad[3]; 
        glm::mat4 viewProj; 
        glm::mat4 prevViewProj; 
        glm::vec4 jitter;       
    };
    
    struct Camera; // Forward declaration

    /**
     * @brief Handles scene data, culling, and GPU buffer updates.
     */
    export class SceneManager {
    public:
        SceneManager() = default;
        ~SceneManager();

        void Initialize(memory::MemoryAllocator* allocator);
        void Shutdown();

        /**
         * @brief Updates camera UBO with new view/proj matrices and TAA jitter.
         */
        void UploadCameraBuffer(const Camera& camera, uint32_t width, uint32_t height, uint64_t frameCount, bool useJitter);

        /**
         * @brief Uploads raw scene data (CPU side cache).
         */
        void UploadSceneData(const std::vector<SceneObject>& objects, 
                             const std::vector<SceneMaterial>& materials,
                             const std::vector<vortex::voxel::Chunk>& chunks);

        /**
         * @brief Performs frustum culling and updates the GPU Object Buffer.
         * @return Number of visible objects.
         */
        size_t CullAndUpload(const Camera& camera, float aspectRatio);

        // Getters for Buffers
        const memory::AllocatedBuffer& GetCameraBuffer() const { return m_CameraUBO; }
        const memory::AllocatedBuffer& GetMaterialBuffer() const { return m_MaterialsSSBO; }
        const memory::AllocatedBuffer& GetObjectBuffer() const { return m_ObjectsSSBO; }
        const memory::AllocatedBuffer& GetChunkBuffer() const { return m_ChunksSSBO; }

    private:
        memory::MemoryAllocator* m_Allocator{nullptr};

        memory::AllocatedBuffer m_CameraUBO;
        memory::AllocatedBuffer m_MaterialsSSBO;
        memory::AllocatedBuffer m_ObjectsSSBO;
        memory::AllocatedBuffer m_ChunksSSBO;

        std::vector<SceneObject> m_CachedObjects;
        std::vector<GPUObject> m_VisibleGPUObjects;
        
        glm::mat4 m_PrevViewProj{1.0f};
    };
}