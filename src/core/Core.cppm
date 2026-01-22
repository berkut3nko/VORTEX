module;

#include <memory>
#include <string>
#include <vector>
#include <functional> 

export module vortex.core;

export import vortex.graphics;
// Explicitly import voxel module to ensure Chunk is visible in UploadScene signature
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
        
        // Proxy methods
        void UploadScene(
            const std::vector<graphics::SceneObject>& objects, 
            const std::vector<graphics::SceneMaterial>& materials,
            const std::vector<voxel::Chunk>& chunks);
            
        graphics::GraphicsContext& GetGraphics();

    private:
        void UpdateSystems(float deltaTime);

        struct InternalState;
        std::unique_ptr<InternalState> m_State;
    };
}