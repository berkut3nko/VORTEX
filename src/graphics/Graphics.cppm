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

export import vortex.voxel;

namespace vortex::graphics {

    export enum class AntiAliasingMode { None, FXAA, TAA };

    struct GraphicsInternal;

    /**
     * @brief The main entry point for the Graphics subsystem.
     * @details Orchestrates the Window, Context, Swapchain, Pipelines, and Rendering Loop.
     */
    export class GraphicsContext {
    public:
        GraphicsContext();
        ~GraphicsContext();

        /**
         * @brief Initializes the entire graphics stack (Window, Vulkan, Pipelines).
         * @param title Window title.
         * @param width Window width.
         * @param height Window height.
         * @return True if initialization was successful.
         */
        bool Initialize(const std::string& title, uint32_t width, uint32_t height);
        
        /**
         * @brief Shuts down all graphics subsystems and releases resources.
         */
        void Shutdown();

        /**
         * @brief Starts a new frame.
         * @details Polls window events, handles resizing, and acquires the next swapchain image.
         * @return False if the window should close, true otherwise.
         */
        bool BeginFrame();
        
        /**
         * @brief Begins command buffer recording for the current frame.
         */
        void BeginRecording();   

        /**
         * @brief Records the main geometry pass.
         */
        void RecordScene();      

        /**
         * @brief Records the post-processing AA pass.
         */
        void RecordAA();         

        /**
         * @brief Ends recording, submits command buffers, and presents the image.
         */
        void EndFrame();         
        
        /**
         * @brief Uploads camera data to the GPU.
         */
        void UploadCamera(); 
        
        /**
         * @brief Uploads scene data (objects, materials, chunks) to GPU buffers.
         * @param objects List of scene objects.
         * @param materials List of physical materials.
         * @param chunks List of voxel chunks.
         */
        void UploadScene(const std::vector<SceneObject>& objects, 
                         const std::vector<vortex::voxel::PhysicalMaterial>& materials,
                         const std::vector<vortex::voxel::Chunk>& chunks);
        
        SceneManager& GetSceneManager();
        Camera& GetCamera();
        GLFWwindow* GetWindow();
        
        void SetAAMode(AntiAliasingMode mode);
        AntiAliasingMode GetAAMode() const;

        /**
         * @brief Gets the GPU execution time for the scene geometry pass in milliseconds.
         */
        float GetSceneGPUTime() const;

        /**
         * @brief Gets the GPU execution time for the Anti-Aliasing pass in milliseconds.
         */
        float GetAAGPUTime() const;

    private:
        std::unique_ptr<GraphicsInternal> m_Internal;
    };
}