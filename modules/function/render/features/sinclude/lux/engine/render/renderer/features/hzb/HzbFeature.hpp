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
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>             // ShaderHandle
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp> // ComputePipelineHandle
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp> // DescriptorLayoutId
#include <lux/engine/render/resources/hzb/HzbResources.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>           // per-view mip-set table
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>   // FifOwned<VkSampler> (C1)
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    class ViewCameraResource;

    class LUX_FUNCTION_PUBLIC HzbFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle compute_shader{};   ///< downsample.comp (self-loads from builtin)
            uint32_t     width{0};           ///< HZB extent — Stage A: caller-provided
            uint32_t     height{0};
        };

        HzbFeature();
        explicit HzbFeature(Config cfg);

        std::string_view name() const override { return "Hzb"; }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;

        [[nodiscard]] const HzbResources* resources() const noexcept { return hzb_res_; }

    private:
        /// 装载期建 HZB 的读布局 / 采样器 / 下采样管线。返回错误 = 这个特性装不起来:
        /// HZB 是遮挡剔除的输入,建不出来时 cull 会保守地一个都不剔 —— 那是性能塌方,
        /// 而不是「少了个效果」,该由上层知道。
        [[nodiscard]] Expected<void> init(const Config& cfg);
        /// (Re)create THIS VIEW's two HZB images at @p width×@p height and
        /// re-point its per-mip build descriptors. Idempotent for an unchanged
        /// extent (HzbResources::ensureView short-circuits).
        void rebuildViewAt(uint32_t view_id, uint32_t width, uint32_t height);

        Config                cfg_{};
        HzbResources*         hzb_res_{nullptr};                 ///< owned by the scene registry
        /// 可选相机资源的记忆化缓存(见 resolveViewCameraOnce)。
        ViewCameraResource*   cam_cache_{nullptr};
        ComputePipelineHandle compute_pipeline_{};
        // The build pipeline's layout is owned by the reflection-driven layout
        // path in PipelineManager; set0/set1 are retrieved during init via
        // computeSetLayout.
        VkDescriptorSetLayout set0_layout_{VK_NULL_HANDLE};
        VkDescriptorSetLayout set1_layout_{VK_NULL_HANDLE};      ///< depth set, for createTransientDS
        // Cull read side (Stage C): set-1 {COMBINED_IMAGE_SAMPLER, UNIFORM_BUFFER}.
        DescriptorLayoutId    read_layout_id_{kInvalidDescriptorLayoutId};
        VkDescriptorSetLayout read_layout_{VK_NULL_HANDLE};
        // nearest+clamp HZB sampler. FifOwned so it retires through the FIF
        // DescriptorService 采样器缓存的共享句柄 —— 服务持有生命周期
        // (matches Deferred/Tonemap)。
        VkSampler             hzb_sampler_{VK_NULL_HANDLE};
        uint32_t              frame_counter_{0};                 ///< absolute frame → ping-pong parity

        /// Per-mip set-0 descriptors, per ping-pong slot — PER VIEW, because the
        /// mip count follows that view's extent. Keyed by View::handle.index,
        /// evicted with the view (same hook that frees the images).
        struct ViewMipSets { std::array<std::vector<VkDescriptorSet>, 2> slot; };
        lux::cxx::BasicSparseSet<uint32_t, ViewMipSets> mip_sets_;
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline HzbFeature::HzbFeature() : HzbFeature(Config{}) {}

} // namespace lux::render
