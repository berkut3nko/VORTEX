module;

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

export module vortex.graphics:renderer;

import vortex.voxel;
import :device;
import :window;
import :ui;
import :frame;
import :swapchain;

export namespace vortex::graphics {

    struct RenderSettings {
        float renderScale = 0.6f;            ///< Internal resolution as a fraction of the output (0.25 .. 1).
        /// Windowed only: FIFO_RELAXED (FIFO where the surface lacks it) by default; `Immediate` (IMMEDIATE, else
        /// MAILBOX, else FIFO) is `--no-vsync`, for measuring (perf.md P1). A change recreates the swapchain.
        PresentPreference present = PresentPreference::FifoRelaxed;
        bool taa = true;
        bool sunShadows = true;
        bool ambientOcclusion = true;
        bool emissiveLighting = true;        ///< Emissive voxels light their surroundings.
        bool lightShadows = true;            ///< Point light shadows.
        bool fog = true;
        /// Fade surfaces to the sky colour over the last 15 % of the world's horizontal view
        /// radius (voxel::World::ViewRadius), so the edge of the loaded window is never a cut.
        bool edgeFog = true;
        bool playBounds = true;              ///< Draw the session's play bounds (the curtain, composite pass; [wp-5]).
        bool spatialFilter = true;
        bool checkerboardLighting = true;    ///< Trace lighting rays for half of the pixels each frame.
        float ambientDistance = 6.0f;        ///< Length of ambient occlusion rays in metres.
        float ambientIntensity = 1.0f;
        float emissiveScale = 1.0f;
        float exposure = 1.0f;
        float lightingHistory = 24.0f;       ///< Frames of temporal accumulation for the lighting buffer.
        DebugView debugView = DebugView::None;
        uint32_t maxChunkUploadsPerFrame = 128;
    };

    /// @brief GPU time of one render pass, in milliseconds (0 until the first results arrive).
    struct PassTiming {
        std::string name;
        float milliseconds = 0.0f;
    };

    struct RenderStats {
        glm::uvec2 renderSize{0};
        glm::uvec2 outputSize{0};
        uint32_t instancesTotal = 0;
        uint32_t instancesDrawn = 0;         ///< After frustum culling.
        uint32_t chunkUploads = 0;           ///< Chunks copied to the GPU this frame.
        uint32_t chunkUploadsPending = 0;
        uint32_t gridBytesUploaded = 0;      ///< Grid words copied to the GPU this frame (dirty 4 KiB pages).
        uint32_t lights = 0;
        uint32_t particles = 0;
        uint32_t hudQuads = 0;               ///< HUD quads drawn this frame.
        uint64_t gpuMemoryBytes = 0;         ///< Buffers + images owned by the renderer.
        uint64_t frameIndex = 0;
        float fenceWaitMs = 0.0f;            ///< CPU wait for this frame slot's fence (0 for a skipped frame).
        uint64_t timingSerial = 0;           ///< +1 each time CollectTimings read new pass times
        std::vector<PassTiming> passes;
    };

    /**
     * @brief The frame renderer: G-buffer by ray marching, ray traced lighting, temporal
     * accumulation, particles, TAA, tone mapping, UI. See docs/ARCHITECTURE.md ("Rendering").
     *
     * Windowed mode presents to the swapchain; headless mode (no window) renders into an
     * offscreen backbuffer, which is what automated runs and screenshots use.
     */
    class Renderer {
    public:
        Renderer();
        ~Renderer();
        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /**
         * @param window nullptr = headless; then width / height give the backbuffer size.
         * @param chunkCapacity Number of chunk slots mirrored on the GPU (ChunkStore::Capacity()).
         */
        bool Initialize(Device& device, Window* window, uint32_t width, uint32_t height,
                        uint32_t chunkCapacity, const RenderSettings& settings);
        void Shutdown();
        bool IsInitialized() const;

        RenderSettings& Settings();
        const RenderStats& Stats() const;

        /// @brief UI drawn on top of the frame (optional). Not owned.
        void SetUiLayer(UiLayer* ui);

        /**
         * @brief Renders and presents one frame.
         * @return false if the frame was skipped (window minimised, swapchain being recreated).
         */
        bool RenderFrame(const FrameInput& input);

        /// @brief Saves the next presented frame as a PNG (blocks until the file is written).
        void RequestScreenshot(const std::filesystem::path& file);
        /// @brief Reloads SPIR-V from disk and rebuilds every pipeline. Keeps the old ones if anything fails.
        bool ReloadShaders();

        /// @brief The present mode in use: "headless" (no swapchain), else PresentModeName of the swapchain's mode.
        const char* PresentModeLabel() const;

        VkFormat BackbufferFormat() const;
        uint32_t BackbufferImageCount() const;
        glm::uvec2 OutputSize() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
