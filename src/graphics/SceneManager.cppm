module;

#include <vulkan/vulkan.h>
#include <vector>
#include <glm/glm.hpp>

export module vortex.graphics:scene_manager;

import vortex.memory;
import vortex.voxel;

namespace vortex::graphics {
    /**
     * @brief High-level CPU representation of an object.
     * @details Contains transform and metadata used by the engine and editor.
     */
    export struct SceneObject { 
        glm::mat4 model; 
        uint32_t materialIdx;
        
        // Editor/Logic fields (NOT sent to GPU directly)
        glm::vec3 logicalCenter{16.0f}; // Default to center of 32^3 chunk
        uint32_t voxelCount{0};
    };
    
    export struct SceneMaterial { glm::vec4 color; };
    
    // Internal GPU structure (Matches Shader std430 layout)
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

        /**
         * @brief Updates a specific object's transform directly in the cached list.
         * @note Requires next CullAndUpload call to sync with GPU.
         * @param index Index of the object in the scene list.
         * @param newModel New model matrix.
         */
        void SetObjectTransform(uint32_t index, const glm::mat4& newModel);

        /**
         * @brief Accessor for cached scene objects (Editable).
         */
        std::vector<SceneObject>& GetObjects() { return m_CachedObjects; }

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

        // OPTIMIZATION: Persistent mapped pointer to avoid per-frame vmaMapMemory calls
        void* m_MappedObjectBuffer = nullptr;

        std::vector<SceneObject> m_CachedObjects;
        std::vector<GPUObject> m_VisibleGPUObjects;
        
        glm::mat4 m_PrevViewProj{1.0f};
    };
}