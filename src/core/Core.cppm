module;

#include <memory>
#include <string>
#include <vector>
#include <functional> 

export module vortex.core;

export import vortex.graphics;
export import vortex.physics;
import vortex.voxel; 

export import :camera;    
export import :profiller;

namespace vortex {

    export class Engine {
    public:
        Engine();
        ~Engine();

        bool Initialize(const std::string& title, uint32_t width, uint32_t height);
        
        void Run(std::function<void()> onGuiRender = nullptr);
        
        void Shutdown();
        
        /**
         * @brief Adds an entity to the world and registers it with the physics engine.
         * @param entity The voxel entity to add.
         * @param isStatic If true, the object will be an immovable static collider (e.g., floor).
         */
        void AddEntity(std::shared_ptr<voxel::VoxelEntity> entity, bool isStatic = false);

        // Internal use / Advanced use
        void UploadScene(
            const std::vector<graphics::SceneObject>& objects, 
            const std::vector<voxel::PhysicalMaterial>& materials,
            const std::vector<voxel::Chunk>& chunks);
            
        graphics::GraphicsContext& GetGraphics();

    private:
        void UpdateSystems(float deltaTime);

        struct InternalState;
        std::unique_ptr<InternalState> m_State;
    };
}