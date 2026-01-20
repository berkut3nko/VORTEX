module;

#include <cstdint>
#include <memory>
#include <string>

export module vortex.graphics;

// Export the shader partition so Engine.cpp can see ShaderCompiler
export import :shader; 

namespace vortex::graphics {

    /**
     * @brief Manages the window, Vulkan instance, and ImGui context.
     */
    export class GraphicsContext {
    public:
        GraphicsContext();
        ~GraphicsContext();

        bool Initialize(const std::string& title, uint32_t width, uint32_t height);
        void Shutdown();
        bool BeginFrame();
        void EndFrame();

    private:
        struct InternalState;
        std::unique_ptr<InternalState> m_State;
    };

}