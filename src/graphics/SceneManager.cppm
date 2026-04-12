module;

#include <vector>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h> 

export module vortex.graphics:scenemanager;

import :camera_struct;
import :light;
import vortex.memory;
import vortex.voxel; 

namespace vortex::graphics {

    export struct SceneMaterial {
        glm::vec4 color;
        float density; float friction; float restitution; float hardness;
        float health; float flammability; float heatRes; uint32_t flags;
        uint32_t textureIndex; uint32_t soundImpact; uint32_t soundDestroy; uint32_t _pad;
    };

    export struct SceneObject {
        glm::mat4 model{1.0f};
        glm::vec3 logicalCenter{0.0f};
        uint32_t voxelCount = 0;
        uint32_t chunkIndex = 0;
        uint32_t paletteOffset = 0;
        uint32_t flags = 0;
    };

    struct GPUObject {
        glm::mat4 model;
        glm::mat4 invModel;
        uint32_t chunkIndex;
        uint32_t paletteOffset;
        uint32_t flags;
        uint32_t _pad;
    };

    /**
     * @brief Node for the Top-Level Acceleration Structure (BVH).
     * @details Aligned to 32 bytes for GPU consumption.
     */
    struct GPUBVHNode {
        glm::vec3 aabbMin; 
        uint32_t leftChildOrInstance; // Leaf: Instance Index, Internal: Left Child Index
        glm::vec3 aabbMax; 
        uint32_t rightChildOrCount;   // Leaf: Sentinel (0xFFFFFFFF), Internal: Right Child Index
    };

    export class SceneManager {
    public:
        SceneManager() = default;
        ~SceneManager();

        void Initialize(memory::MemoryAllocator* allocator);
        void Shutdown();

        void UploadSceneData(const std::vector<SceneObject>& objects, 
                             const std::vector<vortex::voxel::PhysicalMaterial>& materials, 
                             const std::vector<vortex::voxel::Chunk>& chunks);
        
        void UploadCameraBuffer(const Camera& camera, uint32_t width, uint32_t height, uint64_t frameCount, bool useJitter);
        void UploadLightBuffer(const DirectionalLight& light);

        size_t CullAndUpload(const Camera& camera, float aspectRatio);

        std::vector<SceneObject>& GetObjects() { return m_Objects; }
        void SetObjectTransform(int index, const glm::mat4& transform);

        const memory::AllocatedBuffer& GetCameraBuffer() const { return m_CameraUBO; }
        const memory::AllocatedBuffer& GetMaterialBuffer() const { return m_MaterialsSSBO; }
        const memory::AllocatedBuffer& GetObjectBuffer() const { return m_ObjectsSSBO; }
        const memory::AllocatedBuffer& GetChunkBuffer() const { return m_ChunksSSBO; }
        const memory::AllocatedBuffer& GetLightBuffer() const { return m_LightUBO; }
        
        /** @brief Returns the TLAS BVH buffer. */
        const memory::AllocatedBuffer& GetTLASBuffer() const { return m_TLASBuffer; }

    private:
        memory::MemoryAllocator* m_Allocator{nullptr};
        
        std::vector<SceneObject> m_Objects;
        std::vector<SceneObject> m_CachedObjects;
        
        memory::AllocatedBuffer m_CameraUBO;
        memory::AllocatedBuffer m_LightUBO;
        memory::AllocatedBuffer m_MaterialsSSBO;
        memory::AllocatedBuffer m_ObjectsSSBO;
        memory::AllocatedBuffer m_ChunksSSBO;
        
        // New TLAS Buffer
        memory::AllocatedBuffer m_TLASBuffer; 

        void* m_MappedObjectBuffer{nullptr};
        void* m_MappedTLASBuffer{nullptr}; // Mapped pointer for TLAS
        
        glm::mat4 m_PrevViewProj{1.0f};
        std::vector<GPUObject> m_VisibleGPUObjects;

        // BVH Building Helpers
        struct BVHBuildNode {
            glm::vec3 min;
            glm::vec3 max;
            int left = -1;
            int right = -1;
            int objectIndex = -1; // -1 if internal
        };
        
        void BuildTLAS(const std::vector<SceneObject*>& visibleObjects);
        int BuildBVHRecursive(std::vector<int>& objIndices, std::vector<BVHBuildNode>& nodes, const std::vector<SceneObject*>& objects);
    };
}