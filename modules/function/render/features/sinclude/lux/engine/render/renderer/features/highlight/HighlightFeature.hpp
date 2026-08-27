#pragma once
#include <lux/engine/function/render/client/features/GpuDrivenMeshExtFlags.hpp>
// =========================================================================
//  HighlightFeature — object highlight (soft outer outline/halo).
//
//  GPU-driven (subclasses GpuDrivenMeshFeatureBase, like ForwardMesh/DeferredGBuffer):
//  its OWN frustum cull produces Hl{Visible,Indirect,Count}; a depth-less mask
//  draw renders the highlighted silhouettes into a single-channel R8 mask (static +
//  skinned, LIVE pose via the bindless vertex pool at set 7); a separable-blur +
//  composite pass reads the mask and draws the outer halo over SceneColor.
//
//  This is a MECHANISM feature: any client sets the per-instance highlight flag
//  (kInstanceFlagHighlight) and picks the halo color via the comm config. The
//  editor's "selection" is just one client; gameplay/hover/debug can use it too.
//  Single-channel (one color per feature instance); multiple objects can be
//  highlighted at once (all share the configured color).
// =========================================================================
#include <lux/engine/render/renderer/features/GpuDrivenMeshFeatureBase.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC HighlightFeature : public GpuDrivenMeshFeatureBase
    {
    public:
        struct Config
        {
            ShaderHandle cull_shader{};    ///< MESH_CULL_UNIFIED_COMP
            ShaderHandle compact_shader{}; ///< MDC_COMPACT_COMP
            ShaderHandle mask_vert{};      ///< HIGHLIGHT_MASK_VERT
            ShaderHandle mask_frag{};      ///< HIGHLIGHT_MASK_FRAG
            ShaderHandle blur_frag{};      ///< HIGHLIGHT_BLUR_FRAG (separable Gaussian)
            ShaderHandle composite_frag{}; ///< HIGHLIGHT_COMPOSITE_FRAG (outer halo)
            uint32_t descriptor_layout_version{0};
            GpuDrivenMeshExtFlags extension_flags{};
            std::string color_target{"SceneColor"};
            std::string mask_target{"HighlightMask"};
            // Halo appearance
            float glow_color[3]{1.0f, 0.55f, 0.06f};
            float glow_intensity{3.0f};
            float glow_radius{2.5f};
        };

        explicit HighlightFeature(Config cfg);
        ~HighlightFeature() override;

        std::string_view name() const override
        {
            return "Highlight";
        }

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;

    private:
        Expected<void> init();
        /// 析构与 onDetachFromScene 的共同收尾(幂等)。
        void releaseAll() noexcept;

        Config cfg_;
        VkDescriptorSetLayout visible_set_layout_{VK_NULL_HANDLE};     // set 5 (cull → draw)
        VkDescriptorSetLayout blur_ds_layout_{VK_NULL_HANDLE};         // blur set 1 (1 sampler: input)
        VkDescriptorSetLayout composite_ds_layout_{VK_NULL_HANDLE};    // composite set 1 (2 samplers: blurred + sharp)
        GraphicsPipelineHandle blur_pipeline_{kInvalidPipelineHandle}; // separable Gaussian (H & V)
        GraphicsPipelineHandle composite_pipeline_{kInvalidPipelineHandle}; // outer-halo composite
        // The mask DRAW pipeline lives in bucket_pipelines_ (base), built by
        // registerFamilyPipelines with a constant mask-frag resolver — every family
        // bucket resolves to the same mask pipeline.
        VkSampler mask_sampler_{VK_NULL_HANDLE}; ///< 服务缓存句柄
    };

} // namespace lux::render
