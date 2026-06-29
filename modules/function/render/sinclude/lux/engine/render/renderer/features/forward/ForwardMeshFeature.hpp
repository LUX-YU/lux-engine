#pragma once
#include <lux/engine/render/renderer/features/GpuDrivenMeshFeatureBase.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <cstdint>
#include <string>
#include <lux/engine/function/visibility.h>

#include <array>
#include <cstdint>

namespace lux::render
{

/**
 * @brief Forward-mesh GPU-driven feature (three-stream layout)
 *
 * Adds per-material-bucket draw pipelines on top of the shared
 * GpuDrivenMeshFeatureBase infrastructure.
 *
 * Render graph passes:
 *   1. ForwardMeshForwardCull  (COMPUTE)  — frustum cull → per-bucket indirect draws
 *   2. ForwardMeshForwardCompact (COMPUTE) — per-MDC compact indirect commands
 *   3. ForwardMeshForwardDraw  (GRAPHICS) — per-bucket vkCmdDrawIndexedIndirectCount
 */
class LUX_FUNCTION_PUBLIC ForwardMeshFeature : public GpuDrivenMeshFeatureBase
{
public:
    static constexpr phase_mask_t kExtractPhaseMask =
        phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardOpaque));

    struct Config
    {
        ShaderHandle forward_cull_shader{};
        ShaderHandle forward_compact_shader{};
        /// Single mesh vertex shader (the _vp variant; reads the bindless
        /// vertex pool at set 7 for both static and skinned draws).
        ShaderHandle forward_vert_shader{};
        ShaderHandle unlit_fragment{};
        ShaderHandle pbr_fragment{};
        ShaderHandle stylized_fragment{};
        ShaderHandle graph_fragment{};   // optional; no builtin (override-only, Graph family)
        uint32_t descriptor_layout_version{0};
        uint32_t extension_flags{0};
        std::string color_target{"SceneColor"};
        std::string depth_target{"SceneDepth"};
        std::string shadow_atlas{"ShadowAtlas"};
    };

    explicit ForwardMeshFeature(Config cfg);
    ~ForwardMeshFeature() override;

    std::string_view name() const override { return "ForwardMesh"; }

    // =====================================================================
    //  RenderFeature lifecycle
    // =====================================================================
    void initAndAttachTo(RenderScene& scene) override;
    void addPasses(RGBuilder& builder) override;

private:
    void init();

    Config cfg_;
    VkDescriptorSetLayout visible_set_layout_{VK_NULL_HANDLE};

    // bucket_pipelines_ (the per-bucket pipeline pool) lives in
    // GpuDrivenMeshFeatureBase (filled by registerFamilyPipelines, shared with
    // the deferred path).
};

} // namespace lux::render
