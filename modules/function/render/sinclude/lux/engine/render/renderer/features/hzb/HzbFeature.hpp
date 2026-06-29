#pragma once
/**
 * @file HzbFeature.hpp
 * @brief RenderGraph COMPUTE feature that builds the Hi-Z (max-Z) occlusion
 *        pyramid from SceneDepth.
 *
 * P2 HZB Stage A. Owns an HzbResources (R32_SFLOAT mip-chain image) + the
 * downsample compute pipeline + per-mip descriptor sets. addPasses() emits ONE
 * compute pass that binds SceneDepth as set 1 and calls HzbResources::recordBuild
 * to fill the whole pyramid (per-mip storage views + manual barriers, since the
 * RenderGraph only builds a mip-0 view per resource).
 *
 * Stage A: the pyramid is built but nothing consumes it yet, and the extent is
 * Config-driven. Stage C wires the swapchain extent + feeds the pyramid to the
 * cull pass (HZB_MODE).
 */
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>             // ShaderHandle
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp> // ComputePipelineHandle
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp> // DescriptorLayoutId
#include <lux/engine/render/resources/hzb/HzbResources.hpp>
#include <lux/engine/render/resources/lifecycle/FifOwned.hpp>   // FifOwned<VkSampler> (C1)
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC HzbFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle compute_shader{};   ///< downsample.comp (self-loads from builtin)
            uint32_t     width{0};           ///< HZB extent — Stage A: caller-provided
            uint32_t     height{0};
        };

        explicit HzbFeature(Config cfg = {});

        std::string_view name() const override { return "Hzb"; }
        void initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;

        [[nodiscard]] const HzbResources* resources() const noexcept { return hzb_res_; }

    private:
        void init(const Config& cfg);
        void rebuildAt(uint32_t width, uint32_t height);   ///< (re)create both HZB images at this extent

        Config                cfg_{};
        HzbResources*         hzb_res_{nullptr};                 ///< owned by the scene registry
        ComputePipelineHandle compute_pipeline_{};
        VkPipelineLayout      pipeline_layout_{VK_NULL_HANDLE};
        DescriptorLayoutId    set0_layout_id_{kInvalidDescriptorLayoutId};
        DescriptorLayoutId    set1_layout_id_{kInvalidDescriptorLayoutId};
        VkDescriptorSetLayout set1_layout_{VK_NULL_HANDLE};      ///< depth set, for createTransientDS
        // Cull read side (Stage C): set-1 {COMBINED_IMAGE_SAMPLER, UNIFORM_BUFFER}.
        DescriptorLayoutId    read_layout_id_{kInvalidDescriptorLayoutId};
        VkDescriptorSetLayout read_layout_{VK_NULL_HANDLE};
        // nearest+clamp HZB sampler. FifOwned so it retires through the FIF
        // deferred-destroy queue when this feature is destroyed — no inline
        // vkDestroySampler / onDetach discipline (C1; matches Deferred/Tonemap).
        FifOwned<VkSampler>   hzb_sampler_;
        uint32_t              frame_counter_{0};                 ///< absolute frame → ping-pong parity
        std::array<std::vector<VkDescriptorSet>, 2> mip_sets_;   ///< per-mip set 0, one per ping-pong slot
    };

} // namespace lux::render
