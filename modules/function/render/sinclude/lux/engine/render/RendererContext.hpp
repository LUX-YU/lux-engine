#pragma once
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/resources/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/FrameRetireScheduler.hpp>
#include <lux/engine/render/resources/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T *;

namespace lux::render
{
    class PipelineManager;
    class GeneralDescriptorSetLayout;
    class ShaderPermutationCompiler;
    class DescriptorService;
    class PipelineLayoutService;

    // Return structs for the external-memory interop helpers (defined in the public
    // RenderContextView.hpp; forward-declared here so this header stays facade-free).
    struct ExportableBuffer;
    struct ExportableTimelineSemaphore;

    struct SyncManifestSummary
    {
        uint32_t resource_types{0};
        uint32_t features{0};
        uint32_t commands{0};
        uint32_t resource_descriptions{0};
    };

    /// @brief Render infrastructure hub — owns all global render services.
    ///        Constructed via CreateInfo; passed as shared_ptr to Renderer
    ///        and RenderScene instances.
    ///
    /// Owns: PipelineManager, GeneralDescriptorSetLayout, GPUResourceRegistry,
    ///       ShaderPermutationCompiler.
    class LUX_FUNCTION_PUBLIC RenderContext
    {
    public:
        struct CreateInfo
        {
            std::unique_ptr<PipelineManager>            pipeline_mgr;
            std::unique_ptr<GeneralDescriptorSetLayout> descriptor_layouts;
            std::unique_ptr<GlobalResourceRegistry>     global_resources;
            std::unique_ptr<ShaderPermutationCompiler>  permutation_compiler;
            uint32_t                                    frames_in_flight{2};
        };

        explicit RenderContext(ResourceContext &res_ctx, CreateInfo info);
        ~RenderContext();

        RenderContext(const RenderContext &) = delete;
        RenderContext &operator=(const RenderContext &) = delete;

        // ── Vulkan infrastructure ──
        [[nodiscard]] ResourceContext&
        resourceContext() noexcept { return resource_ctx_; }

        [[nodiscard]] DeviceContext&
        deviceContext() noexcept;

        [[nodiscard]] VkDevice 
        device() const noexcept;

        [[nodiscard]] VmaAllocator 
        vmaAllocator() const noexcept;

        // ── Pipeline / descriptor infrastructure ──

        [[nodiscard]] PipelineManager&
        pipelineManager() noexcept { return *pipeline_mgr_; }

        [[nodiscard]] GeneralDescriptorSetLayout&
        descriptorLayouts() noexcept { return *descriptor_layouts_; }

        [[nodiscard]] DescriptorService&
        descriptorService() noexcept { return *descriptor_service_; }

        [[nodiscard]] PipelineLayoutService&
        pipelineLayoutService() noexcept { return *pipeline_layout_service_; }

        // ── Global resource registry ──

        [[nodiscard]] GlobalResourceRegistry&
        globalRegistry() noexcept { return *global_registry_; }

        [[nodiscard]] const GlobalResourceRegistry&
        globalRegistry() const noexcept { return *global_registry_; }

        // ── Configuration ──

        [[nodiscard]] uint32_t 
        framesInFlight() const noexcept { return frames_in_flight_; }

        [[nodiscard]] DeferredDestroyQueue&
        deferredDestroyQueue() noexcept { return deferred_destroy_queue_; }

        [[nodiscard]] FrameRetireScheduler&
        retireScheduler() noexcept { return retire_scheduler_; }

        // ── External-memory interop backing (impl in RenderContext.cpp) ──
        // Domain-neutral: dedicated, VMA-bypassing exportable allocations for
        // CUDA<->Vulkan zero-copy. Surfaced to features via RenderContextView.
        [[nodiscard]] bool        supportsExternalMemory() const noexcept;
        void                      deviceUUID(uint8_t out[16]) const noexcept;
        [[nodiscard]] uint32_t    findMemoryTypeIndex(uint32_t type_filter, uint32_t property_flags) const noexcept;
        ExportableBuffer            createExportableBuffer(uint64_t size, uint32_t usage_flags);
        ExportableTimelineSemaphore createExportableTimelineSemaphore();

    private:
        ResourceContext&                            resource_ctx_;

        // Destruction order: reverse of declaration.
        // deferred_destroy_queue_ is declared FIRST so it is destroyed LAST —
        // after every GPU-resource holder (global_registry_, per-... ) whose
        // FifOwned members retire into it during their own teardown. Its
        // destructor flushAll()s, so handles retired during member teardown are
        // actually destroyed. (C1 — makes the queue the owner-of-last-resort.)
        // permutation_compiler_ must outlive pipeline_mgr_ (raw-pointer dep).
        // pipeline_layout_service_ must outlive pipeline_mgr_ because pipelines
        // can reference layouts returned by the service.
        DeferredDestroyQueue                        deferred_destroy_queue_;
        std::unique_ptr<GeneralDescriptorSetLayout> descriptor_layouts_;
        std::unique_ptr<DescriptorService>          descriptor_service_;
        std::unique_ptr<PipelineLayoutService>      pipeline_layout_service_;
        std::unique_ptr<ShaderPermutationCompiler>  permutation_compiler_;
        std::unique_ptr<PipelineManager>            pipeline_mgr_;
        std::unique_ptr<GlobalResourceRegistry>     global_registry_;

        uint32_t                                    frames_in_flight_;
         ///< Cached: high-frequency access, zero-cost uint64 cache
        VkDevice                                    vk_device_{VK_NULL_HANDLE};
        FrameRetireScheduler                        retire_scheduler_;
    };

} // namespace lux::render
