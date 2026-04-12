module;

#include <vulkan/vulkan.h>
#include <vector>

export module vortex.graphics:render_resources;

import vortex.memory;

namespace vortex::graphics {

    /**
     * @brief Manages offscreen render targets (Color, Depth, Velocity, TAA History).
     */
    export class RenderResources {
    public:
        RenderResources() = default;
        ~RenderResources();

        /**
         * @brief Allocates all necessary offscreen images.
         * @param allocator Pointer to memory allocator.
         * @param width Render width.
         * @param height Render height.
         */
        void Initialize(memory::MemoryAllocator* allocator, uint32_t width, uint32_t height);

        /**
         * @brief Destroys all allocated resources.
         */
        void Shutdown();

        // Accessors
        const memory::AllocatedImage& GetColor() const { return m_ColorTarget; }
        const memory::AllocatedImage& GetVelocity() const { return m_VelocityTarget; }
        const memory::AllocatedImage& GetDepth() const { return m_DepthTarget; }
        const memory::AllocatedImage& GetResolve() const { return m_ResolveTarget; }
        
        /** @brief Gets the history texture for reading (previous frame). */
        const memory::AllocatedImage& GetHistoryRead() const { return m_HistoryTexture[(m_HistoryIndex + 1) % 2]; }
        /** @brief Gets the history texture for writing (current frame). */
        const memory::AllocatedImage& GetHistoryWrite() const { return m_HistoryTexture[m_HistoryIndex]; }

        /** @brief Swaps history buffers for TAA ping-ponging. */
        void SwapHistory() { m_HistoryIndex = (m_HistoryIndex + 1) % 2; m_HistoryValid = true; }
        
        /** @brief Invalidates history (e.g. on teleport or camera cut). */
        void InvalidateHistory() { m_HistoryValid = false; }
        
        bool IsHistoryValid() const { return m_HistoryValid; }

    private:
        memory::MemoryAllocator* m_Allocator{nullptr};
        VkSampler m_DefaultSampler{VK_NULL_HANDLE};

        memory::AllocatedImage m_ColorTarget;
        memory::AllocatedImage m_VelocityTarget;
        memory::AllocatedImage m_DepthTarget;
        memory::AllocatedImage m_ResolveTarget;
        
        memory::AllocatedImage m_HistoryTexture[2];
        int m_HistoryIndex = 0;
        bool m_HistoryValid = false;
    };
}