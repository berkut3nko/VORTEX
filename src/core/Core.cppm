module;

#include <memory>
#include <string>
#include <vector>
#include <functional> 

export module vortex.core;

export import vortex.graphics;
export import vortex.physics;
import vortex.voxel; 

export import :camera;    
export import :profiller;

namespace vortex {

    /**
     * @brief The main engine class coordinating all subsystems.
     */
    export class Engine {
    public:
        Engine();
        ~Engine();

        /**
         * @brief Initializes the engine, window, and subsystems.
         * @param title Application title.
         * @param width Window width.
         * @param height Window height.
         * @return True on success.
         */
        bool Initialize(const std::string& title, uint32_t width, uint32_t height);
        
        /**
         * @brief Starts the main application loop.
         * @param onGuiRender Optional callback for rendering custom ImGui elements.
         */
        void Run(std::function<void()> onGuiRender = nullptr);
        
        /**
         * @brief Shuts down the engine.
         */
        void Shutdown();
        
        /**
         * @brief Adds an entity to the world and registers it with the physics engine.
         * @param entity The voxel entity to add.
         * @param isStatic If true, the object will be an immovable static collider (e.g., floor).
         */
        void AddEntity(std::shared_ptr<voxel::VoxelEntity> entity, bool isStatic = false);

        /**
         * @brief Advanced: Manually uploads a scene configuration to the GPU.
         * @param objects List of graphic objects.
         * @param materials List of materials.
         * @param chunks List of chunks.
         */
        void UploadScene(
            const std::vector<graphics::SceneObject>& objects, 
            const std::vector<voxel::PhysicalMaterial>& materials,
            const std::vector<voxel::Chunk>& chunks);
            
        graphics::GraphicsContext& GetGraphics();

    private:
        /**
         * @brief Updates physics, input, and game logic systems.
         * @param deltaTime Time elapsed since last frame in seconds.
         */
        void UpdateSystems(float deltaTime);

        struct InternalState;
        std::unique_ptr<InternalState> m_State;
    };
}