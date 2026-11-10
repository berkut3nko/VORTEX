module;

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Declarations only: the implementation is compiled once, in the voxel module (MeshImport.cpp).
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

module vortex.graphics;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;

namespace vortex::graphics {

    namespace {

        // -------------------------------------------------------------------------------------
        //  Constants
        // -------------------------------------------------------------------------------------
        constexpr uint32_t kFramesInFlight = 2;
        constexpr uint32_t kMaxLights = 256;
        constexpr uint32_t kTaaJitterFrames = 8;
        constexpr float kTaaHistoryWeight = 0.9f;

        constexpr VkFormat kMaterialFormat = VK_FORMAT_R32_UINT;
        constexpr VkFormat kNormalFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
        constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
        constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        constexpr VkFormat kCountFormat = VK_FORMAT_R8_UNORM;
        constexpr VkFormat kHeadlessFormat = VK_FORMAT_R8G8B8A8_UNORM;

        constexpr VkDeviceSize kOccBytes = sizeof(voxel::ChunkOccupancy);                       // 1042 words per slot
        constexpr VkDeviceSize kIdBytes = static_cast<VkDeviceSize>(voxel::kChunkVolume);       // 8192 words per slot
        constexpr VkDeviceSize kChunkBytes = kOccBytes + kIdBytes;
        constexpr VkDeviceSize kStagingBytes = 8u * 1024u * 1024u;
        constexpr VkDeviceSize kMinGridBytes = 64;                                              // never bind a zero-sized buffer

        enum class Pass : uint32_t { GBuffer, Lighting, Composite, Particles, Taa, Present, Count };
        constexpr uint32_t kPassCount = static_cast<uint32_t>(Pass::Count);
        constexpr uint32_t kQueryCount = kPassCount * 2;
        constexpr std::array<const char*, kPassCount> kPassNames = {"gbuffer", "lighting", "composite", "particles", "taa", "present"};

        enum class PipelineId : size_t {
            GBufferWorld, GBufferObjectFront, GBufferObjectInside, Lighting, Composite, Particles, Taa, Present, Count
        };
        constexpr size_t kPipelineCount = static_cast<size_t>(PipelineId::Count);

        // -------------------------------------------------------------------------------------
        //  Small helpers
        // -------------------------------------------------------------------------------------
        VkClearValue ClearFloat(float r, float g, float b, float a) {
            VkClearValue value{};
            value.color.float32[0] = r;
            value.color.float32[1] = g;
            value.color.float32[2] = b;
            value.color.float32[3] = a;
            return value;
        }

        VkClearValue ClearUint(uint32_t v) {
            VkClearValue value{};
            value.color.uint32[0] = v;
            value.color.uint32[1] = v;
            value.color.uint32[2] = v;
            value.color.uint32[3] = v;
            return value;
        }

        VkClearValue ClearDepth(float depth) {
            VkClearValue value{};
            value.depthStencil.depth = depth;
            value.depthStencil.stencil = 0;
            return value;
        }

        const VkClearValue kClearZero = ClearFloat(0.0f, 0.0f, 0.0f, 0.0f);
        const VkClearValue kClearNormal = ClearFloat(0.5f, 0.5f, 1.0f, 0.0f);

        VkRenderingAttachmentInfo Attachment(VkImageView view, VkImageLayout layout, VkAttachmentLoadOp load,
                                             VkAttachmentStoreOp store, const VkClearValue& clear) {
            VkRenderingAttachmentInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            info.imageView = view;
            info.imageLayout = layout;
            info.resolveMode = VK_RESOLVE_MODE_NONE;
            info.loadOp = load;
            info.storeOp = store;
            info.clearValue = clear;
            return info;
        }

        VkRenderingAttachmentInfo ColorClear(const Image& image, const VkClearValue& clear) {
            return Attachment(image.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                              VK_ATTACHMENT_STORE_OP_STORE, clear);
        }

        void BeginRendering(VkCommandBuffer cmd, VkExtent2D extent, std::span<const VkRenderingAttachmentInfo> colors,
                            const VkRenderingAttachmentInfo* depth) {
            VkRenderingInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            info.renderArea.offset = VkOffset2D{0, 0};
            info.renderArea.extent = extent;
            info.layerCount = 1;
            info.colorAttachmentCount = static_cast<uint32_t>(colors.size());
            info.pColorAttachments = colors.empty() ? nullptr : colors.data();
            info.pDepthAttachment = depth;
            vkCmdBeginRendering(cmd, &info);
        }

        void SetViewportScissor(VkCommandBuffer cmd, VkExtent2D extent) {
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            const VkRect2D scissor{VkOffset2D{0, 0}, extent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);
        }

        /// Orders buffer writes by transfer commands against shader reads of the same buffers.
        void TransferBarrier(VkCommandBuffer cmd, bool beforeCopies) {
            const VkPipelineStageFlags2 shaderStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            if (beforeCopies) {
                barrier.srcStageMask = shaderStages;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            } else {
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = shaderStages;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            }
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }

        VkDeviceSize BytesPerPixel(VkFormat format) {
            switch (format) {
                case VK_FORMAT_R8_UNORM: return 1;
                case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
                default: return 4;
            }
        }

        /// Moves the translation of a world-space transform into render space (double precision subtraction).
        glm::mat4 Rebase(const glm::mat4& localToWorld, const glm::dvec3& origin) {
            glm::mat4 out = localToWorld;
            const glm::dvec3 translation = glm::dvec3(glm::vec3(localToWorld[3])) - origin;
            out[3] = glm::vec4(glm::vec3(translation), localToWorld[3].w);
            return out;
        }

        Camera SanitizeCamera(Camera camera) {
            camera.fovY = std::isfinite(camera.fovY) ? std::clamp(camera.fovY, 0.01f, 3.0f) : 1.0472f;
            camera.nearPlane = std::isfinite(camera.nearPlane) ? std::max(camera.nearPlane, 1.0e-4f) : 0.05f;
            return camera;
        }

        /// Reverse-Z view-projection of `camera` expressed relative to `origin`, with a clip-space jitter.
        glm::mat4 ViewProjection(const Camera& camera, const glm::dvec3& origin, float aspect, const glm::vec2& jitter) {
            const glm::vec3 position = glm::vec3(camera.position - origin);
            const glm::mat4 view = glm::lookAt(position, position + camera.Forward(), camera.Up());
            glm::mat4 projection = math::PerspectiveReverseZ(camera.fovY, aspect, camera.nearPlane);
            projection[2][0] = -jitter.x;
            projection[2][1] = -jitter.y;
            return projection * view;
        }

        std::array<PipelineDesc, kPipelineCount> MakePipelineDescs(VkFormat backbufferFormat) {
            const std::vector<VkFormat> gbufferFormats = {kMaterialFormat, kNormalFormat, kVelocityFormat};
            std::array<PipelineDesc, kPipelineCount> descs;

            auto fullscreen = [&descs](PipelineId id, const char* name, const char* fragment, std::vector<VkFormat> formats) -> PipelineDesc& {
                PipelineDesc& desc = descs[static_cast<size_t>(id)];
                desc.name = name;
                desc.vertexShader = "fullscreen.vert.spv";
                desc.fragmentShader = fragment;
                desc.colorFormats = std::move(formats);
                desc.cullMode = VK_CULL_MODE_NONE;
                return desc;
            };
            auto object = [&descs, &gbufferFormats](PipelineId id, const char* name, const char* fragment, VkCullModeFlags cull) {
                PipelineDesc& desc = descs[static_cast<size_t>(id)];
                desc.name = name;
                desc.vertexShader = "gbuffer_object.vert.spv";
                desc.fragmentShader = fragment;
                desc.colorFormats = gbufferFormats;
                desc.depthFormat = kDepthFormat;
                desc.depthTest = true;
                desc.depthWrite = true;
                desc.depthCompare = VK_COMPARE_OP_GREATER;
                desc.cullMode = cull;
                desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            };

            PipelineDesc& world = fullscreen(PipelineId::GBufferWorld, "gbuffer_world", "gbuffer_world.frag.spv", gbufferFormats);
            world.depthFormat = kDepthFormat;
            world.depthTest = true;
            world.depthWrite = true;
            world.depthCompare = VK_COMPARE_OP_GREATER;

            object(PipelineId::GBufferObjectFront, "gbuffer_object_front", "gbuffer_object_front.frag.spv", VK_CULL_MODE_BACK_BIT);
            object(PipelineId::GBufferObjectInside, "gbuffer_object_inside", "gbuffer_object_inside.frag.spv", VK_CULL_MODE_FRONT_BIT);

            fullscreen(PipelineId::Lighting, "lighting", "lighting.frag.spv", {kHdrFormat, kCountFormat});
            fullscreen(PipelineId::Composite, "composite", "composite.frag.spv", {kHdrFormat});

            PipelineDesc& particles = descs[static_cast<size_t>(PipelineId::Particles)];
            particles.name = "particles";
            particles.vertexShader = "particles.vert.spv";
            particles.fragmentShader = "particles.frag.spv";
            particles.colorFormats = {kHdrFormat};
            particles.blend = {BlendMode::Premultiplied};
            particles.depthFormat = kDepthFormat;
            particles.depthTest = true;
            particles.depthWrite = false;
            particles.depthCompare = VK_COMPARE_OP_GREATER;
            particles.cullMode = VK_CULL_MODE_NONE;

            fullscreen(PipelineId::Taa, "taa", "taa.frag.spv", {kHdrFormat});
            fullscreen(PipelineId::Present, "present", "present.frag.spv", {backbufferFormat});
            return descs;
        }

        struct PipelineSet {
            std::array<Pipeline, kPipelineCount> pipelines;
        };

        /// A texture offered to a pipeline under the name its shader declares.
        struct TextureBinding {
            std::string_view name;
            VkImageView view = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
        };

        /// Everything owned by one frame in flight.
        struct FrameContext {
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            VkSemaphore imageAcquired = VK_NULL_HANDLE;
            VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
            VkQueryPool queryPool = VK_NULL_HANDLE;
            bool queriesPending = false;       // timestamps of a submitted frame are waiting to be read

            Buffer uniforms;
            Buffer instances;
            Buffer bvh;
            Buffer drawList;
            Buffer lights;
            Buffer particles;
            Buffer staging;
            DeletionQueue deletions;
        };

        struct RenderTargets {
            VkExtent2D render{0, 0};
            VkExtent2D output{0, 0};
            Image gMaterial;
            Image gVelocity;
            Image hdr;
            Image backbuffer;                  // headless only
            std::array<Image, 2> gNormal;
            std::array<Image, 2> depth;
            std::array<Image, 2> lighting;
            std::array<Image, 2> lightCount;
            std::array<Image, 2> taa;
            bool valid = false;
            bool needsClear = true;            // ping-pong targets have undefined contents

            template <typename Fn>
            void ForEach(Fn&& fn) {
                fn(gMaterial);
                fn(gVelocity);
                fn(hdr);
                fn(backbuffer);
                for (int i = 0; i < 2; ++i) {
                    fn(gNormal[i]);
                    fn(depth[i]);
                    fn(lighting[i]);
                    fn(lightCount[i]);
                    fn(taa[i]);
                }
            }
        };

        /// GPU mirror of the chunk store, the world grid and the material table.
        struct VoxelGpu {
            uint32_t capacity = 0;             // chunk slots mirrored
            Buffer occ;
            Buffer ids;
            Buffer grid;
            Buffer materials;

            std::vector<uint8_t> uploaded;     // per slot: the GPU holds data for it
            std::vector<uint8_t> queued;       // per slot: it is in `pending`
            std::deque<uint32_t> pending;

            const voxel::World* gridWorld = nullptr;
            uint64_t gridRevision = 0;
            bool gridStale = true;             // an 'uploaded' flag changed since the grid was written
            bool gridZeroed = false;           // the first entries are defined (used while there is no world)
            bool gridValid = false;            // this frame's GPU grid mirrors the input world

            const voxel::MaterialLibrary* materialSource = nullptr;
            uint64_t materialRevision = 0;

            std::vector<VkBufferCopy> occCopies;
            std::vector<VkBufferCopy> idCopies;
        };

        /// Result of building the per-frame buffers.
        struct FrameData {
            uint32_t frontCount = 0;
            uint32_t insideCount = 0;
            uint32_t particleCount = 0;
            bool drawWorld = false;
        };
    }

    // =============================================================================================
    //  Renderer::Impl
    // =============================================================================================
    struct Renderer::Impl {
        Device* device = nullptr;
        Window* window = nullptr;
        UiLayer* ui = nullptr;
        bool initialized = false;
        bool headless = true;

        RenderSettings settings;
        RenderStats stats;

        // --- presentation ---
        Swapchain swapchain;
        std::vector<VkSemaphore> renderFinished;   // one per swapchain image
        bool swapchainDirty = false;
        bool appliedVsync = true;
        VkFormat backbufferFormat = kHeadlessFormat;
        VkExtent2D outputExtent{0, 0};

        VkSampler nearestSampler = VK_NULL_HANDLE;
        VkSampler linearSampler = VK_NULL_HANDLE;

        std::array<FrameContext, kFramesInFlight> frames;
        uint32_t frameSlot = 0;

        RenderTargets targets;
        std::unique_ptr<PipelineSet> pipelines;
        VoxelGpu voxels;

        // --- temporal state ---
        bool historyValid = false;
        uint32_t parity = 0;                       // index of the "current" ping-pong target
        Camera prevCamera;
        float prevAspect = 1.0f;
        glm::vec2 prevJitter{0.0f};
        uint64_t frameIndex = 0;

        // --- GPU timing ---
        bool timestampsSupported = false;
        uint64_t timestampMask = ~uint64_t{0};
        std::array<std::string, kPassCount> profilerNames;

        // --- screenshot ---
        bool screenshotRequested = false;
        std::filesystem::path screenshotPath;

        // --- scratch (kept to avoid per-frame allocations) ---
        std::vector<voxel::GpuInstance> gpuInstances;
        std::vector<math::AABB> instanceBounds;
        std::vector<voxel::GpuBvhNode> bvhNodes;
        std::vector<uint32_t> drawList;
        std::vector<uint32_t> insideList;
        std::vector<GpuLight> gpuLights;
        std::vector<uint32_t> lightOrder;
        std::vector<float> lightDistance;
        std::unordered_set<std::string> reported;

        // -----------------------------------------------------------------------------------------
        //  Utilities
        // -----------------------------------------------------------------------------------------
        bool ReportOnce(std::string key) { return reported.insert(std::move(key)).second; }

        const Pipeline& GetPipeline(PipelineId id) const { return pipelines->pipelines[static_cast<size_t>(id)]; }

        void Flush(const Buffer& buffer) const {
            if (buffer.allocation != VK_NULL_HANDLE) vmaFlushAllocation(device->Allocator(), buffer.allocation, 0, VK_WHOLE_SIZE);
        }

        void DeferDestroy(FrameContext& frame, const Buffer& buffer) {
            if (!buffer) return;
            Device* dev = device;
            Buffer old = buffer;
            frame.deletions.Push([dev, old]() mutable { DestroyBuffer(*dev, old); });
        }

        /**
         * Makes `buffer` at least `required` bytes (power-of-two growth). The old buffer goes to
         * the frame's deletion queue. Contents are not preserved. On failure the old buffer stays.
         */
        bool EnsureCapacity(FrameContext& frame, Buffer& buffer, VkDeviceSize required, VkBufferUsageFlags usage,
                            BufferKind kind, std::string_view name) {
            if (buffer && buffer.size >= required) return true;
            const VkDeviceSize newSize = std::bit_ceil(std::max<VkDeviceSize>(required, 64));
            Buffer grown = CreateBuffer(*device, newSize, usage, kind, name);
            if (!grown) {
                if (ReportOnce(std::format("grow/{}", name))) {
                    Log::Error("Renderer: could not grow buffer '{}' to {} bytes; its contents are truncated", name, newSize);
                }
                return false;
            }
            DeferDestroy(frame, buffer);
            buffer = grown;
            return true;
        }

        /// Writes an array into a per-frame storage buffer. Returns how many elements fit.
        template <typename T>
        uint32_t Upload(FrameContext& frame, Buffer& buffer, std::span<const T> data, std::string_view name) {
            if (data.empty()) return 0;
            size_t count = data.size();
            if (!EnsureCapacity(frame, buffer, data.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferKind::CpuToGpu, name)) {
                count = std::min<size_t>(count, static_cast<size_t>(buffer.size / sizeof(T)));
            }
            if (count != 0) {
                std::memcpy(buffer.mapped, data.data(), count * sizeof(T));
                Flush(buffer);
            }
            return static_cast<uint32_t>(count);
        }

        VkExtent2D DesiredRenderExtent() const {
            float scale = settings.renderScale;
            if (!std::isfinite(scale)) scale = 1.0f;
            scale = std::clamp(scale, 0.25f, 1.0f);
            VkExtent2D extent;
            extent.width = std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<float>(outputExtent.width) * scale)));
            extent.height = std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<float>(outputExtent.height) * scale)));
            return extent;
        }

        // -----------------------------------------------------------------------------------------
        //  Frame contexts
        // -----------------------------------------------------------------------------------------
        bool CreateSyncObjects(FrameContext& frame) {
            const VkDevice dev = device->Handle();
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (frame.fence == VK_NULL_HANDLE && vkCreateFence(dev, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS) return false;

            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (frame.imageAcquired == VK_NULL_HANDLE &&
                vkCreateSemaphore(dev, &semaphoreInfo, nullptr, &frame.imageAcquired) != VK_SUCCESS) return false;
            return true;
        }

        bool CreateFrames() {
            const VkDevice dev = device->Handle();
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                FrameContext& frame = frames[i];

                VkCommandPoolCreateInfo poolInfo{};
                poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                poolInfo.queueFamilyIndex = device->GraphicsQueueFamily();
                if (vkCreateCommandPool(dev, &poolInfo, nullptr, &frame.commandPool) != VK_SUCCESS) return false;

                VkCommandBufferAllocateInfo allocInfo{};
                allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocInfo.commandPool = frame.commandPool;
                allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocInfo.commandBufferCount = 1;
                if (vkAllocateCommandBuffers(dev, &allocInfo, &frame.cmd) != VK_SUCCESS) return false;

                if (!CreateSyncObjects(frame)) return false;

                // Eight pipelines bind one set each per frame; sized with generous headroom.
                const std::array<VkDescriptorPoolSize, 3> sizes = {
                    VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32},
                    VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 160},
                    VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 96},
                };
                VkDescriptorPoolCreateInfo descriptorInfo{};
                descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                descriptorInfo.maxSets = 32;
                descriptorInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
                descriptorInfo.pPoolSizes = sizes.data();
                if (vkCreateDescriptorPool(dev, &descriptorInfo, nullptr, &frame.descriptorPool) != VK_SUCCESS) return false;

                if (timestampsSupported) {
                    VkQueryPoolCreateInfo queryInfo{};
                    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
                    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                    queryInfo.queryCount = kQueryCount;
                    if (vkCreateQueryPool(dev, &queryInfo, nullptr, &frame.queryPool) != VK_SUCCESS) return false;
                }

                const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                frame.uniforms = CreateBuffer(*device, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, BufferKind::CpuToGpu,
                                              std::format("Frame {} uniforms", i));
                frame.instances = CreateBuffer(*device, 64 * sizeof(voxel::GpuInstance), storage, BufferKind::CpuToGpu,
                                               std::format("Frame {} instances", i));
                frame.bvh = CreateBuffer(*device, 128 * sizeof(voxel::GpuBvhNode), storage, BufferKind::CpuToGpu,
                                         std::format("Frame {} bvh", i));
                frame.drawList = CreateBuffer(*device, 64 * sizeof(uint32_t), storage, BufferKind::CpuToGpu,
                                              std::format("Frame {} draw list", i));
                frame.lights = CreateBuffer(*device, 16 * sizeof(GpuLight), storage, BufferKind::CpuToGpu,
                                            std::format("Frame {} lights", i));
                frame.particles = CreateBuffer(*device, 256 * sizeof(fx::GpuParticle), storage, BufferKind::CpuToGpu,
                                               std::format("Frame {} particles", i));
                frame.staging = CreateBuffer(*device, kStagingBytes, 0, BufferKind::Staging, std::format("Frame {} staging", i));
                if (!frame.uniforms || !frame.instances || !frame.bvh || !frame.drawList || !frame.lights || !frame.particles ||
                    !frame.staging) {
                    return false;
                }
            }
            return true;
        }

        void DestroyFrames(bool deviceAlive) {
            const VkDevice dev = deviceAlive ? device->Handle() : VK_NULL_HANDLE;
            for (FrameContext& frame : frames) {
                frame.deletions.Flush();
                DestroyBuffer(*device, frame.staging);
                DestroyBuffer(*device, frame.particles);
                DestroyBuffer(*device, frame.lights);
                DestroyBuffer(*device, frame.drawList);
                DestroyBuffer(*device, frame.bvh);
                DestroyBuffer(*device, frame.instances);
                DestroyBuffer(*device, frame.uniforms);
                if (dev != VK_NULL_HANDLE) {
                    if (frame.queryPool != VK_NULL_HANDLE) vkDestroyQueryPool(dev, frame.queryPool, nullptr);
                    if (frame.descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, frame.descriptorPool, nullptr);
                    if (frame.imageAcquired != VK_NULL_HANDLE) vkDestroySemaphore(dev, frame.imageAcquired, nullptr);
                    if (frame.fence != VK_NULL_HANDLE) vkDestroyFence(dev, frame.fence, nullptr);
                    if (frame.commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(dev, frame.commandPool, nullptr);   // frees cmd
                }
                frame.queryPool = VK_NULL_HANDLE;
                frame.descriptorPool = VK_NULL_HANDLE;
                frame.imageAcquired = VK_NULL_HANDLE;
                frame.fence = VK_NULL_HANDLE;
                frame.commandPool = VK_NULL_HANDLE;
                frame.cmd = VK_NULL_HANDLE;
                frame.queriesPending = false;
            }
        }

        void DetectTimestampSupport() {
            timestampsSupported = false;
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device->Physical(), &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            if (count != 0) vkGetPhysicalDeviceQueueFamilyProperties(device->Physical(), &count, families.data());
            const uint32_t family = device->GraphicsQueueFamily();
            if (family >= count) return;
            const uint32_t bits = families[family].timestampValidBits;
            if (bits == 0 || !(device->TimestampPeriod() > 0.0f)) return;
            timestampMask = bits >= 64 ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1);
            timestampsSupported = true;
        }

        /// Reads the timestamps this frame slot recorded the last time it was submitted.
        void CollectTimings(FrameContext& frame) {
            if (frame.queryPool == VK_NULL_HANDLE || !frame.queriesPending) return;
            frame.queriesPending = false;
            std::array<uint64_t, kQueryCount> ticks{};
            const VkResult result = vkGetQueryPoolResults(device->Handle(), frame.queryPool, 0, kQueryCount, sizeof(ticks),
                                                          ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result != VK_SUCCESS) return;
            const double nsPerTick = static_cast<double>(device->TimestampPeriod());
            for (uint32_t i = 0; i < kPassCount; ++i) {
                const uint64_t delta = (ticks[i * 2 + 1] - ticks[i * 2]) & timestampMask;
                const float ms = static_cast<float>(static_cast<double>(delta) * nsPerTick * 1.0e-6);
                stats.passes[i].milliseconds = ms;
                Profiler::AddSample(profilerNames[i], ms);
            }
        }

        void BeginPass(FrameContext& frame, Pass pass) {
            if (frame.queryPool == VK_NULL_HANDLE) return;
            vkCmdWriteTimestamp(frame.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.queryPool, static_cast<uint32_t>(pass) * 2);
        }

        void EndPass(FrameContext& frame, Pass pass) {
            if (frame.queryPool == VK_NULL_HANDLE) return;
            vkCmdWriteTimestamp(frame.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.queryPool, static_cast<uint32_t>(pass) * 2 + 1);
        }

        // -----------------------------------------------------------------------------------------
        //  Render targets
        // -----------------------------------------------------------------------------------------
        void DestroyTargets() {
            targets.ForEach([this](Image& image) { DestroyImage(*device, image); });
            targets.valid = false;
            targets.needsClear = true;
            targets.render = VkExtent2D{0, 0};
            targets.output = VkExtent2D{0, 0};
        }

        /// (Re)creates every render target for the current output size. The device must be idle.
        bool RecreateTargets() {
            DestroyTargets();
            historyValid = false;
            parity = 0;
            if (outputExtent.width == 0 || outputExtent.height == 0) return false;

            const VkExtent2D render = DesiredRenderExtent();
            const VkImageUsageFlags color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            const VkImageUsageFlags depth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

            targets.gMaterial = CreateImage2D(*device, kMaterialFormat, render, color, "gMaterial");
            targets.gVelocity = CreateImage2D(*device, kVelocityFormat, render, color, "gVelocity");
            targets.hdr = CreateImage2D(*device, kHdrFormat, render, color, "hdr");
            for (int i = 0; i < 2; ++i) {
                targets.gNormal[i] = CreateImage2D(*device, kNormalFormat, render, color, std::format("gNormal[{}]", i));
                targets.depth[i] = CreateImage2D(*device, kDepthFormat, render, depth, std::format("depth[{}]", i));
                targets.lighting[i] = CreateImage2D(*device, kHdrFormat, render, color, std::format("lighting[{}]", i));
                targets.lightCount[i] = CreateImage2D(*device, kCountFormat, render, color, std::format("lightCount[{}]", i));
                targets.taa[i] = CreateImage2D(*device, kHdrFormat, render, color, std::format("taa[{}]", i));
            }
            bool ok = targets.gMaterial && targets.gVelocity && targets.hdr;
            for (int i = 0; i < 2; ++i) {
                ok = ok && targets.gNormal[i] && targets.depth[i] && targets.lighting[i] && targets.lightCount[i] && targets.taa[i];
            }
            if (headless) {
                targets.backbuffer = CreateImage2D(*device, kHeadlessFormat, outputExtent,
                                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "backbuffer");
                ok = ok && targets.backbuffer;
            }
            if (!ok) {
                Log::Error("Renderer: creating the render targets failed ({}x{} render, {}x{} output)", render.width, render.height,
                           outputExtent.width, outputExtent.height);
                DestroyTargets();
                return false;
            }
            targets.render = render;
            targets.output = outputExtent;
            targets.valid = true;
            targets.needsClear = true;
            return true;
        }

        /// Gives the ping-pong targets defined contents so the first frame never samples garbage.
        void ClearHistoryTargets(VkCommandBuffer cmd) {
            for (int i = 0; i < 2; ++i) {
                TransitionImage(cmd, targets.gNormal[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                TransitionImage(cmd, targets.lighting[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                TransitionImage(cmd, targets.lightCount[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                TransitionImage(cmd, targets.taa[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                TransitionImage(cmd, targets.depth[i], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
                const std::array<VkRenderingAttachmentInfo, 4> colors = {
                    ColorClear(targets.gNormal[i], kClearNormal),
                    ColorClear(targets.lighting[i], kClearZero),
                    ColorClear(targets.lightCount[i], kClearZero),
                    ColorClear(targets.taa[i], kClearZero),
                };
                const VkRenderingAttachmentInfo depth = Attachment(targets.depth[i].view, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                                                   VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ClearDepth(0.0f));
                BeginRendering(cmd, targets.render, colors, &depth);
                vkCmdEndRendering(cmd);
            }
            targets.needsClear = false;
        }

        // -----------------------------------------------------------------------------------------
        //  Pipelines
        // -----------------------------------------------------------------------------------------
        std::unique_ptr<PipelineSet> BuildPipelines(VkFormat backbuffer) {
            auto set = std::make_unique<PipelineSet>();
            const std::array<PipelineDesc, kPipelineCount> descs = MakePipelineDescs(backbuffer);
            for (size_t i = 0; i < kPipelineCount; ++i) {
                if (!set->pipelines[i].Create(*device, descs[i])) {
                    Log::Error("Renderer: pipeline '{}' could not be created", descs[i].name);
                    return nullptr;   // nothing of the partial set was ever used by the GPU
                }
            }
            return set;
        }

        bool ReloadShaders() {
            if (!initialized) return false;
            std::unique_ptr<PipelineSet> fresh = BuildPipelines(backbufferFormat);
            if (!fresh) {
                Log::Warn("Renderer: shader reload failed, keeping the current pipelines");
                return false;
            }
            device->WaitIdle();          // frames in flight may still use the old set
            pipelines = std::move(fresh);
            reported.clear();
            Log::Info("Renderer: shaders reloaded");
            return true;
        }

        // -----------------------------------------------------------------------------------------
        //  Presentation surface
        // -----------------------------------------------------------------------------------------
        void DestroyRenderFinished() {
            if (device->Handle() != VK_NULL_HANDLE) {
                for (VkSemaphore semaphore : renderFinished) {
                    if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device->Handle(), semaphore, nullptr);
                }
            }
            renderFinished.clear();
        }

        bool CreateRenderFinished() {
            DestroyRenderFinished();
            VkSemaphoreCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            for (uint32_t i = 0; i < swapchain.ImageCount(); ++i) {
                VkSemaphore semaphore = VK_NULL_HANDLE;
                if (vkCreateSemaphore(device->Handle(), &info, nullptr, &semaphore) != VK_SUCCESS) {
                    Log::Error("Renderer: creating a render-finished semaphore failed");
                    return false;
                }
                renderFinished.push_back(semaphore);
            }
            return true;
        }

        /// Recreates the swapchain and everything that depends on its size / format (windowed only).
        bool RecreateSurface() {
            swapchainDirty = true;
            device->WaitIdle();
            const glm::ivec2 size = window->FramebufferSize();
            if (size.x <= 0 || size.y <= 0) return false;
            if (!swapchain.Recreate(static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), settings.vsync)) return false;
            appliedVsync = settings.vsync;
            outputExtent = swapchain.Extent();
            if (!CreateRenderFinished()) return false;

            if (swapchain.Format() != backbufferFormat) {
                // The present pipeline is compiled for the backbuffer format.
                backbufferFormat = swapchain.Format();
                pipelines = BuildPipelines(backbufferFormat);
                if (!pipelines) Log::Error("Renderer: pipelines could not be rebuilt for the new swapchain format");
            }
            if (!RecreateTargets()) return false;
            swapchainDirty = false;
            return true;
        }

        /// Resize / minimise / settings handling. Returns false if this frame must be skipped.
        bool PrepareSurface() {
            if (!headless) {
                const glm::ivec2 size = window->FramebufferSize();
                if (size.x <= 0 || size.y <= 0 || window->IsMinimized()) return false;
                const bool resized = window->ConsumeResized();
                if (resized || swapchainDirty || !swapchain.IsValid() || settings.vsync != appliedVsync) {
                    RecreateSurface();
                    return false;
                }
            }
            const VkExtent2D wanted = DesiredRenderExtent();
            if (!targets.valid || wanted.width != targets.render.width || wanted.height != targets.render.height) {
                device->WaitIdle();
                if (!RecreateTargets()) return false;
            }
            return pipelines != nullptr;
        }

        // -----------------------------------------------------------------------------------------
        //  GPU voxel store
        // -----------------------------------------------------------------------------------------
        bool CreateVoxelStore(uint32_t chunkCapacity) {
            VoxelGpu& v = voxels;
            v.capacity = chunkCapacity;
            const VkDeviceSize slots = std::max<uint32_t>(chunkCapacity, 1u);
            const VkDeviceSize occBytes = slots * kOccBytes;
            const VkDeviceSize idBytes = slots * kIdBytes;
            const VkDeviceSize maxRange = device->Properties().limits.maxStorageBufferRange;
            if (idBytes > maxRange || idBytes / 4 > std::numeric_limits<uint32_t>::max()) {
                Log::Error("Renderer: {} chunk slots need a {} byte id buffer, more than this GPU can bind ({} bytes)",
                           chunkCapacity, idBytes, maxRange);
                return false;
            }
            const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            v.occ = CreateBuffer(*device, occBytes, storage, BufferKind::GpuOnly, "u_Occ");
            v.ids = CreateBuffer(*device, idBytes, storage, BufferKind::GpuOnly, "u_Ids");
            v.grid = CreateBuffer(*device, kMinGridBytes, storage, BufferKind::GpuOnly, "u_Grid");
            v.materials = CreateBuffer(*device, voxel::kPaletteSize * sizeof(voxel::GpuMaterial), storage, BufferKind::GpuOnly, "u_Materials");
            if (!v.occ || !v.ids || !v.grid || !v.materials) return false;
            v.uploaded.assign(chunkCapacity, 0);
            v.queued.assign(chunkCapacity, 0);
            v.pending.clear();
            v.gridWorld = nullptr;
            v.gridRevision = 0;
            v.gridStale = true;
            v.gridZeroed = false;
            v.gridValid = false;
            v.materialSource = nullptr;
            v.materialRevision = 0;
            return true;
        }

        void DestroyVoxelStore() {
            DestroyBuffer(*device, voxels.materials);
            DestroyBuffer(*device, voxels.grid);
            DestroyBuffer(*device, voxels.ids);
            DestroyBuffer(*device, voxels.occ);
            voxels.uploaded.clear();
            voxels.queued.clear();
            voxels.pending.clear();
            voxels.capacity = 0;
        }

        void EnqueueSlot(uint32_t slot) {
            VoxelGpu& v = voxels;
            if (slot >= v.capacity) {
                if (ReportOnce("slot-capacity")) {
                    Log::Error("Renderer: chunk slot {} is beyond the GPU chunk capacity ({}); such chunks are not drawn", slot, v.capacity);
                }
                return;
            }
            if (v.queued[slot]) return;
            v.queued[slot] = 1;
            v.pending.push_back(slot);
        }

        /// Forgets everything the GPU mirror holds (after a frame whose commands never executed).
        void InvalidateVoxelMirror(const voxel::ChunkStore& store) {
            VoxelGpu& v = voxels;
            std::fill(v.uploaded.begin(), v.uploaded.end(), uint8_t{0});
            std::fill(v.queued.begin(), v.queued.end(), uint8_t{0});
            v.pending.clear();
            v.gridStale = true;
            v.gridZeroed = false;
            v.gridWorld = nullptr;
            v.materialSource = nullptr;
            const uint32_t end = std::min(v.capacity, store.HighWaterMark());
            for (uint32_t slot = 0; slot < end; ++slot) {
                if (store.IsAllocated(slot)) EnqueueSlot(slot);
            }
        }

        /// The world if its grid is something the shaders can traverse, else nullptr.
        const voxel::World* UsableWorld(const voxel::World* world) {
            if (world == nullptr) return nullptr;
            const glm::ivec3 size = world->WindowSize();
            const bool valid = size.x > 0 && size.y > 0 && size.z > 0 &&
                               static_cast<size_t>(size.x) * static_cast<size_t>(size.y) * static_cast<size_t>(size.z) == world->Grid().size() &&
                               world->VoxelSize() > 0.0f;
            if (!valid && ReportOnce("world-grid")) Log::Error("Renderer: the world grid does not match its window size; the world is not drawn");
            return valid ? world : nullptr;
        }

        /**
         * Brings the device-local buffers up to date through this frame's staging buffer:
         * materials, budgeted chunk uploads, then the grid (which only exposes uploaded chunks).
         * Staging layout: [materials][grid][chunks ...].
         */
        void SyncVoxelData(FrameContext& frame, const FrameInput& input) {
            VoxelGpu& v = voxels;
            voxel::ChunkStore& store = *input.store;
            const VkCommandBuffer cmd = frame.cmd;
            v.gridValid = false;
            stats.chunkUploads = 0;

            for (uint32_t slot : store.TakeDirty()) EnqueueSlot(slot);

            // --- what has to be staged -----------------------------------------------------------
            const std::span<const voxel::GpuMaterial> materialData = input.materials->GpuData();
            bool materialsDirty = !materialData.empty() &&
                                  (input.materials != v.materialSource || input.materials->Revision() != v.materialRevision);
            const VkDeviceSize materialBytes = materialsDirty ? materialData.size_bytes() : 0;

            const voxel::World* world = UsableWorld(input.world);
            const VkDeviceSize gridBytes = world != nullptr ? world->Grid().size_bytes() : kMinGridBytes;
            const VkDeviceSize reserved = materialBytes + gridBytes;

            EnsureCapacity(frame, frame.staging, reserved + kChunkBytes, 0, BufferKind::Staging, "staging");
            if (frame.staging.size < reserved) {
                // Not even the tables fit: leave the GPU state untouched and retry next frame.
                stats.chunkUploadsPending = static_cast<uint32_t>(v.pending.size());
                return;
            }
            uint8_t* staging = static_cast<uint8_t*>(frame.staging.mapped);

            // --- materials -----------------------------------------------------------------------
            if (materialsDirty && materialBytes > v.materials.size) {
                Buffer grown = CreateBuffer(*device, std::bit_ceil(materialBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferKind::GpuOnly, "u_Materials");
                if (grown) {
                    DeferDestroy(frame, v.materials);
                    v.materials = grown;
                } else {
                    materialsDirty = false;
                    if (ReportOnce("materials-grow")) Log::Error("Renderer: the material buffer could not grow to {} bytes", materialBytes);
                }
            }
            if (materialsDirty) std::memcpy(staging, materialData.data(), materialBytes);

            // --- chunks (budgeted) ---------------------------------------------------------------
            v.occCopies.clear();
            v.idCopies.clear();
            VkDeviceSize cursor = reserved;
            const uint32_t budget = std::max<uint32_t>(settings.maxChunkUploadsPerFrame, 1u);
            uint32_t uploads = 0;
            while (!v.pending.empty() && uploads < budget && cursor + kChunkBytes <= frame.staging.size) {
                const uint32_t slot = v.pending.front();
                v.pending.pop_front();
                v.queued[slot] = 0;
                const voxel::Chunk* chunk = store.TryGet(slot);
                if (chunk == nullptr) {
                    // Freed since it was marked dirty: the GPU copy is stale and must not be exposed.
                    if (v.uploaded[slot]) {
                        v.uploaded[slot] = 0;
                        v.gridStale = true;
                    }
                    continue;
                }
                std::memcpy(staging + cursor, &chunk->occ, kOccBytes);
                v.occCopies.push_back(VkBufferCopy{cursor, static_cast<VkDeviceSize>(slot) * kOccBytes, kOccBytes});
                cursor += kOccBytes;
                std::memcpy(staging + cursor, chunk->ids, kIdBytes);
                v.idCopies.push_back(VkBufferCopy{cursor, static_cast<VkDeviceSize>(slot) * kIdBytes, kIdBytes});
                cursor += kIdBytes;
                if (!v.uploaded[slot]) {
                    v.uploaded[slot] = 1;
                    v.gridStale = true;
                }
                ++uploads;
            }

            // --- grid ----------------------------------------------------------------------------
            VkDeviceSize gridCopyBytes = 0;
            if (world != nullptr) {
                bool gridOk = true;
                if (gridBytes > v.grid.size) {
                    Buffer grown = CreateBuffer(*device, std::bit_ceil(gridBytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferKind::GpuOnly, "u_Grid");
                    if (grown) {
                        DeferDestroy(frame, v.grid);
                        v.grid = grown;
                        v.gridStale = true;
                        v.gridZeroed = false;
                    } else {
                        gridOk = false;
                        if (ReportOnce("grid-grow")) Log::Error("Renderer: the grid buffer could not grow to {} bytes", gridBytes);
                    }
                }
                if (gridOk) {
                    if (world != v.gridWorld || world->GridRevision() != v.gridRevision || v.gridStale) {
                        const std::span<const uint32_t> grid = world->Grid();
                        uint32_t* out = reinterpret_cast<uint32_t*>(staging + materialBytes);   // 4-byte aligned: materialBytes % 32 == 0
                        for (size_t i = 0; i < grid.size(); ++i) {
                            uint32_t entry = grid[i];
                            if (entry != 0 && (entry & 0x80000000u) == 0) {
                                const uint32_t slot = entry - 1;
                                if (slot >= v.capacity) {
                                    EnqueueSlot(slot);   // reports the capacity problem once
                                    entry = 0;
                                } else if (!v.uploaded[slot]) {
                                    if (store.IsAllocated(slot)) EnqueueSlot(slot);
                                    entry = 0;           // not on the GPU yet: hide it
                                }
                            }
                            out[i] = entry;
                        }
                        gridCopyBytes = gridBytes;
                        v.gridWorld = world;
                        v.gridRevision = world->GridRevision();
                        v.gridStale = false;
                        v.gridZeroed = true;
                    }
                    v.gridValid = true;
                }
            } else if (!v.gridZeroed) {
                std::memset(staging + materialBytes, 0, kMinGridBytes);
                gridCopyBytes = kMinGridBytes;
                v.gridZeroed = true;
                v.gridStale = true;   // a world appearing later must rewrite the grid
            }

            // --- record the copies ---------------------------------------------------------------
            if (materialsDirty || gridCopyBytes != 0 || !v.occCopies.empty()) {
                Flush(frame.staging);
                TransferBarrier(cmd, true);
                if (materialsDirty) {
                    const VkBufferCopy region{0, 0, materialBytes};
                    vkCmdCopyBuffer(cmd, frame.staging.handle, v.materials.handle, 1, &region);
                    v.materialSource = input.materials;
                    v.materialRevision = input.materials->Revision();
                }
                if (!v.occCopies.empty()) {
                    vkCmdCopyBuffer(cmd, frame.staging.handle, v.occ.handle, static_cast<uint32_t>(v.occCopies.size()), v.occCopies.data());
                    vkCmdCopyBuffer(cmd, frame.staging.handle, v.ids.handle, static_cast<uint32_t>(v.idCopies.size()), v.idCopies.data());
                }
                if (gridCopyBytes != 0) {
                    const VkBufferCopy region{materialBytes, 0, gridCopyBytes};
                    vkCmdCopyBuffer(cmd, frame.staging.handle, v.grid.handle, 1, &region);
                }
                TransferBarrier(cmd, false);
            }

            stats.chunkUploads = uploads;
            stats.chunkUploadsPending = static_cast<uint32_t>(v.pending.size());
        }

        // -----------------------------------------------------------------------------------------
        //  Frame data
        // -----------------------------------------------------------------------------------------
        FrameData BuildFrameData(FrameContext& frame, const FrameInput& input, const Camera& camera) {
            FrameData data;
            const voxel::World* world = voxels.gridValid ? input.world : nullptr;
            data.drawWorld = world != nullptr;

            // Render space: relative to the window origin even if the grid could not be mirrored.
            const voxel::World* spaceWorld = UsableWorld(input.world);
            const glm::dvec3 origin = spaceWorld != nullptr ? spaceWorld->WindowOrigin() : glm::dvec3(0.0);
            const double voxelSize = spaceWorld != nullptr ? static_cast<double>(spaceWorld->VoxelSize()) : 1.0;

            const glm::vec2 renderSize(static_cast<float>(targets.render.width), static_cast<float>(targets.render.height));
            const glm::vec2 outputSize(static_cast<float>(targets.output.width), static_cast<float>(targets.output.height));
            const float aspect = renderSize.x / renderSize.y;

            // --- camera --------------------------------------------------------------------------
            const glm::dvec3 cameraPosD = camera.position - origin;
            const glm::vec3 cameraPos = glm::vec3(cameraPosD);
            const glm::vec3 forward = camera.Forward();
            const glm::vec3 right = camera.Right();
            const glm::vec3 up = camera.Up();
            const float tanHalfFovY = std::tan(camera.fovY * 0.5f);

            glm::vec2 jitter(0.0f);
            if (settings.taa) {
                const uint32_t index = static_cast<uint32_t>(frameIndex % kTaaJitterFrames) + 1;
                jitter = (glm::vec2(math::Halton(index, 2), math::Halton(index, 3)) - 0.5f) * 2.0f / renderSize;
            }
            const glm::mat4 viewProjNoJitter = ViewProjection(camera, origin, aspect, glm::vec2(0.0f));

            FrameUniforms u;
            u.viewProj = ViewProjection(camera, origin, aspect, jitter);
            u.viewProjNoJitter = viewProjNoJitter;
            u.prevViewProjNoJitter = historyValid ? ViewProjection(prevCamera, origin, prevAspect, glm::vec2(0.0f)) : viewProjNoJitter;
            u.cameraPos = glm::vec4(cameraPos, camera.nearPlane);
            u.cameraRight = glm::vec4(right * (tanHalfFovY * aspect), 0.0f);
            u.cameraUp = glm::vec4(up * tanHalfFovY, 0.0f);
            u.cameraForward = glm::vec4(forward, 0.0f);
            u.cameraPosVoxel = glm::vec4(glm::vec3(cameraPosD / voxelSize), static_cast<float>(voxelSize));
            u.renderSize = glm::vec4(renderSize, 1.0f / renderSize);
            u.outputSize = glm::vec4(outputSize, 1.0f / outputSize);
            u.jitter = glm::vec4(jitter, historyValid ? prevJitter : jitter);

            const float sunLength = glm::length(input.sun.direction);
            const glm::vec3 sunDirection = sunLength > 1.0e-6f ? input.sun.direction / sunLength : glm::vec3(0.0f, 1.0f, 0.0f);
            u.sunDirection = glm::vec4(sunDirection, std::tan(std::clamp(input.sun.angularRadius, 0.0f, 1.5f)));
            u.sunRadiance = glm::vec4(input.sun.color * input.sun.intensity, 0.0f);
            u.skyZenith = glm::vec4(input.sky.zenith, 0.0f);
            u.skyHorizon = glm::vec4(input.sky.horizon, 0.0f);
            u.skyGround = glm::vec4(input.sky.ground, 0.0f);
            u.fogColor = glm::vec4(input.fog.color, input.fog.density);
            u.fogParams = glm::vec4(input.fog.heightFalloff, static_cast<float>(static_cast<double>(input.fog.baseHeight) - origin.y),
                                    input.fog.startDistance, input.fog.maxOpacity);
            u.ambientParams = glm::vec4(settings.ambientIntensity, settings.ambientDistance, settings.emissiveScale, settings.exposure);
            u.temporalParams = glm::vec4(std::clamp(settings.lightingHistory, 1.0f, 255.0f), kTaaHistoryWeight,
                                         historyValid ? 1.0f : 0.0f, static_cast<float>(std::fmod(input.time, 3600.0)));
            u.gridSize = world != nullptr ? glm::ivec4(world->WindowSize(), 0) : glm::ivec4(0);
            u.gridMinChunk = world != nullptr ? glm::ivec4(world->WindowMinChunk(), 0) : glm::ivec4(0);

            // --- instances -----------------------------------------------------------------------
            const uint32_t paletteCount = input.materials->PaletteCount();
            gpuInstances.clear();
            for (const InstanceDesc& desc : input.instances) {
                // A slot the GPU has not received (or cannot hold) must never be traversed.
                if (desc.slot >= voxels.capacity || !voxels.uploaded[desc.slot]) {
                    // Make sure a live slot that slipped through the dirty list is uploaded eventually.
                    if (input.store->IsAllocated(desc.slot)) EnqueueSlot(desc.slot);
                    continue;
                }
                const glm::ivec3 boundsMin = glm::clamp(desc.boundsMin, glm::ivec3(0), glm::ivec3(voxel::kChunkSize));
                const glm::ivec3 boundsMax = glm::clamp(desc.boundsMax, glm::ivec3(0), glm::ivec3(voxel::kChunkSize));
                if (glm::any(glm::greaterThanEqual(boundsMin, boundsMax))) continue;

                voxel::GpuInstance instance;
                instance.localToWorld = Rebase(desc.localToWorld, origin);
                const float determinant = glm::determinant(glm::mat3(instance.localToWorld));
                if (!(std::abs(determinant) > 1.0e-12f)) continue;   // degenerate (or NaN) transform
                instance.worldToLocal = glm::inverse(instance.localToWorld);
                instance.prevLocalToWorld = Rebase(desc.prevLocalToWorld, origin);
                instance.boundsMinSlot = glm::ivec4(boundsMin, static_cast<int>(desc.slot));
                const voxel::PaletteId palette = desc.palette < paletteCount ? desc.palette : voxel::kWorldPalette;
                instance.boundsMaxPalette = glm::ivec4(boundsMax, static_cast<int>(palette * voxel::kPaletteSize));
                gpuInstances.push_back(instance);
            }
            const uint32_t instanceCount = Upload<voxel::GpuInstance>(frame, frame.instances, gpuInstances, "instances");
            gpuInstances.resize(instanceCount);

            // --- BVH over all instances + draw list ----------------------------------------------
            instanceBounds.clear();
            drawList.clear();
            insideList.clear();
            const math::Frustum frustum = math::Frustum::FromViewProjection(viewProjNoJitter);
            const float insideMargin = 2.0f * camera.nearPlane + 0.05f;
            for (uint32_t i = 0; i < instanceCount; ++i) {
                const voxel::GpuInstance& instance = gpuInstances[i];
                const glm::vec3 boundsMin = glm::vec3(glm::ivec3(instance.boundsMinSlot));
                const glm::vec3 boundsMax = glm::vec3(glm::ivec3(instance.boundsMaxPalette));
                const math::AABB box = math::AABB(boundsMin, boundsMax).Transformed(instance.localToWorld);
                instanceBounds.push_back(box);
                if (!frustum.Intersects(box)) continue;

                // Boxes that contain the camera (or straddle the near plane) are drawn from the inside.
                const float metresPerUnit = glm::length(glm::vec3(instance.localToWorld[0]));
                const float margin = metresPerUnit > 1.0e-9f ? insideMargin / metresPerUnit : 0.0f;
                const glm::vec3 local = glm::vec3(instance.worldToLocal * glm::vec4(cameraPos, 1.0f));
                const bool inside = glm::all(glm::greaterThanEqual(local, boundsMin - margin)) &&
                                    glm::all(glm::lessThanEqual(local, boundsMax + margin));
                (inside ? insideList : drawList).push_back(i);
            }
            voxel::BuildBvh(instanceBounds, bvhNodes);
            uint32_t bvhCount = Upload<voxel::GpuBvhNode>(frame, frame.bvh, bvhNodes, "bvh");
            if (bvhCount != bvhNodes.size()) bvhCount = 0;   // a truncated tree is not traversable

            uint32_t frontCount = static_cast<uint32_t>(drawList.size());
            uint32_t insideCount = static_cast<uint32_t>(insideList.size());
            drawList.insert(drawList.end(), insideList.begin(), insideList.end());
            const uint32_t drawCount = Upload<uint32_t>(frame, frame.drawList, drawList, "draw list");
            frontCount = std::min(frontCount, drawCount);
            insideCount = std::min(insideCount, drawCount - frontCount);
            data.frontCount = frontCount;
            data.insideCount = insideCount;

            // --- lights (nearest kMaxLights) -----------------------------------------------------
            lightOrder.resize(input.lights.size());
            std::iota(lightOrder.begin(), lightOrder.end(), 0u);
            if (lightOrder.size() > kMaxLights) {
                lightDistance.resize(input.lights.size());
                for (size_t i = 0; i < input.lights.size(); ++i) {
                    const glm::dvec3 delta = input.lights[i].position - camera.position;
                    lightDistance[i] = static_cast<float>(glm::dot(delta, delta));
                }
                std::nth_element(lightOrder.begin(), lightOrder.begin() + kMaxLights, lightOrder.end(),
                                 [this](uint32_t a, uint32_t b) { return lightDistance[a] < lightDistance[b]; });
                lightOrder.resize(kMaxLights);
            }
            gpuLights.clear();
            for (uint32_t index : lightOrder) {
                const PointLight& light = input.lights[index];
                GpuLight gpu;
                gpu.positionRadius = glm::vec4(glm::vec3(light.position - origin), light.radius);
                gpu.colorSize = glm::vec4(light.color * light.intensity, light.sourceRadius);
                gpuLights.push_back(gpu);
            }
            const uint32_t lightCount = Upload<GpuLight>(frame, frame.lights, gpuLights, "lights");

            // --- particles (already relative to the render origin) -------------------------------
            data.particleCount = Upload<fx::GpuParticle>(frame, frame.particles, input.particles, "particles");

            // --- uniforms ------------------------------------------------------------------------
            int features = 0;
            if (settings.sunShadows) features |= Feature_SunShadows;
            if (settings.ambientOcclusion) features |= Feature_AmbientOcclusion;
            if (settings.emissiveLighting) features |= Feature_EmissiveLight;
            if (settings.lightShadows) features |= Feature_LightShadows;
            if (settings.taa) features |= Feature_Taa;
            if (settings.fog) features |= Feature_Fog;
            if (settings.spatialFilter) features |= Feature_SpatialFilter;
            u.counts = glm::ivec4(static_cast<int>(instanceCount), static_cast<int>(bvhCount), static_cast<int>(lightCount),
                                  static_cast<int>(frameIndex & 0x7FFFFFFFu));
            u.settings = glm::ivec4(features, static_cast<int>(settings.debugView), static_cast<int>(frontCount), 0);
            std::memcpy(frame.uniforms.mapped, &u, sizeof(u));
            Flush(frame.uniforms);

            prevJitter = jitter;
            stats.instancesTotal = static_cast<uint32_t>(input.instances.size());
            stats.instancesDrawn = frontCount + insideCount;
            stats.lights = lightCount;
            stats.particles = data.particleCount;
            return data;
        }

        // -----------------------------------------------------------------------------------------
        //  Descriptor binding
        // -----------------------------------------------------------------------------------------
        const Buffer* FindBuffer(const FrameContext& frame, std::string_view name) const {
            if (name == "u_Frame") return &frame.uniforms;
            if (name == "u_Grid") return &voxels.grid;
            if (name == "u_Occ") return &voxels.occ;
            if (name == "u_Ids") return &voxels.ids;
            if (name == "u_Materials") return &voxels.materials;
            if (name == "u_Instances") return &frame.instances;
            if (name == "u_Bvh") return &frame.bvh;
            if (name == "u_DrawList") return &frame.drawList;
            if (name == "u_Lights") return &frame.lights;
            if (name == "u_Particles") return &frame.particles;
            return nullptr;
        }

        /**
         * Supplies every binding the pipeline reflects (by name), binds pipeline + set and sets
         * viewport / scissor. Returns false (nothing may be drawn) if anything is missing.
         */
        bool BindPipeline(FrameContext& frame, PipelineId id, VkExtent2D extent, std::span<const TextureBinding> textures) {
            const Pipeline& pipeline = GetPipeline(id);
            if (!pipeline.IsValid()) return false;

            DescriptorWriter writer(*device, frame.descriptorPool, pipeline);
            bool complete = true;
            for (const ShaderBinding& binding : pipeline.Bindings()) {
                bool found = false;
                if (binding.type == BindingType::UniformBuffer || binding.type == BindingType::StorageBuffer) {
                    if (const Buffer* buffer = FindBuffer(frame, binding.name); buffer != nullptr && *buffer) {
                        writer.Buffer(binding.name, *buffer);
                        found = true;
                    }
                } else if (binding.type == BindingType::CombinedImageSampler) {
                    for (const TextureBinding& texture : textures) {
                        if (texture.name == binding.name) {
                            writer.Texture(binding.name, texture.view, texture.sampler);
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    complete = false;
                    if (ReportOnce(std::format("binding/{}/{}", pipeline.Name(), binding.name))) {
                        Log::Error("Renderer: pipeline '{}' declares binding {} '{}' which the renderer does not provide",
                                   pipeline.Name(), binding.binding, binding.name);
                    }
                }
            }
            if (!complete || !writer.Bind(frame.cmd)) return false;
            SetViewportScissor(frame.cmd, extent);
            return true;
        }

        // -----------------------------------------------------------------------------------------
        //  Passes
        // -----------------------------------------------------------------------------------------
        void PassGBuffer(FrameContext& frame, const FrameData& data) {
            const VkCommandBuffer cmd = frame.cmd;
            const uint32_t cur = parity;
            BeginPass(frame, Pass::GBuffer);
            TransitionImage(cmd, targets.gMaterial, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            TransitionImage(cmd, targets.gNormal[cur], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            TransitionImage(cmd, targets.gVelocity, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            TransitionImage(cmd, targets.depth[cur], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

            const std::array<VkRenderingAttachmentInfo, 3> colors = {
                ColorClear(targets.gMaterial, ClearUint(0)),
                ColorClear(targets.gNormal[cur], kClearNormal),
                ColorClear(targets.gVelocity, kClearZero),
            };
            const VkRenderingAttachmentInfo depth = Attachment(targets.depth[cur].view, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                                               VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ClearDepth(0.0f));
            BeginRendering(cmd, targets.render, colors, &depth);
            if (data.drawWorld && BindPipeline(frame, PipelineId::GBufferWorld, targets.render, {})) {
                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
            if (data.frontCount != 0 && BindPipeline(frame, PipelineId::GBufferObjectFront, targets.render, {})) {
                vkCmdDraw(cmd, 36, data.frontCount, 0, 0);
            }
            if (data.insideCount != 0 && BindPipeline(frame, PipelineId::GBufferObjectInside, targets.render, {})) {
                vkCmdDraw(cmd, 36, data.insideCount, 0, data.frontCount);
            }
            vkCmdEndRendering(cmd);
            EndPass(frame, Pass::GBuffer);
        }

        void PassLighting(FrameContext& frame) {
            const VkCommandBuffer cmd = frame.cmd;
            const uint32_t cur = parity;
            const uint32_t prev = 1 - parity;
            BeginPass(frame, Pass::Lighting);
            TransitionImage(cmd, targets.gMaterial, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.gNormal[cur], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.gVelocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.depth[cur], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.lighting[prev], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.lightCount[prev], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.depth[prev], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.gNormal[prev], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.lighting[cur], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            TransitionImage(cmd, targets.lightCount[cur], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            const std::array<VkRenderingAttachmentInfo, 2> colors = {
                ColorClear(targets.lighting[cur], kClearZero),
                ColorClear(targets.lightCount[cur], kClearZero),
            };
            const std::array<TextureBinding, 8> textures = {
                TextureBinding{"u_GMaterial", targets.gMaterial.view, nearestSampler},
                TextureBinding{"u_GNormal", targets.gNormal[cur].view, nearestSampler},
                TextureBinding{"u_GDepth", targets.depth[cur].view, nearestSampler},
                TextureBinding{"u_GVelocity", targets.gVelocity.view, nearestSampler},
                TextureBinding{"u_PrevLighting", targets.lighting[prev].view, linearSampler},
                TextureBinding{"u_PrevLightCount", targets.lightCount[prev].view, linearSampler},
                TextureBinding{"u_PrevDepth", targets.depth[prev].view, nearestSampler},
                TextureBinding{"u_PrevNormal", targets.gNormal[prev].view, nearestSampler},
            };
            BeginRendering(cmd, targets.render, colors, nullptr);
            if (BindPipeline(frame, PipelineId::Lighting, targets.render, textures)) vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            EndPass(frame, Pass::Lighting);
        }

        void PassComposite(FrameContext& frame) {
            const VkCommandBuffer cmd = frame.cmd;
            const uint32_t cur = parity;
            BeginPass(frame, Pass::Composite);
            TransitionImage(cmd, targets.lighting[cur], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.lightCount[cur], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.hdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            const std::array<VkRenderingAttachmentInfo, 1> colors = {ColorClear(targets.hdr, kClearZero)};
            const std::array<TextureBinding, 5> textures = {
                TextureBinding{"u_GMaterial", targets.gMaterial.view, nearestSampler},
                TextureBinding{"u_GNormal", targets.gNormal[cur].view, nearestSampler},
                TextureBinding{"u_GDepth", targets.depth[cur].view, nearestSampler},
                TextureBinding{"u_Lighting", targets.lighting[cur].view, nearestSampler},
                TextureBinding{"u_LightCount", targets.lightCount[cur].view, nearestSampler},
            };
            BeginRendering(cmd, targets.render, colors, nullptr);
            if (BindPipeline(frame, PipelineId::Composite, targets.render, textures)) vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            EndPass(frame, Pass::Composite);
        }

        void PassParticles(FrameContext& frame, const FrameData& data) {
            const VkCommandBuffer cmd = frame.cmd;
            const uint32_t cur = parity;
            BeginPass(frame, Pass::Particles);
            if (data.particleCount != 0) {
                TransitionImage(cmd, targets.hdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                TransitionImage(cmd, targets.depth[cur], VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
                const std::array<VkRenderingAttachmentInfo, 1> colors = {
                    Attachment(targets.hdr.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                               VK_ATTACHMENT_STORE_OP_STORE, kClearZero),
                };
                // Read-only depth: tested, never written, so nothing has to be stored.
                const VkRenderingAttachmentInfo depth = Attachment(targets.depth[cur].view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                                                                   VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_NONE, ClearDepth(0.0f));
                BeginRendering(cmd, targets.render, colors, &depth);
                if (BindPipeline(frame, PipelineId::Particles, targets.render, {})) vkCmdDraw(cmd, 6, data.particleCount, 0, 0);
                vkCmdEndRendering(cmd);
            }
            EndPass(frame, Pass::Particles);
        }

        void PassTaa(FrameContext& frame) {
            const VkCommandBuffer cmd = frame.cmd;
            const uint32_t cur = parity;
            const uint32_t prev = 1 - parity;
            BeginPass(frame, Pass::Taa);
            TransitionImage(cmd, targets.hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.taa[prev], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.gVelocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.depth[cur], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            TransitionImage(cmd, targets.taa[cur], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            const std::array<VkRenderingAttachmentInfo, 1> colors = {ColorClear(targets.taa[cur], kClearZero)};
            const std::array<TextureBinding, 4> textures = {
                TextureBinding{"u_Color", targets.hdr.view, nearestSampler},
                TextureBinding{"u_History", targets.taa[prev].view, linearSampler},
                TextureBinding{"u_GVelocity", targets.gVelocity.view, nearestSampler},
                TextureBinding{"u_GDepth", targets.depth[cur].view, nearestSampler},
            };
            BeginRendering(cmd, targets.render, colors, nullptr);
            if (BindPipeline(frame, PipelineId::Taa, targets.render, textures)) vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            EndPass(frame, Pass::Taa);
        }

        /// The backbuffer must already be in COLOR_ATTACHMENT_OPTIMAL.
        void PassPresent(FrameContext& frame, VkImageView backbufferView) {
            const VkCommandBuffer cmd = frame.cmd;
            const uint32_t cur = parity;
            BeginPass(frame, Pass::Present);
            TransitionImage(cmd, targets.taa[cur], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            const std::array<VkRenderingAttachmentInfo, 1> colors = {
                Attachment(backbufferView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                           VK_ATTACHMENT_STORE_OP_STORE, ClearFloat(0.0f, 0.0f, 0.0f, 1.0f)),
            };
            const std::array<TextureBinding, 1> textures = {TextureBinding{"u_Color", targets.taa[cur].view, linearSampler}};
            BeginRendering(cmd, targets.output, colors, nullptr);
            if (BindPipeline(frame, PipelineId::Present, targets.output, textures)) vkCmdDraw(cmd, 3, 1, 0, 0);
            if (ui != nullptr) {
                SetViewportScissor(cmd, targets.output);
                ui->Draw(cmd);
            }
            vkCmdEndRendering(cmd);
            EndPass(frame, Pass::Present);
        }

        // -----------------------------------------------------------------------------------------
        //  Screenshot
        // -----------------------------------------------------------------------------------------
        void RecordScreenshotCopy(VkCommandBuffer cmd, VkImage backbuffer, const Buffer& readback) {
            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = VkOffset3D{0, 0, 0};
            region.imageExtent = VkExtent3D{targets.output.width, targets.output.height, 1};
            vkCmdCopyImageToBuffer(cmd, backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &region);
        }

        /// The frame that filled `readback` must have finished executing.
        void WriteScreenshot(const Buffer& readback) {
            const uint32_t width = targets.output.width;
            const uint32_t height = targets.output.height;
            const size_t bytes = static_cast<size_t>(width) * height * 4;
            vmaInvalidateAllocation(device->Allocator(), readback.allocation, 0, VK_WHOLE_SIZE);

            std::vector<uint8_t> pixels(bytes);
            std::memcpy(pixels.data(), readback.mapped, bytes);
            const bool bgra = backbufferFormat == VK_FORMAT_B8G8R8A8_UNORM || backbufferFormat == VK_FORMAT_B8G8R8A8_SRGB;
            for (size_t i = 0; i < bytes; i += 4) {
                if (bgra) std::swap(pixels[i], pixels[i + 2]);
                pixels[i + 3] = 255;
            }

            std::error_code ec;
            if (screenshotPath.has_parent_path()) std::filesystem::create_directories(screenshotPath.parent_path(), ec);
            const std::string file = screenshotPath.string();
            if (stbi_write_png(file.c_str(), static_cast<int>(width), static_cast<int>(height), 4, pixels.data(),
                               static_cast<int>(width * 4)) != 0) {
                Log::Info("Screenshot saved: {} ({}x{})", file, width, height);
            } else {
                Log::Error("Screenshot: writing '{}' failed", file);
            }
        }

        // -----------------------------------------------------------------------------------------
        //  Frame
        // -----------------------------------------------------------------------------------------
        void UpdateMemoryStats() {
            uint64_t bytes = voxels.occ.size + voxels.ids.size + voxels.grid.size + voxels.materials.size;
            for (const FrameContext& frame : frames) {
                bytes += frame.uniforms.size + frame.instances.size + frame.bvh.size + frame.drawList.size + frame.lights.size +
                         frame.particles.size + frame.staging.size;
            }
            targets.ForEach([&bytes](Image& image) {
                if (image) bytes += static_cast<uint64_t>(image.extent.width) * image.extent.height * BytesPerPixel(image.format);
            });
            stats.gpuMemoryBytes = bytes;
        }

        /// A recorded frame was not executed: resynchronise everything the recording assumed.
        void RecoverFailedSubmit(FrameContext& frame, const FrameInput& input, Buffer& readback) {
            device->WaitIdle();
            const VkDevice dev = device->Handle();
            DestroyBuffer(*device, readback);
            // The fence may be unsignalled and the acquire semaphore signalled: replace both.
            if (frame.fence != VK_NULL_HANDLE) vkDestroyFence(dev, frame.fence, nullptr);
            if (frame.imageAcquired != VK_NULL_HANDLE) vkDestroySemaphore(dev, frame.imageAcquired, nullptr);
            frame.fence = VK_NULL_HANDLE;
            frame.imageAcquired = VK_NULL_HANDLE;
            if (!CreateSyncObjects(frame)) Log::Error("Renderer: recreating frame synchronisation objects failed");
            swapchainDirty = !headless;
            InvalidateVoxelMirror(*input.store);
            RecreateTargets();   // tracked image layouts no longer match the GPU
        }

        bool RenderFrame(const FrameInput& input) {
            if (!initialized) return false;
            if (input.store == nullptr || input.materials == nullptr) {
                if (ReportOnce("frame-input")) Log::Error("Renderer: FrameInput::store and FrameInput::materials are required");
                return false;
            }
            ProfileScope profile("Renderer::RenderFrame");
            if (!PrepareSurface()) return false;

            const VkDevice dev = device->Handle();
            FrameContext& frame = frames[frameSlot];
            if (frame.fence == VK_NULL_HANDLE || frame.imageAcquired == VK_NULL_HANDLE) {
                if (!CreateSyncObjects(frame)) return false;
            }
            if (vkWaitForFences(dev, 1, &frame.fence, VK_TRUE, std::numeric_limits<uint64_t>::max()) != VK_SUCCESS) {
                Log::Error("Renderer: waiting for the frame fence failed");
                return false;
            }
            // Nothing recorded by this slot is in use any more.
            frame.deletions.Flush();
            CollectTimings(frame);

            if (vkResetCommandPool(dev, frame.commandPool, 0) != VK_SUCCESS ||
                vkResetDescriptorPool(dev, frame.descriptorPool, 0) != VK_SUCCESS) {
                Log::Error("Renderer: resetting the frame pools failed");
                return false;
            }
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(frame.cmd, &beginInfo) != VK_SUCCESS) {
                Log::Error("Renderer: beginning the command buffer failed");
                return false;
            }
            const VkCommandBuffer cmd = frame.cmd;

            // Acquire after everything that can fail without side effects: an unsubmitted command
            // buffer is simply reset next time, an acquired image would have to be presented.
            uint32_t imageIndex = 0;
            if (!headless && !swapchain.Acquire(frame.imageAcquired, imageIndex)) {
                RecreateSurface();
                return false;
            }

            if (frame.queryPool != VK_NULL_HANDLE) vkCmdResetQueryPool(cmd, frame.queryPool, 0, kQueryCount);

            SyncVoxelData(frame, input);
            if (targets.needsClear) ClearHistoryTargets(cmd);

            const Camera camera = SanitizeCamera(input.camera);
            const FrameData data = BuildFrameData(frame, input, camera);

            PassGBuffer(frame, data);
            PassLighting(frame);
            PassComposite(frame);
            PassParticles(frame, data);
            PassTaa(frame);

            // --- present pass + screenshot ---------------------------------------------------------
            Buffer readback;
            if (screenshotRequested) {
                readback = CreateBuffer(*device, static_cast<VkDeviceSize>(targets.output.width) * targets.output.height * 4, 0,
                                        BufferKind::Readback, "Screenshot readback");
                if (!readback) {
                    Log::Error("Screenshot: the readback buffer could not be created");
                    screenshotRequested = false;
                }
            }
            if (headless) {
                TransitionImage(cmd, targets.backbuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                PassPresent(frame, targets.backbuffer.view);
                if (readback) {
                    TransitionImage(cmd, targets.backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    RecordScreenshotCopy(cmd, targets.backbuffer.handle, readback);
                }
            } else {
                const VkImage image = swapchain.Image(imageIndex);
                VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                TransitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, layout);
                PassPresent(frame, swapchain.View(imageIndex));
                if (readback) {
                    TransitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    RecordScreenshotCopy(cmd, image, readback);
                }
                TransitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            }

            // --- submit ----------------------------------------------------------------------------
            if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
                Log::Error("Renderer: ending the command buffer failed");
                RecoverFailedSubmit(frame, input, readback);
                return false;
            }
            VkCommandBufferSubmitInfo commandInfo{};
            commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            commandInfo.commandBuffer = cmd;

            // The whole command buffer waits for the image: its first use is a layout transition,
            // which is not tied to the colour attachment stage.
            VkSemaphoreSubmitInfo waitInfo{};
            waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waitInfo.semaphore = frame.imageAcquired;
            waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSemaphoreSubmitInfo signalInfo{};
            signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signalInfo.semaphore = headless ? VK_NULL_HANDLE : renderFinished[imageIndex];
            signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

            VkSubmitInfo2 submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &commandInfo;
            if (!headless) {
                submit.waitSemaphoreInfoCount = 1;
                submit.pWaitSemaphoreInfos = &waitInfo;
                submit.signalSemaphoreInfoCount = 1;
                submit.pSignalSemaphoreInfos = &signalInfo;
            }
            vkResetFences(dev, 1, &frame.fence);
            const VkResult submitted = vkQueueSubmit2(device->GraphicsQueue(), 1, &submit, frame.fence);
            if (submitted != VK_SUCCESS) {
                Log::Error("Renderer: queue submission failed (VkResult {})", static_cast<int>(submitted));
                RecoverFailedSubmit(frame, input, readback);
                return false;
            }
            frame.queriesPending = frame.queryPool != VK_NULL_HANDLE;

            if (!headless && !swapchain.Present(renderFinished[imageIndex], imageIndex)) {
                swapchainDirty = true;   // recreated at the start of the next frame
            }

            if (readback) {
                if (vkWaitForFences(dev, 1, &frame.fence, VK_TRUE, std::numeric_limits<uint64_t>::max()) == VK_SUCCESS) {
                    WriteScreenshot(readback);
                } else {
                    Log::Error("Screenshot: waiting for the frame failed");
                }
                DestroyBuffer(*device, readback);   // the frame has finished (or the device is lost)
                screenshotRequested = false;
            }

            // --- bookkeeping -----------------------------------------------------------------------
            prevCamera = camera;
            prevAspect = static_cast<float>(targets.render.width) / static_cast<float>(targets.render.height);
            historyValid = true;
            parity = 1 - parity;
            frameSlot = (frameSlot + 1) % kFramesInFlight;

            stats.renderSize = glm::uvec2(targets.render.width, targets.render.height);
            stats.outputSize = glm::uvec2(targets.output.width, targets.output.height);
            stats.frameIndex = frameIndex;
            UpdateMemoryStats();
            ++frameIndex;
            return true;
        }

        // -----------------------------------------------------------------------------------------
        //  Lifetime
        // -----------------------------------------------------------------------------------------
        bool Initialize(Device& dev, Window* win, uint32_t width, uint32_t height, uint32_t chunkCapacity, const RenderSettings& initial) {
            if (initialized || device != nullptr) {
                Log::Error("Renderer::Initialize: already initialised");
                return false;
            }
            if (!dev.IsInitialized()) {
                Log::Error("Renderer::Initialize: the device is not initialised");
                return false;
            }
            if (win != nullptr && dev.IsHeadless()) {
                Log::Error("Renderer::Initialize: a window was given but the device was created headless");
                return false;
            }
            device = &dev;
            window = win;
            headless = win == nullptr;
            settings = initial;
            stats = RenderStats{};
            for (uint32_t i = 0; i < kPassCount; ++i) {
                stats.passes.push_back(PassTiming{kPassNames[i], 0.0f});
                profilerNames[i] = std::format("GPU: {}", kPassNames[i]);
            }
            frameSlot = 0;
            frameIndex = 0;
            parity = 0;
            historyValid = false;
            swapchainDirty = false;
            screenshotRequested = false;
            reported.clear();

            if (headless) {
                outputExtent = VkExtent2D{width, height};
                backbufferFormat = kHeadlessFormat;
            } else {
                const glm::ivec2 size = win->FramebufferSize();
                const uint32_t w = size.x > 0 ? static_cast<uint32_t>(size.x) : width;
                const uint32_t h = size.y > 0 ? static_cast<uint32_t>(size.y) : height;
                if (w == 0 || h == 0 || !swapchain.Create(dev, w, h, settings.vsync)) {
                    Log::Error("Renderer::Initialize: the swapchain could not be created");
                    return false;
                }
                appliedVsync = settings.vsync;
                outputExtent = swapchain.Extent();
                backbufferFormat = swapchain.Format();
                if (!CreateRenderFinished()) return false;
            }
            if (outputExtent.width == 0 || outputExtent.height == 0) {
                Log::Error("Renderer::Initialize: the output size must not be zero");
                return false;
            }

            nearestSampler = CreateSampler(dev, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
            linearSampler = CreateSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
            if (nearestSampler == VK_NULL_HANDLE || linearSampler == VK_NULL_HANDLE) return false;

            DetectTimestampSupport();
            if (!CreateFrames()) {
                Log::Error("Renderer::Initialize: the per-frame resources could not be created");
                return false;
            }
            if (!CreateVoxelStore(chunkCapacity)) {
                Log::Error("Renderer::Initialize: the GPU voxel store could not be created ({} chunk slots)", chunkCapacity);
                return false;
            }
            if (!RecreateTargets()) return false;
            pipelines = BuildPipelines(backbufferFormat);
            if (!pipelines) return false;

            initialized = true;
            UpdateMemoryStats();
            stats.renderSize = glm::uvec2(targets.render.width, targets.render.height);
            stats.outputSize = glm::uvec2(targets.output.width, targets.output.height);
            Log::Info("Renderer: {} mode, output {}x{}, render {}x{}, {} chunk slots, {:.1f} MiB GPU memory",
                      headless ? "headless" : "windowed", outputExtent.width, outputExtent.height, targets.render.width,
                      targets.render.height, chunkCapacity, static_cast<double>(stats.gpuMemoryBytes) / (1024.0 * 1024.0));
            return true;
        }

        void Shutdown() {
            if (device == nullptr) return;
            const bool deviceAlive = device->IsInitialized() && device->Handle() != VK_NULL_HANDLE;
            if (deviceAlive) device->WaitIdle();

            // Reverse order of creation.
            pipelines.reset();
            DestroyTargets();
            DestroyVoxelStore();
            DestroyFrames(deviceAlive);
            if (deviceAlive) {
                if (linearSampler != VK_NULL_HANDLE) vkDestroySampler(device->Handle(), linearSampler, nullptr);
                if (nearestSampler != VK_NULL_HANDLE) vkDestroySampler(device->Handle(), nearestSampler, nullptr);
            }
            linearSampler = VK_NULL_HANDLE;
            nearestSampler = VK_NULL_HANDLE;
            DestroyRenderFinished();
            swapchain.Destroy();

            initialized = false;
            device = nullptr;
            window = nullptr;
            ui = nullptr;
            historyValid = false;
            screenshotRequested = false;
            outputExtent = VkExtent2D{0, 0};
        }
    };

    // =============================================================================================
    //  Renderer
    // =============================================================================================
    Renderer::Renderer() : m_Impl(std::make_unique<Impl>()) {}

    Renderer::~Renderer() { Shutdown(); }

    bool Renderer::Initialize(Device& device, Window* window, uint32_t width, uint32_t height, uint32_t chunkCapacity,
                              const RenderSettings& settings) {
        if (m_Impl->Initialize(device, window, width, height, chunkCapacity, settings)) return true;
        if (!m_Impl->initialized) m_Impl->Shutdown();   // release whatever a failed start-up created
        return false;
    }

    void Renderer::Shutdown() { m_Impl->Shutdown(); }

    bool Renderer::IsInitialized() const { return m_Impl->initialized; }

    RenderSettings& Renderer::Settings() { return m_Impl->settings; }

    const RenderStats& Renderer::Stats() const { return m_Impl->stats; }

    void Renderer::SetUiLayer(UiLayer* ui) { m_Impl->ui = ui; }

    bool Renderer::RenderFrame(const FrameInput& input) { return m_Impl->RenderFrame(input); }

    void Renderer::RequestScreenshot(const std::filesystem::path& file) {
        if (file.empty()) {
            Log::Warn("Renderer::RequestScreenshot: empty file name");
            return;
        }
        m_Impl->screenshotPath = file;
        m_Impl->screenshotRequested = true;
    }

    bool Renderer::ReloadShaders() { return m_Impl->ReloadShaders(); }

    VkFormat Renderer::BackbufferFormat() const { return m_Impl->backbufferFormat; }

    uint32_t Renderer::BackbufferImageCount() const {
        return (m_Impl->headless || !m_Impl->swapchain.IsValid()) ? 1u : m_Impl->swapchain.ImageCount();
    }

    glm::uvec2 Renderer::OutputSize() const { return {m_Impl->outputExtent.width, m_Impl->outputExtent.height}; }
}
