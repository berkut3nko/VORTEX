module;

#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
struct GLFWwindow;

export module vortex.graphics;

// Import partitions
export import :window;
export import :context;
export import :swapchain;
export import :ui;
export import :pipeline;
export import :taapipeline;
export import :fxaapipeline;
export import :shader;
export import :render_resources;
export import :scene_manager;
export import :camera_struct; // Export the Camera struct definition

export import vortex.voxel;

namespace vortex::graphics {

    export enum class AntiAliasingMode { None, FXAA, TAA };

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
        
        /**
         * @brief Access to scene manager for editor.
         */
        SceneManager& GetSceneManager();

        Camera& GetCamera();
        GLFWwindow* GetWindow();
        void SetAAMode(AntiAliasingMode mode);
        AntiAliasingMode GetAAMode() const;

    private:
        std::unique_ptr<GraphicsInternal> m_Internal;
    };
}