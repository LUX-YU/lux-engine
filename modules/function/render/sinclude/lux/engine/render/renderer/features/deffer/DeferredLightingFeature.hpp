#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/renderer/features/deffer/GBufferTypes.hpp>
#include <lux/engine/render/core/EShadowTechnique.hpp>
#include <lux/engine/render/resources/lifecycle/FifOwned.hpp>
#include <cstdint>
#include <string>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

namespace lux::render
{

/**
 * @brief Deferred lighting pass — reads GBuffer textures, applies PBR lighting,
 *        outputs an HDR color target (RGBA16F).
 *
 * Path A (SAMPLED):  Standalone pass sampling GBuffer as sampler2D textures.
 * Path B (INPUT_ATTACHMENT): Subpass input path (requires Phase A-2 infrastructure).
 *
 * Descriptor layout:
 *   Set 0: Scene (ViewGpuData[])
 *   Set 1: GBuffer (4× combined_image_sampler: albedo, normal, emissive, depth)
 *   Set 2: Light SSBOs + Shadow (same layout as forward Set 3)
 */
class LUX_FUNCTION_PUBLIC DeferredLightingFeature : public RenderFeature
{
public:
    enum class EReadMode
    {
        SAMPLED,            ///< Path A: independent pass, texture() sampling
        INPUT_ATTACHMENT    ///< Path B: subpass input, subpassLoad() reading
    };

    struct Config
    {
        ShaderHandle vertex_shader{};     ///< fullscreen triangle
        /// Optional explicit override of the lighting fragment shader. If left
        /// invalid, init() picks the SPIR-V variant matching `technique`. To
        /// drive runtime technique switches, leave `fragment_shader` empty
        /// and update `technique` (then trigger a feature rebuild).
        ShaderHandle fragment_shader{};
        EShadowTechnique technique{EShadowTechnique::PCF};
        ShaderHandle cluster_build_shader{};
        ShaderHandle cluster_count_shader{};
        ShaderHandle cluster_scan_shader{};
        ShaderHandle cluster_fill_shader{};
        ShaderHandle cluster_clear_shader{};
        uint32_t enable_clustered{0};
        uint32_t cluster_x{16};
        uint32_t cluster_y{9};
        uint32_t cluster_z{24};
        uint32_t max_cluster_indices{1'048'576};
        EReadMode    read_mode = EReadMode::SAMPLED;
        std::string     color_output{"LitColor"};                        ///< Name of the lit color target to create
        GBufferLayout   gbuffer;
        std::string     shadow_atlas{"ShadowAtlas"};
        std::string     depth_target{"SceneDepth"};
    };

    explicit DeferredLightingFeature(Config cfg);
    ~DeferredLightingFeature() override;

    std::string_view name() const override { return "DeferredLighting"; }

    lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
    void onDetachFromScene(RenderScene& scene) override;
    void addPasses(RGBuilder& builder) override;
    void onFrameBegin(const FeatureFrameContext& ctx) override;

private:
    void init();
    void destroy() noexcept;

    /// Whether clustered light culling should be active for the given live light
    /// count. Shared by addPasses (the compile-time decision) and onFrameBegin
    /// (the threshold-crossing watch) so the two never disagree. (perf)
    [[nodiscard]] bool clusteringWanted(uint32_t light_count) const noexcept
    {
        return (cfg_.enable_clustered != 0u)
            && light_count >= kClusteredLightThreshold
            && cluster_build_pipeline_.valid() && cluster_count_pipeline_.valid()
            && cluster_scan_pipeline_.valid()  && cluster_fill_pipeline_.valid();
    }

    static constexpr uint32_t kClusteredLightThreshold = 16u;

    Config                  cfg_;
    GraphicsPipelineHandle  lighting_pipeline_{kInvalidPipelineHandle};
    ComputePipelineHandle   cluster_build_pipeline_{kInvalidComputePipelineHandle};
    ComputePipelineHandle   cluster_count_pipeline_{kInvalidComputePipelineHandle};
    ComputePipelineHandle   cluster_scan_pipeline_{kInvalidComputePipelineHandle};
    ComputePipelineHandle   cluster_fill_pipeline_{kInvalidComputePipelineHandle};
    ComputePipelineHandle   cluster_clear_pipeline_{kInvalidComputePipelineHandle};

    // GBuffer descriptor set layout (4× combined_image_sampler) — used by transient DS
    VkDescriptorSetLayout   gbuffer_ds_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout   cluster_ds_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout   cluster_clear_ds_layout_{VK_NULL_HANDLE};
    // FifOwned: retired through the FIF deferred-destroy queue on destruction —
    // no inline vkDestroySampler, no onDetach discipline. (C1)
    FifOwned<VkSampler>     gbuffer_sampler_;

    // The clustering decision the render graph was last compiled with. addPasses
    // bakes the cluster passes (or not) from the live light count; onFrameBegin
    // re-checks the count each frame and requests a recompile when it crosses the
    // threshold, since light add/remove does not otherwise invalidate the graph. (perf)
    bool                    clustering_compiled_{false};
};

} // namespace lux::render
