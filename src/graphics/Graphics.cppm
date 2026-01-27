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
<<<<<<< Updated upstream
export import :scene_manager;
=======
export import :scenemanager;
>>>>>>> Stashed changes
export import :camera_struct;

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

<<<<<<< Updated upstream
        // 1. Swapchain Acquisition
        bool BeginFrame();
        
        // 2. Command Buffer Recording Phases
        void BeginRecording();   // Reset & Begin CB
        void RecordScene();      // Geometry Pass
        void RecordAA();         // Anti-Aliasing Pass

        // 3. Submission & Presentation
        void EndFrame();         // UI, Blit, End CB, Submit, Present
        
        void UploadCamera(); 
=======
        bool BeginFrame();
        
        void BeginRecording();   
        void RecordScene();      
        void RecordAA();         

        void EndFrame();         
        
        void UploadCamera(); 
        
        // CHANGED: SceneMaterial -> PhysicalMaterial
>>>>>>> Stashed changes
        void UploadScene(const std::vector<SceneObject>& objects, 
                         const std::vector<vortex::voxel::PhysicalMaterial>& materials,
                         const std::vector<vortex::voxel::Chunk>& chunks);
        
        SceneManager& GetSceneManager();
        Camera& GetCamera();
        GLFWwindow* GetWindow();
        void SetAAMode(AntiAliasingMode mode);
        AntiAliasingMode GetAAMode() const;

<<<<<<< Updated upstream
        /**
         * @brief Gets the GPU execution time for the Geometry pass in ms.
         */
        float GetSceneGPUTime() const;

        /**
         * @brief Gets the GPU execution time for the AA pass in ms.
         */
=======
        float GetSceneGPUTime() const;
>>>>>>> Stashed changes
        float GetAAGPUTime() const;

    private:
        std::unique_ptr<GraphicsInternal> m_Internal;
    };
}