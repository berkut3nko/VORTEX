module;

#include <memory>
#include <string>
#include <vector>
#include <functional> // For std::function

export module vortex.core;

export import vortex.graphics;
export import :camera;    
export import :profiller;

namespace vortex {

    export class Engine {
    public:
        Engine();
        ~Engine();

        bool Initialize(const std::string& title, uint32_t width, uint32_t height);
        
        /**
         * @brief Starts the main engine loop.
         * @param onGuiRender Optional callback for rendering custom ImGui UI.
         */
        void Run(std::function<void()> onGuiRender = nullptr);
        
        void Shutdown();
        
        // Proxy methods to keep main.cpp clean
        void UploadScene(const std::vector<graphics::SceneObject>& objects, const std::vector<graphics::SceneMaterial>& materials);
        graphics::GraphicsContext& GetGraphics();

    private:
        void UpdateSystems(float deltaTime);

        struct InternalState;
        std::unique_ptr<InternalState> m_State;
    };
}