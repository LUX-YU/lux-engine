#pragma once
#include <lux/engine/function/render/client/features/GpuDrivenMeshExtFlags.hpp>
#include <lux/engine/render/renderer/features/GpuDrivenMeshFeatureBase.hpp>
#include <lux/engine/render/renderer/features/deferred/GBufferTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
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
            GpuDrivenMeshExtFlags extension_flags{};
            GBufferLayout gbuffer;
            std::string depth_target{"SceneDepth"};
        };

        explicit DeferredGBufferFeature(Config cfg);
        ~DeferredGBufferFeature() override;

        /// 稳定身份,兄弟特性靠它相认(理由同 ShadowMapFeature::kFeatureName)。
        static constexpr std::string_view kFeatureName = "DeferredGBuffer";

        std::string_view name() const override
        {
            return kFeatureName;
        }

        // =====================================================================
        //  RenderFeature lifecycle
        // =====================================================================
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;

        /// G-buffer 是否写进 local-read 合并作用域。DeferredLightingFeature 在自己的
        /// attach 期读它来交叉校验 —— 两边必须同开同关,否则光照那条管线会去
        /// subpassLoad 一组根本没并进同一作用域的附件。
        [[nodiscard]] bool localReadScope() const noexcept
        {
            return local_read_scope_;
        }

    private:
        Expected<void> init();
        /// 析构与 onDetachFromScene 的共同收尾(幂等)。
        void releaseAll() noexcept;

        Config cfg_;
        /// local-read 合并作用域是否生效(init 期由 extension_flags × caps
        /// 解析一次)。管线 remap 声明与 G-buffer usage 声明共用此判定——
        /// 必须与 lighting 消费者的 effective_local_read_ 落在同一 caps 位。
        bool local_read_scope_{false};
        VkDescriptorSetLayout visible_set_layout_{VK_NULL_HANDLE};
        // bucket_pipelines_ (the per-bucket pipeline pool) lives in
        // GpuDrivenMeshFeatureBase (filled by registerFamilyPipelines, shared with
        // the forward path).
    };
} // namespace lux::render
