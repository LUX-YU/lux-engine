#pragma once
/**
 * @file RenderContextView.hpp
 * @brief Global render-services facade handed to a RenderFeature.
 *
 * RenderContextView is a NARROW, public facade over the engine-internal RenderContext.
 * `RenderFeature::contextView()` hands a feature one of these, so an (eventually
 * external) feature sees ONLY the curated, Vulkan-handle-level surface below —
 * never the engine-internal RenderContext API (PipelineManager, DescriptorService,
 * ResourceContext, …).
 *
 * The registry accessor returns the PUBLIC base `ResourceRegistry&`, whose
 * `find<T>/emplace<T>/…` are template members instantiated at the CALL site with
 * the feature's OWN resource type — the engine never names an internal type here.
 */

#include <lux/engine/render/gpu/ExternalInterop.hpp> // ExportableBuffer / ExportableTimelineSemaphore(L1,按值返回需完整定义)
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <lux/engine/render/core/vk_fwd.hpp> // VkDevice/VkShaderModule/VkPipelineLayout/VkDescriptorSetLayout
#include <lux/engine/function/render/client/core/PipelineHandle.hpp>      // Graphics/ComputePipelineHandle
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>      // ShaderHandle
#include <lux/engine/function/render/client/core/EngineSetSlot.hpp>       // EDescriptorSetSlot
#include <lux/engine/function/render/client/resources/EBuiltinShader.hpp> // EBuiltinShader
#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp>           // ResourceRegistry
#include <lux/engine/render/core/FrameRetireScheduler.hpp>                // FrameRetireScheduler
#include <lux/engine/render/core/PreparedPipelineStages.hpp>              // preparePipelineStages 的结果
#include <lux/engine/function/render/client/core/Errors.hpp>              // Expected<ShaderHandle>
#include <lux/engine/function/visibility.h>

// VMA handles — same fwd-typedefs the engine uses (core/VmaFwd.hpp), replicated
// here so this public header stays <vulkan/vulkan.h>- and <vk_mem_alloc.h>-free.
struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::rdesc
{
    struct ShaderInfo;
}

namespace lux::render
{
    class RenderContext;             // held subject (forwarded to; defined in an internal header)
    class DeferredDestroyQueue;      // returned by-reference; defined in an internal header
    struct GraphicsPipelineTemplate; // taken by-reference; engine pipeline description

    /// Result of createExportableBuffer — a VMA-BYPASSING dedicated allocation
    /// (vkAllocateMemory + VkExportMemoryAllocateInfo + vkBindBufferMemory) whose
    /// memory can be exported for import by an external API (e.g. CUDA). The caller
    /// OWNS all three handles: retire `buffer` via retireRawBuffer (NOT retireBuffer —
    /// the buffer came from vkCreateBuffer, not VMA) and `memory` via retireDeviceMemory,
    /// and close `external_handle` once the importer has dup'd it.
    /// A successful null result (buffer == 0) means external memory is unsupported;
    /// Vulkan failures are returned through Expected with their exact VkResult.

    /// Result of createExportableTimelineSemaphore — an exportable TIMELINE semaphore
    /// for cross-API (CUDA<->Vulkan) ping-pong sync. Caller owns both; retire the
    /// semaphore via retireSemaphore and close the handle after import.

    class LUX_FUNCTION_PUBLIC RenderContextView
    {
    public:
        /// Wrap a subject. The facade is a non-owning view; @p ctx must outlive it
        /// (it always does — the owning scene outlives every feature it serves).
        explicit RenderContextView(RenderContext& ctx) noexcept : ctx_(&ctx)
        {
        }

        // ── Vulkan infrastructure ───────────────────────────────────────
        [[nodiscard]] VkDevice device() const noexcept;
        [[nodiscard]] VmaAllocator vmaAllocator() const noexcept;
        [[nodiscard]] uint32_t framesInFlight() const noexcept;

        // ── Global resource registry ────────────────────────────────────
        // Returns the public base; find<T>/emplace<T> instantiate on the caller's T.
        [[nodiscard]] ResourceRegistry& globalRegistry() noexcept;
        [[nodiscard]] const ResourceRegistry& globalRegistry() const noexcept;

        // ── Shaders (the engine's ShaderResources stays hidden) ─────────
        /// 解析一个内置着色器(configured 有效则原样返回)。解析不出模块时返回错误并
        /// 指出是哪一个 —— 不再交回一个无效句柄让调用方在取模块时才崩。
        [[nodiscard]] Expected<ShaderHandle>
        createBuiltinShaderModule(EBuiltinShader builtin, ShaderHandle configured = {});

        /// 一个待回填的着色器句柄槽:目标有效则保留,无效则填入内置解析结果。
        struct BuiltinShaderSlot
        {
            EBuiltinShader builtin{};
            ShaderHandle* target{nullptr};
        };

        /// 批量回填一组槽位 —— feature 的 Config 通常有一串 shader 字段,客户端只覆盖
        /// 其中几个。任一槽解析失败即整体报错,不留半填状态。
        [[nodiscard]] Expected<void> createBuiltinShaderModules(std::span<const BuiltinShaderSlot> slots);

        /// 一条管线一个 stage 的请求:内置着色器 id + 调用方可能已配置的覆盖句柄。
        struct PipelineStageDesc
        {
            EBuiltinShader builtin{};
            ShaderHandle configured{};
        };

        /// 解析并切换一条管线的**全部 stage**,一次拿到模块与反射。
        ///
        /// 这是自足 feature 应该用的入口:解析内置默认、域合并切换、取模块反射三件事在
        /// 一次调用里做完,任一失败即整体报错。因为所有内部记录变动都发生在调用之内,
        /// 返回的模块与反射在此之后必然有效 —— 调用方写不出「先取指针再切换」那个顺序。
        [[nodiscard]] Expected<PreparedPipelineStages> preparePipelineStages(std::span<const PipelineStageDesc> stages);
        [[nodiscard]] ShaderHandle
        createShaderModule(std::span<const std::byte> spirv, const lux::rdesc::ShaderInfo& info);
        [[nodiscard]] VkShaderModule shaderModule(ShaderHandle handle) const;
        [[nodiscard]] const lux::rdesc::ShaderInfo* shaderInfo(ShaderHandle handle) const;

        // ── Descriptor-set layouts ──────────────────────────────────────
        /// The engine's pre-built layout for a well-known descriptor slot.
        [[nodiscard]] VkDescriptorSetLayout engineSetLayout(EDescriptorSetSlot slot);

        // ── Pipeline layout + pipeline registration (Expected unwrapped here) ──
        [[nodiscard]] VkPipelineLayout buildStandardGraphicsLayout(
            uint32_t descriptor_set_count,
            std::span<const lux::rdesc::ShaderInfo* const> shader_infos,
            std::string_view debug_name
        );

        [[nodiscard]] GraphicsPipelineHandle registerGraphics(
            const GraphicsPipelineTemplate& description,
            std::span<const lux::rdesc::ShaderInfo* const> shader_infos = {}
        );

        [[nodiscard]] ComputePipelineHandle registerCompute(VkShaderModule shader, VkPipelineLayout layout);

        // ── Feature-owned GPU-resource lifecycle plumbing ───────────────
        [[nodiscard]] DeferredDestroyQueue& deferredDestroyQueue() noexcept;
        [[nodiscard]] FrameRetireScheduler& retireScheduler() noexcept;

        // ── Retire feature-owned GPU resources (FIF-safe deferred destroy) ──
        // Forward to the deferred-destroy queue so a feature can release its own
        // buffers/images without ever naming the concrete DeferredDestroyQueue
        // type. Tagged with the scene's current frame serial; actually destroyed
        // once the GPU has finished the frames that may still reference them.
        void retireBuffer(VkBuffer buffer, VmaAllocation allocation);
        void retireImage(VkImage image, VmaAllocation allocation);
        void retireImageView(VkImageView view);
        void retireSampler(VkSampler sampler);

        // ── External-memory interop (CUDA<->Vulkan zero-copy) ───────────
        // Domain-neutral seam: a feature can allocate an EXPORTABLE buffer + the two
        // TIMELINE semaphores that synchronise an external producer (e.g. a CUDA
        // writer) with the engine's reads, then hand the exported handles out of
        // process-or-thread via its own (downstream) query op. The engine knows
        // nothing of what the buffer holds.
        [[nodiscard]] bool supportsExternalMemory() noexcept;
        /// Copies VK_UUID_SIZE (16) bytes into out[16] so an external API matches its device.
        void deviceUUID(uint8_t out[16]) noexcept;
        /// Forwards to PhysicalDevice::findMemoryTypeIndex; UINT32_MAX on miss.
        [[nodiscard]] uint32_t findMemoryTypeIndex(uint32_t type_filter, uint32_t property_flags) noexcept;

        /// Dedicated, VMA-BYPASSING exportable storage buffer. @p usage_flags is OR'd
        /// with STORAGE_BUFFER|TRANSFER_DST (pass a VkBufferUsageFlags as uint32_t to
        /// keep this header vulkan.h-free; 0 for storage-only). Unsupported is a
        /// successful empty value; Vulkan failures retain their exact VkResult.
        /// See ExportableBuffer for ownership.
        [[nodiscard]] Expected<ExportableBuffer> createExportableBuffer(uint64_t size, uint32_t usage_flags);

        /// Exportable TIMELINE semaphore (initial value 0). Pair TWO of these for the
        /// producer/consumer ping-pong: the external producer signals one when its
        /// write is done (the engine waits on it via addExternalGraphicsWait before
        /// reading), and the engine signals the other when its read is done (the
        /// producer waits on it before reusing the buffer). Strictly monotonic values.
        /// Unsupported is a successful empty value; Vulkan creation/export failures
        /// retain their exact VkResult in Expected.
        [[nodiscard]] Expected<ExportableTimelineSemaphore> createExportableTimelineSemaphore();

        /// Retire raw (non-VMA) interop handles (FIF-safe deferred destroy), for the
        /// VkBuffer / VkDeviceMemory / VkSemaphore returned by the two creators above.
        /// Use retireRawBuffer (vkDestroyBuffer) for createExportableBuffer's buffer —
        /// NOT retireBuffer, which routes to vmaDestroyBuffer (wrong for a non-VMA buffer).
        void retireRawBuffer(VkBuffer buffer);
        void retireDeviceMemory(VkDeviceMemory memory);
        void retireSemaphore(VkSemaphore semaphore);

    private:
        RenderContext* ctx_; // the subject this facade forwards to (never null)
    };

} // namespace lux::render
