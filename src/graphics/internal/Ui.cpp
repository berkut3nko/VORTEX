module;

#include <vulkan/vulkan.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>

module vortex.graphics;

import vortex.core;

namespace vortex::graphics {

    namespace {

        /// Smallest delta time handed to ImGui (it asserts on a non-positive value).
        constexpr float kMinDeltaTime = 1.0f / 10000.0f;
        /// Combined image sampler descriptors in the backend-owned pool (font atlas + user textures).
        constexpr uint32_t kDescriptorPoolSize = 64;

        // There is one UiLayer per process, so its backend state lives here rather than in the interface.

        /// The context created by Initialize(); nullptr while no layer is initialised.
        ImGuiContext* g_Context = nullptr;
        /// Backbuffer format. The backend keeps a pointer to it for as long as it is initialised.
        VkFormat g_ColorFormat = VK_FORMAT_UNDEFINED;

        /// Set when the backend reports a Vulkan error (negative VkResult). ImGui_ImplVulkan_Init
        /// returns true even if creating its device objects failed, so Initialize() checks this.
        bool g_BackendError = false;

        void CheckVkResult(VkResult result) {
            if (result != VK_SUCCESS) {
                Log::Error("ImGui Vulkan backend: VkResult {}", static_cast<int>(result));
            }
            if (result < 0) {
                g_BackendError = true;
            }
        }

        void DestroyContext() {
            if (g_Context != nullptr) {
                ImGui::DestroyContext(g_Context);
                g_Context = nullptr;
            }
        }
    }

    UiLayer::~UiLayer() {
        Shutdown();
    }

    bool UiLayer::Initialize(Device& device, Window* window, VkFormat colorFormat, uint32_t imageCount) {
        if (m_Initialized) {
            Log::Error("UiLayer::Initialize called twice");
            return true;
        }
        if (g_Context != nullptr) {
            Log::Error("UiLayer: another UI layer is already initialised in this process");
            return false;
        }

        const bool windowed = window != nullptr;
        if (windowed) {
            if (window->Native() == nullptr) {
                Log::Error("UiLayer: the window is not initialised");
                return false;
            }
            if (!device.IsInitialized()) {
                Log::Error("UiLayer: the device is not initialised");
                return false;
            }
            if (colorFormat == VK_FORMAT_UNDEFINED) {
                Log::Error("UiLayer: undefined colour attachment format");
                return false;
            }
        }

        IMGUI_CHECKVERSION();
        g_Context = ImGui::CreateContext();
        if (g_Context == nullptr) {
            Log::Error("UiLayer: failed to create the ImGui context");
            return false;
        }
        ImGui::SetCurrentContext(g_Context);
        ImGui::StyleColorsDark();
        ImGuiIO& io = ImGui::GetIO();

        if (windowed) {
            // Keyboard navigation stays off: with it, io.WantCaptureKeyboard is set whenever any
            // window has focus (not only while a widget is being edited), which would hide the
            // keyboard from the game and the editor after every click on a panel.

            // The window installed its own GLFW callbacks already; the backend chains to them.
            if (!ImGui_ImplGlfw_InitForVulkan(window->Native(), true)) {
                Log::Error("UiLayer: ImGui GLFW backend initialisation failed");
                DestroyContext();
                return false;
            }

            g_ColorFormat = colorFormat;

            ImGui_ImplVulkan_InitInfo info{};
            info.ApiVersion = VK_API_VERSION_1_3;
            info.Instance = device.Instance();
            info.PhysicalDevice = device.Physical();
            info.Device = device.Handle();
            info.QueueFamily = device.GraphicsQueueFamily();
            info.Queue = device.GraphicsQueue();
            info.DescriptorPool = VK_NULL_HANDLE;
            info.DescriptorPoolSize = kDescriptorPoolSize;
            info.RenderPass = VK_NULL_HANDLE;
            info.MinImageCount = 2;
            info.ImageCount = std::max<uint32_t>(2, imageCount);
            info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            info.UseDynamicRendering = true;
            info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
            info.PipelineRenderingCreateInfo.pNext = nullptr;
            info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
            info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_ColorFormat;
            info.PipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
            info.PipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
            info.Allocator = nullptr;
            info.CheckVkResultFn = CheckVkResult;

            g_BackendError = false;
            const bool backendCreated = ImGui_ImplVulkan_Init(&info);
            if (!backendCreated || g_BackendError) {
                Log::Error("UiLayer: ImGui Vulkan backend initialisation failed");
                if (backendCreated) {
                    device.WaitIdle();
                    ImGui_ImplVulkan_Shutdown();   // releases whatever was created
                }
                ImGui_ImplGlfw_Shutdown();
                DestroyContext();
                g_ColorFormat = VK_FORMAT_UNDEFINED;
                return false;
            }
        } else {
            // No backends: no settings file, and the font atlas is built on the CPU so that
            // ImGui::NewFrame() accepts it. Nothing is ever uploaded or drawn.
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            if (pixels == nullptr || !io.Fonts->IsBuilt()) {
                Log::Error("UiLayer: failed to build the font atlas");
                DestroyContext();
                return false;
            }
            io.DisplaySize = ImVec2(1.0f, 1.0f);
            io.DeltaTime = 1.0f / 60.0f;
        }

        m_Device = &device;
        m_HasBackends = windowed;
        m_FrameOpen = false;
        m_HasDrawData = false;
        m_Initialized = true;
        Log::Info("UI layer initialised (ImGui {}, {})", IMGUI_VERSION, windowed ? "windowed" : "headless");
        return true;
    }

    void UiLayer::Shutdown() {
        if (!m_Initialized) {
            return;
        }

        if (g_Context != nullptr) {
            ImGui::SetCurrentContext(g_Context);
            if (m_HasBackends) {
                if (m_Device != nullptr && m_Device->IsInitialized()) {
                    m_Device->WaitIdle();
                }
                ImGui_ImplVulkan_Shutdown();
                ImGui_ImplGlfw_Shutdown();
            }
            DestroyContext();
        }

        g_ColorFormat = VK_FORMAT_UNDEFINED;
        m_Device = nullptr;
        m_HasBackends = false;
        m_FrameOpen = false;
        m_HasDrawData = false;
        m_Initialized = false;
    }

    void UiLayer::BeginFrame(glm::uvec2 displaySize, float deltaTime) {
        if (!m_Initialized) {
            return;
        }
        ImGui::SetCurrentContext(g_Context);

        // A frame that was never ended is closed without producing draw data.
        if (m_FrameOpen) {
            ImGui::EndFrame();
            m_FrameOpen = false;
        }
        // NewFrame invalidates the draw data of the previous frame.
        m_HasDrawData = false;

        if (m_HasBackends) {
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();   // sets DisplaySize and DeltaTime from the window
        } else {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(displaySize.x), static_cast<float>(displaySize.y));
            // The comparison is written so that NaN falls back to the minimum as well.
            io.DeltaTime = deltaTime > kMinDeltaTime ? deltaTime : kMinDeltaTime;
        }

        ImGui::NewFrame();
        m_FrameOpen = true;
    }

    void UiLayer::EndFrame() {
        if (!m_Initialized || !m_FrameOpen) {
            return;
        }
        ImGui::SetCurrentContext(g_Context);
        ImGui::Render();
        m_FrameOpen = false;
        m_HasDrawData = true;
    }

    void UiLayer::Draw(VkCommandBuffer cmd) {
        if (!m_Initialized || !m_HasBackends || !m_HasDrawData || m_FrameOpen || cmd == VK_NULL_HANDLE) {
            return;
        }
        ImGui::SetCurrentContext(g_Context);
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData == nullptr || !drawData->Valid || drawData->CmdListsCount <= 0) {
            return;
        }
        if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
            return;   // minimised
        }
        ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
    }

    void UiLayer::UpdateInputCapture(Input& input) const {
        input.SetUiCapture(WantsMouse(), WantsKeyboard());
    }

    bool UiLayer::WantsMouse() const {
        if (!m_Initialized || g_Context == nullptr) {
            return false;
        }
        ImGui::SetCurrentContext(g_Context);
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool UiLayer::WantsKeyboard() const {
        if (!m_Initialized || g_Context == nullptr) {
            return false;
        }
        ImGui::SetCurrentContext(g_Context);
        return ImGui::GetIO().WantCaptureKeyboard;
    }
}
