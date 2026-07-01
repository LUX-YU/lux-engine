#pragma once
#include <lux/engine/render/renderer/features/GpuDrivenMeshFeatureBase.hpp>
#include <lux/engine/render/renderer/features/deffer/GBufferTypes.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <cstdint>
#include <lux/engine/function/visibility.h>
#include <lux/engine/gapi/vk/Pipeline.hpp>

#include <array>

namespace lux::render
{
    /**
     * @brief Deferred G-Buffer feature — MRT geometry pass + GPU-driven cull.
     *
     * Inherits common infrastructure (instance storage, cull resources,
     * bucket management) from GpuDrivenMeshFeatureBase.
     * Shadow rendering is handled separately by MeshShadowFeature.
     *
     * Render graph passes:
     *   1. DeferredGBufferCull  (COMPUTE)  — frustum cull → per-bucket indirect draws
     *   2. DeferredGBufferCompact (COMPUTE) — per-MDC compact indirect commands
     *   3. DeferredGBufferDraw  (GRAPHICS) — MRT draw with compacted indirect count
     */
    class LUX_FUNCTION_PUBLIC DeferredGBufferFeature : public GpuDrivenMeshFeatureBase
    {
    public:
        static constexpr phase_mask_t kExtractPhaseMask =
            phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::GBuffer));

        struct Config
        {
            /// Single mesh vertex shader (the _vp variant; reads the bindless
            /// vertex pool at set 7 for both static and skinned draws).
            ShaderHandle gbuffer_vertex_shader{};
            ShaderHandle gbuffer_unlit_fragment_shader{};
            ShaderHandle gbuffer_pbr_fragment_shader{};
            ShaderHandle gbuffer_stylized_fragment_shader{};
            ShaderHandle gbuffer_graph_fragment_shader{}; // optional; no builtin (override-only)
            ShaderHandle cull_compute_shader{};
            ShaderHandle compact_compute_shader{};
            uint32_t descriptor_layout_version{0};
            uint32_t extension_flags{0};
            GBufferLayout gbuffer;
            std::string depth_target{"SceneDepth"};
        };

        explicit DeferredGBufferFeature(Config cfg);
        ~DeferredGBufferFeature() override;

        std::string_view name() const override { return "DeferredGBuffer"; }

        // =====================================================================
        //  RenderFeature lifecycle
        // =====================================================================
        lux::render::Expected<void> initAndAttachTo(RenderScene &scene) override;
        void onDetachFromScene(RenderScene &scene) override;
        void addPasses(RGBuilder &builder) override;

    private:
        void init();

        Config cfg_;
        VkDescriptorSetLayout visible_set_layout_{VK_NULL_HANDLE};
        // bucket_pipelines_ (the per-bucket pipeline pool) lives in
        // GpuDrivenMeshFeatureBase (filled by registerFamilyPipelines, shared with
        // the forward path).
    };
} // namespace lux::render
