module;

#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
struct GLFWwindow;

export module vortex.graphics;

export import :window;
export import :context;
export import :swapchain;
export import :ui;
export import :pipeline;
export import :taapipeline;
export import :shader;

// Export Voxel definitions so consumers of Graphics (like Core) see Chunk
export import vortex.voxel;

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

    struct GraphicsInternal;

    export class GraphicsContext {
    public:
        GraphicsContext();
        ~GraphicsContext();

        bool Initialize(const std::string& title, uint32_t width, uint32_t height);
        void Shutdown();
        
        bool BeginFrame();
        void EndFrame();

        void UploadCamera(); 
        
        void UploadScene(const std::vector<SceneObject>& objects, 
                         const std::vector<SceneMaterial>& materials,
                         const std::vector<vortex::voxel::Chunk>& chunks);
        
        Camera& GetCamera();
        GLFWwindow* GetWindow();

    private:
        std::unique_ptr<GraphicsInternal> m_Internal;
    };
}