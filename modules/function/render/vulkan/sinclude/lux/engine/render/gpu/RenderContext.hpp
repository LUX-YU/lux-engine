#pragma once
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/core/FrameRetireScheduler.hpp>
#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>
#include <lux/engine/render/gpu/ExternalInterop.hpp>   // 外部内存互操作的返回结构(同层)
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/resource/deployment/RuntimeCapacity.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>   // 自发上报汇集器(非拥有)
#include <lux/engine/render/resources/material/TextureSamplingRepresentationCatalog.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T *;

namespace lux::render
{
    class PipelineManager;
    class GeneralDescriptorSetLayout;
    class DescriptorService;
    class PipelineLayoutService;

    // (原此处前置声明 ExportableBuffer / ExportableTimelineSemaphore,附一句
    //  "defined in the public RenderContextView.hpp"。那句话在它们下沉 L0 时就
    //  过期了,却没人会发现 —— 前置声明**不校验**定义在哪,注释是唯一的指路牌,
    //  而指路牌指错了也不会有任何构建信号。现在它们归位 gpu/ExternalInterop.hpp,
    //  与本头同层,直接 include 即可:定义位置由编译器保证,不再需要指路牌。)

    struct SyncManifestSummary
    {
        uint32_t resource_types{0};
        uint32_t features{0};
        uint32_t commands{0};
        uint32_t resource_descriptions{0};
    };

    /// @brief Render infrastructure hub — owns all global render services.
    ///        Created via create(); passed as shared_ptr to Renderer
    ///        and RenderScene instances.
    ///
    /// Owns: PipelineManager, GeneralDescriptorSetLayout, GPUResourceRegistry.
    class LUX_FUNCTION_PUBLIC RenderContext
    {
        struct ConstructionKey final {};

    public:
        struct CreateInfo
        {
            std::unique_ptr<PipelineManager>            pipeline_mgr;
            std::unique_ptr<GeneralDescriptorSetLayout> descriptor_layouts;
            std::unique_ptr<ResourceRegistry>     global_resources;
            uint32_t                                    frames_in_flight{2};
            lux::deployment::RuntimeCapacityPlan capacity_plan{};
        };

        [[nodiscard]] static Expected<std::shared_ptr<RenderContext>> create(
            ResourceContext& res_ctx,
            CreateInfo info
        );

        /// Public only so std::make_shared can construct the control block and
        /// object together. ConstructionKey is private, so create() remains the
        /// sole source-level construction boundary.
        explicit RenderContext(
            ConstructionKey,
            ResourceContext& res_ctx,
            CreateInfo info
        );

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

        [[nodiscard]] ResourceRegistry&
        globalRegistry() noexcept { return *global_registry_; }

        [[nodiscard]] const ResourceRegistry&
        globalRegistry() const noexcept { return *global_registry_; }

        /// Process-global upload scheduling is backend infrastructure. Feature
        /// resources register type-erased contributors when they are installed;
        /// the renderer only drives this scheduler and never names those types.
        [[nodiscard]] TransferScheduler&
        globalTransferScheduler() noexcept { return global_transfer_scheduler_; }

        // ── Configuration ──

        [[nodiscard]] uint32_t
        framesInFlight() const noexcept { return frames_in_flight_; }

        [[nodiscard]] const lux::deployment::RuntimeCapacityPlan&
        capacityPlan() const noexcept { return capacity_plan_; }

        [[nodiscard]] const TextureSamplingRepresentationCatalog&
        textureSamplingRepresentations() const noexcept
        {
            return texture_sampling_catalog_;
        }

        [[nodiscard]] DeferredDestroyQueue&
        deferredDestroyQueue() noexcept { return deferred_destroy_queue_; }

        [[nodiscard]] FrameRetireScheduler&
        retireScheduler() noexcept { return retire_scheduler_; }

        // ── 自发上报 ──
        //
        // 渲染线程上有一类失败**没有调用方可以交待**:一帧已经在飞了(变体预算耗尽、
        // 骨骼池满、顶点池注册表满),或者根本不是谁请求的。它们此前一律写 stderr,
        // 于是渲染库替上层决定了诊断出现在哪个终端,编辑器想显示在自己的面板里则
        // 无从下手。汇集器由服务器在 init 时装进来;未装(纯 RenderContext 的单元
        // 测试)时上报是无害的空操作。

        /// 装上汇集器,并转交给本上下文拥有的、自己也要上报的服务(管线管理器)。
        void setErrorSink(RenderErrorSink* sink) noexcept;

        /// 供那些自己持有汇集器指针的资源类取用(它们不该为一次上报就依赖整个
        /// RenderContext)。未接线时为 null,资源侧按 null 静默跳过。
        [[nodiscard]] RenderErrorSink* errorSink() const noexcept { return error_sink_; }

        /// 记一条自发上报。同键(场景 + 错误类型 + 实参)在同一批内合并计数,
        /// **所以每帧调用是安全的** —— 限流是通道的职责,失败点不必各写一份。
        void reportError(const RenderError& error,
                         std::uint32_t scene_index = RenderErrorEvent::kNoScene,
                         std::uint64_t frame_serial = 0) noexcept
        {
            if (error_sink_ != nullptr)
                error_sink_->emit(error, scene_index, frame_serial);
        }

        // ── External-memory interop backing (impl in RenderContext.cpp) ──
        // Domain-neutral: dedicated, VMA-bypassing exportable allocations for
        // CUDA<->Vulkan zero-copy. Surfaced to features via RenderContextView.
        [[nodiscard]] bool        supportsExternalMemory() const noexcept;
        void                      deviceUUID(uint8_t out[16]) const noexcept;
        [[nodiscard]] uint32_t    findMemoryTypeIndex(uint32_t type_filter, uint32_t property_flags) const noexcept;
        [[nodiscard]] Expected<ExportableBuffer> createExportableBuffer(
            uint64_t size,
            uint32_t usage_flags
        );
        [[nodiscard]] Expected<ExportableTimelineSemaphore>
        createExportableTimelineSemaphore();

    private:
        ResourceContext&                            resource_ctx_;
        /// 非拥有 —— 服务器持有,生命周期覆盖本上下文。未接线时为 null。
        RenderErrorSink*                            error_sink_{nullptr};

        // Destruction order: reverse of declaration.
        // deferred_destroy_queue_ is declared FIRST so it is destroyed LAST —
        // after every GPU-resource holder (global_registry_, per-... ) whose
        // FifOwned members retire into it during their own teardown. Its
        // destructor flushAll()s, so handles retired during member teardown are
        // actually destroyed. (C1 — makes the queue the owner-of-last-resort.)
        // pipeline_layout_service_ must outlive pipeline_mgr_ because pipelines
        // can reference layouts returned by the service.
        DeferredDestroyQueue                        deferred_destroy_queue_;
        std::unique_ptr<GeneralDescriptorSetLayout> descriptor_layouts_;
        std::unique_ptr<DescriptorService>          descriptor_service_;
        std::unique_ptr<PipelineLayoutService>      pipeline_layout_service_;
        std::unique_ptr<PipelineManager>            pipeline_mgr_;
        std::unique_ptr<ResourceRegistry>     global_registry_;

        uint32_t                                    frames_in_flight_;
        lux::deployment::RuntimeCapacityPlan        capacity_plan_{};
        TextureSamplingRepresentationCatalog       texture_sampling_catalog_;
         ///< Cached: high-frequency access, zero-cost uint64 cache
        VkDevice                                    vk_device_{VK_NULL_HANDLE};
        FrameRetireScheduler                        retire_scheduler_;
        TransferScheduler                           global_transfer_scheduler_;
    };

} // namespace lux::render
