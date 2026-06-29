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

    private:
        RenderContext* ctx_;   // the subject this facade forwards to (never null)
    };

} // namespace lux::render
