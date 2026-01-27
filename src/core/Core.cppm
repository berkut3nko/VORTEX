module;

#include <memory>
#include <string>
#include <vector>
#include <functional> 

export module vortex.core;

export import vortex.graphics;
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
        
        void AddEntity(std::shared_ptr<voxel::VoxelEntity> entity);

        // Updated to use PhysicalMaterial to match GraphicsContext
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