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
export import :fxaapipeline;
export import :shader;
export import :render_resources;
export import :scenemanager;
export import :camera_struct;
export import :light; // Import light

export import vortex.voxel;

namespace vortex::graphics {

    export enum class AntiAliasingMode { None, FXAA, TAA };

    struct GraphicsInternal;

    /**
     * @brief The main entry point for the Graphics subsystem.
     */
    export class GraphicsContext {
    public:
        GraphicsContext();
        ~GraphicsContext();

        bool Initialize(const std::string& title, uint32_t width, uint32_t height);
        void Shutdown();

        bool BeginFrame();
        void BeginRecording();   
        void RecordScene();      
        void RecordAA();         
        void EndFrame();         
        
        void UploadCamera(); 
        
        /**
         * @brief Uploads global light data to GPU.
         */
        void UploadLight(const DirectionalLight& light);

        void UploadScene(const std::vector<SceneObject>& objects, 
                         const std::vector<vortex::voxel::PhysicalMaterial>& materials,
                         const std::vector<vortex::voxel::Chunk>& chunks);
        
        SceneManager& GetSceneManager();
        Camera& GetCamera();
        GLFWwindow* GetWindow();
        
        void SetAAMode(AntiAliasingMode mode);
        AntiAliasingMode GetAAMode() const;

        float GetSceneGPUTime() const;
        float GetAAGPUTime() const;

    private:
        std::unique_ptr<GraphicsInternal> m_Internal;
    };
}