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
 * Design (see .internal/p3-featurecontext-design-2026-06-26.md):
 *   - COMPOSITION, not inheritance: a RenderContextView HOLDS a reference to its
 *     subject (RenderContext&) and forwards to it. RenderContext was designed
 *     independently and stays OBLIVIOUS to features/facades — the dependency flows
 *     one way only (feature → RenderContextView → RenderContext), never the reverse.
 *   - ZERO virtual: a single subject type, no polymorphism. The facade is a
 *     lightweight value (one pointer) — copy/pass it freely, like a view.
 *   - The header traffics only neutral enums + forward-declared Vulkan HANDLES
 *     (core/vk_fwd.hpp) and forward-declares RenderContext, so it pulls NO
 *     <vulkan/vulkan.h> and NO internal RenderContext header onto a consumer; the
 *     forwarding bodies live in RenderContextView.cpp where RenderContext is complete.
 *
 * The registry accessor returns the PUBLIC base `ResourceRegistryBase&`, whose
 * `find<T>/emplace<T>/…` are template members instantiated at the CALL site with
 * the feature's OWN resource type — the engine never names an internal type here.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <lux/engine/render/core/vk_fwd.hpp>                          // VkDevice/VkShaderModule/VkPipelineLayout/VkDescriptorSetLayout
#include <lux/engine/render/core/PipelineHandle.hpp>                  // Graphics/ComputePipelineHandle (public)
#include <lux/engine/render/core/ResourceHandle.hpp>                  // ShaderHandle (public)
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>     // EDescriptorSetSlot (public)
#include <lux/engine/render/resources/EBuiltinShader.hpp>             // EBuiltinShader (public, P3-pre-1)
#include <lux/engine/render/resources/lifecycle/ResourceRegistry.hpp> // ResourceRegistryBase (public, P3-pre-2)
#include <lux/engine/render/FrameRetireScheduler.hpp>                 // FrameRetireScheduler (public)
#include <lux/engine/function/visibility.h>

// VMA handles — same fwd-typedefs the engine uses (core/VmaFwd.hpp), replicated
// here so this public header stays <vulkan/vulkan.h>- and <vk_mem_alloc.h>-free.
struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::rdesc { struct ShaderInfo; }

namespace lux::render
{
    class RenderContext;            // held subject (forwarded to; defined in an internal header)
    class DeferredDestroyQueue;     // returned by-reference; defined in an internal header
    struct GraphicsPipelineTemplate;// taken by-reference; engine pipeline description

    /// Result of createExportableBuffer — a VMA-BYPASSING dedicated allocation
    /// (vkAllocateMemory + VkExportMemoryAllocateInfo + vkBindBufferMemory) whose
    /// memory can be exported for import by an external API (e.g. CUDA). The caller
    /// OWNS all three handles: retire `buffer` + `memory` via retireBuffer*/
    /// retireDeviceMemory, and close `external_handle` once the importer has dup'd it.
    /// A null result (buffer == 0) means external memory is unsupported or a Vulkan
    /// call failed — the caller should fall back to a host-upload path.
    struct ExportableBuffer
    {
        VkBuffer       buffer{};         // forward-declared in core/vk_fwd.hpp
        VkDeviceMemory memory{};
        uint64_t       external_handle{0};   // Win32 HANDLE / POSIX fd widened to u64
        uint64_t       actual_size{0};       // >= requested (driver may round up)
    };

    /// Result of createExportableTimelineSemaphore — an exportable TIMELINE semaphore
    /// for cross-API (CUDA<->Vulkan) ping-pong sync. Caller owns both; retire the
    /// semaphore via retireSemaphore and close the handle after import.
    struct ExportableTimelineSemaphore
    {
        VkSemaphore semaphore{};
        uint64_t    external_handle{0};
    };

    class LUX_FUNCTION_PUBLIC RenderContextView
    {
    public:
        /// Wrap a subject. The facade is a non-owning view; @p ctx must outlive it
        /// (it always does — the owning scene outlives every feature it serves).
        explicit RenderContextView(RenderContext& ctx) noexcept : ctx_(&ctx) {}

        // ── Vulkan infrastructure ───────────────────────────────────────
        [[nodiscard]] VkDevice     device()         const noexcept;
        [[nodiscard]] VmaAllocator vmaAllocator()   const noexcept;
        [[nodiscard]] uint32_t     framesInFlight() const noexcept;

        // ── Global resource registry ────────────────────────────────────
        // Returns the public base; find<T>/emplace<T> instantiate on the caller's T.
        [[nodiscard]] ResourceRegistryBase&       globalRegistry() noexcept;
        [[nodiscard]] const ResourceRegistryBase& globalRegistry() const noexcept;

        // ── Shaders (the engine's ShaderResources stays hidden) ─────────
        [[nodiscard]] ShaderHandle   loadBuiltinShader(EBuiltinShader builtin, ShaderHandle configured = {});
        [[nodiscard]] ShaderHandle   loadShader(std::span<const std::byte> spirv, const lux::rdesc::ShaderInfo& info);
        [[nodiscard]] VkShaderModule shaderModule(ShaderHandle handle) const;
        [[nodiscard]] const lux::rdesc::ShaderInfo* shaderInfo(ShaderHandle handle) const;

        // ── Descriptor-set layouts ──────────────────────────────────────
        /// The engine's pre-built layout for a well-known descriptor slot.
        [[nodiscard]] VkDescriptorSetLayout engineSetLayout(EDescriptorSetSlot slot);

        // ── Pipeline layout + pipeline registration (Expected unwrapped here) ──
        [[nodiscard]] VkPipelineLayout buildStandardGraphicsLayout(
            uint32_t                                       descriptor_set_count,
            std::span<const lux::rdesc::ShaderInfo* const> shader_infos,
            std::string_view                               debug_name);
        [[nodiscard]] GraphicsPipelineHandle registerGraphics(
            const GraphicsPipelineTemplate&                  description,
            const std::vector<const lux::rdesc::ShaderInfo*>& shader_infos = {});
        [[nodiscard]] ComputePipelineHandle  registerCompute(VkShaderModule shader, VkPipelineLayout layout);

        // ── Feature-owned GPU-resource lifecycle plumbing ───────────────
        [[nodiscard]] DeferredDestroyQueue& deferredDestroyQueue() noexcept;
        [[nodiscard]] FrameRetireScheduler& retireScheduler()      noexcept;

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
        [[nodiscard]] bool     supportsExternalMemory() noexcept;
        /// Copies VK_UUID_SIZE (16) bytes into out[16] so an external API matches its device.
        void                   deviceUUID(uint8_t out[16]) noexcept;
        /// Forwards to PhysicalDevice::findMemoryTypeIndex; UINT32_MAX on miss.
        [[nodiscard]] uint32_t findMemoryTypeIndex(uint32_t type_filter, uint32_t property_flags) noexcept;

        /// Dedicated, VMA-BYPASSING exportable storage buffer. @p usage_flags is OR'd
        /// with STORAGE_BUFFER|TRANSFER_DST (pass a VkBufferUsageFlags as uint32_t to
        /// keep this header vulkan.h-free; 0 for storage-only). Null result on
        /// unsupported / failure. See ExportableBuffer for ownership.
        [[nodiscard]] ExportableBuffer createExportableBuffer(uint64_t size, uint32_t usage_flags);

        /// Exportable TIMELINE semaphore (initial value 0). Pair TWO of these for the
        /// producer/consumer ping-pong: the external producer signals one when its
        /// write is done (the engine waits on it via addExternalGraphicsWait before
        /// reading), and the engine signals the other when its read is done (the
        /// producer waits on it before reusing the buffer). Strictly monotonic values.
        [[nodiscard]] ExportableTimelineSemaphore createExportableTimelineSemaphore();

        /// Retire raw (non-VMA) interop handles (FIF-safe deferred destroy), for the
        /// VkDeviceMemory / VkSemaphore returned by the two creators above.
        void retireDeviceMemory(VkDeviceMemory memory);
        void retireSemaphore(VkSemaphore semaphore);

    private:
        RenderContext* ctx_;   // the subject this facade forwards to (never null)
    };

} // namespace lux::render
