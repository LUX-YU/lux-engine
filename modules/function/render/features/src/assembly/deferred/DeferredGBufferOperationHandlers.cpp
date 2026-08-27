#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/function/render/client/features/deferred/DeferredGBufferOperation.hpp>
#include <lux/engine/render/renderer/features/deferred/DeferredGBufferFeature.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/renderer/features/MeshCommConfigValidation.hpp>

#include <limits>

namespace lux::render
{

    // ── Uniform factory interface ────────────────────────────────────────

    Expected<FeatureHandle> DeferredGBufferCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        // 回填/校验收敛为一张表(语义与原手写逐字段等价)。
        //  · cull 为条件条目:HZB 旗标选声明 set 1 的变体,与
        //    GpuDrivenMeshFeatureBase::initCommon 的管线布局分支成对
        //  · graph frag 可选无内置(空=跳过该族);finalize 已退役,
        //    wire 兼容:空合法,非空必须可解析
        using DGFill = BuiltinShaderFill<DeferredGBufferCommConfig>;
        static constexpr DGFill kShaderFills[] = {
            {.field = &DeferredGBufferCommConfig::cull_compute_shader,
             .builtin = EBuiltinShader::MESH_CULL_UNIFIED_COMP,
             .flag_mask = EGpuDrivenMeshExt::HZB,
             .builtin_alt = EBuiltinShader::MESH_CULL_UNIFIED_COMP_HZB},
            {.field = &DeferredGBufferCommConfig::compact_compute_shader, .builtin = EBuiltinShader::MDC_COMPACT_COMP},
            {.field = &DeferredGBufferCommConfig::gbuffer_vertex_shader, .builtin = EBuiltinShader::GBUFFER_VERT},
            {.field = &DeferredGBufferCommConfig::gbuffer_unlit_fragment_shader,
             .builtin = EBuiltinShader::GBUFFER_UNLIT_FRAG},
            {.field = &DeferredGBufferCommConfig::gbuffer_pbr_fragment_shader,
             .builtin = EBuiltinShader::GBUFFER_PBR_FRAG},
            {.field = &DeferredGBufferCommConfig::gbuffer_stylized_fragment_shader,
             .builtin = EBuiltinShader::GBUFFER_STYLIZED_FRAG},
            {.field = &DeferredGBufferCommConfig::gbuffer_graph_fragment_shader, .fill = false, .optional = true},
        };
        static constexpr MeshCommConfigExpectation kExpectation{
            .comm_version = kDeferredGBufferCommConfigVersion,
            .descriptor_layout_version = kDeferredGBufferDescriptorLayoutVersion,
            .known_ext_flags = kDeferredGBufferKnownExtFlags,
        };

        auto& shaders = sc->renderContext().globalRegistry().must<ShaderResources>();
        auto prepared = prepareMeshCommConfig(shaders, param, param_size, kExpectation, kShaderFills);
        if (!prepared)
            return lux::cxx::unexpected(prepared.error());

        const DeferredGBufferCommConfig& cc = *prepared;

        DeferredGBufferFeature::Config cfg{};
        cfg.gbuffer_vertex_shader = cc.gbuffer_vertex_shader;
        cfg.gbuffer_unlit_fragment_shader = cc.gbuffer_unlit_fragment_shader;
        cfg.gbuffer_pbr_fragment_shader = cc.gbuffer_pbr_fragment_shader;
        cfg.gbuffer_stylized_fragment_shader = cc.gbuffer_stylized_fragment_shader;
        cfg.gbuffer_graph_fragment_shader = cc.gbuffer_graph_fragment_shader; // optional (may be null)
        cfg.cull_compute_shader = cc.cull_compute_shader;
        cfg.compact_compute_shader = cc.compact_compute_shader;
        cfg.descriptor_layout_version = cc.descriptor_layout_version;
        cfg.extension_flags = cc.extension_flags;
        return sc->addFeature<DeferredGBufferFeature>(cfg);
    }

} // namespace lux::render
