#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/renderer/features/deffer/DeferredGBufferOperation.hpp>
#include <lux/engine/render/renderer/features/deffer/DeferredGBufferFeature.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/renderer/features/MeshCommConfigValidation.hpp>

#include <limits>

namespace lux::render
{

    // ── Uniform factory interface ────────────────────────────────────────

    static FeatureHandle deferredGBufferCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        constexpr uint32_t kInvalidFeatureId = std::numeric_limits<uint32_t>::max();

        DeferredGBufferCommConfig cc{};
        if (!validateMeshCommConfig("DeferredGBufferOperationHandlers", param, param_size,
                kDeferredGBufferCommConfigVersion, kDeferredGBufferDescriptorLayoutVersion,
                kDeferredGBufferExtFlagHZB | kDeferredGBufferExtFlagBindless, cc))
            return {};

        auto* shaders = sc->renderContext().globalRegistry().find<ShaderResources>();
        if (!shaders)
            return {};

        // Fill null handles with embedded builtin shaders
        // HZB-on picks the cull variant that declares descriptor set 1; HZB-off
        // (default) picks the set-0-only variant. GpuDrivenMeshFeatureBase::initCommon
        // branches the pipeline layout on the same flag, so shader + layout match.
        cc.cull_compute_shader              = ensureBuiltinShader(shaders, cc.cull_compute_shader,
            (cc.extension_flags & kDeferredGBufferExtFlagHZB)
                ? EBuiltinShader::MESH_CULL_UNIFIED_COMP_HZB
                : EBuiltinShader::MESH_CULL_UNIFIED_COMP);
        cc.compact_compute_shader           = ensureBuiltinShader(shaders, cc.compact_compute_shader,           EBuiltinShader::MDC_COMPACT_COMP);
        cc.gbuffer_vertex_shader            = ensureBuiltinShader(shaders, cc.gbuffer_vertex_shader,            EBuiltinShader::GBUFFER_VERT);
        cc.gbuffer_unlit_fragment_shader    = ensureBuiltinShader(shaders, cc.gbuffer_unlit_fragment_shader,    EBuiltinShader::GBUFFER_UNLIT_FRAG);
        cc.gbuffer_pbr_fragment_shader      = ensureBuiltinShader(shaders, cc.gbuffer_pbr_fragment_shader,      EBuiltinShader::GBUFFER_PBR_FRAG);
        cc.gbuffer_stylized_fragment_shader = ensureBuiltinShader(shaders, cc.gbuffer_stylized_fragment_shader, EBuiltinShader::GBUFFER_STYLIZED_FRAG);

        const auto has_shader = [shaders](ShaderHandle h) {
            return shaders->get(h) != nullptr;
        };

        // The legacy finalize stage is removed from runtime execution.
        // Keep wire compatibility by accepting finalize_compute_shader == invalid.
        const bool has_deprecated_finalize =
            cc.finalize_compute_shader.is_null() || has_shader(cc.finalize_compute_shader);

        // The Graph family frag is optional (no builtin): null is fine (family
        // skipped); if supplied it must resolve to a real shader.
        const bool has_valid_graph =
            cc.gbuffer_graph_fragment_shader.is_null() || has_shader(cc.gbuffer_graph_fragment_shader);

        if (!has_shader(cc.gbuffer_vertex_shader)
            || !has_shader(cc.gbuffer_unlit_fragment_shader)
            || !has_shader(cc.gbuffer_pbr_fragment_shader)
            || !has_shader(cc.gbuffer_stylized_fragment_shader)
            || !has_shader(cc.cull_compute_shader)
            || !has_shader(cc.compact_compute_shader)
            || !has_deprecated_finalize
            || !has_valid_graph)
        {
            return {};
        }

        DeferredGBufferFeature::Config cfg{};
        cfg.gbuffer_vertex_shader   = cc.gbuffer_vertex_shader;
        cfg.gbuffer_unlit_fragment_shader = cc.gbuffer_unlit_fragment_shader;
        cfg.gbuffer_pbr_fragment_shader = cc.gbuffer_pbr_fragment_shader;
        cfg.gbuffer_stylized_fragment_shader = cc.gbuffer_stylized_fragment_shader;
        cfg.gbuffer_graph_fragment_shader = cc.gbuffer_graph_fragment_shader;  // optional (may be null)
        cfg.cull_compute_shader     = cc.cull_compute_shader;
        cfg.compact_compute_shader  = cc.compact_compute_shader;
        cfg.descriptor_layout_version = cc.descriptor_layout_version;
        cfg.extension_flags = cc.extension_flags;
        return sc->addFeature<DeferredGBufferFeature>(cfg);
    }

    const FeatureFactory kDeferredGBufferFeatureFactory = makeSimpleFactory(&deferredGBufferCreateFn, "DeferredGBuffer");

} // namespace lux::render
