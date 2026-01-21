module;

#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
// Forward decl for GLFW
struct GLFWwindow;

export module vortex.graphics;

// Export sub-modules so users can see types if needed
export import :window;
export import :context;
export import :swapchain;
export import :ui;
export import :pipeline;
export import :shader;

namespace vortex::graphics {

    export struct Camera {
        glm::vec3 position{0.0f, 0.0f, 5.0f};
        glm::vec3 front{0.0f, 0.0f, -1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        float yaw = -90.0f;
        float pitch = 0.0f;
        float fov = 60.0f;
    };

    export struct SceneObject {
        glm::mat4 model;
        uint32_t materialIdx;
    };

    export struct SceneMaterial {
        glm::vec4 color;
    };

    // Forward declaration to hide implementation
    struct GraphicsInternal;

    /**
     * @brief Main facade for the graphics engine.
     */
    export class GraphicsContext {
    public:
        GraphicsContext();
        ~GraphicsContext();

        bool Initialize(const std::string& title, uint32_t width, uint32_t height);
        void Shutdown();
        
        bool BeginFrame();
        void EndFrame();

        // Renamed to clarify intent: this only uploads data to GPU, doesn't handle input
        void UploadCamera(); 
        
        void UploadScene(const std::vector<SceneObject>& objects, const std::vector<SceneMaterial>& materials);
        
        Camera& GetCamera();
        
        // New: Access to window for Input Controller
        GLFWwindow* GetWindow();

    private:
        std::unique_ptr<GraphicsInternal> m_Internal;
    };
}