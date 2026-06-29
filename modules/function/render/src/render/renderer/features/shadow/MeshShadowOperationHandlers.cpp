#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowFeature.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/renderer/features/MeshCommConfigValidation.hpp>

#include <limits>

namespace lux::render
{

    // ── Uniform factory interface ────────────────────────────────────────

    static FeatureHandle meshShadowCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        constexpr uint32_t kInvalidFeatureId = std::numeric_limits<uint32_t>::max();

        MeshShadowCommConfig cc{};
        if (!validateMeshCommConfig("MeshShadowOperationHandlers", param, param_size,
                kMeshShadowCommConfigVersion, kMeshShadowDescriptorLayoutVersion,
                kMeshShadowExtFlagHZB | kMeshShadowExtFlagBindless, cc))
            return {};

        auto* shaders = sc->renderContext().globalRegistry().find<ShaderResources>();
        if (!shaders){
            return {};
        }
        // Fill null handles with embedded builtin shaders
        cc.shadow_cull_shader    = ensureBuiltinShader(shaders, cc.shadow_cull_shader,    EBuiltinShader::MESH_CULL_UNIFIED_COMP);
        cc.shadow_compact_shader = ensureBuiltinShader(shaders, cc.shadow_compact_shader, EBuiltinShader::MDC_COMPACT_COMP);
        cc.shadow_clear_shader   = ensureBuiltinShader(shaders, cc.shadow_clear_shader,   EBuiltinShader::CLEAR_COUNT_BUFFERS_COMP);
        // Depth-only caster pipeline → thin vert matching shadow_depth.frag's
        // (vUV-only) interface. The EVSM caster pipeline pulls the fat
        // MESH_SHADOW_VERT lazily inside MeshShadowFeature, so this default
        // does not affect EVSM correctness.
        cc.shadow_vert_shader    = ensureBuiltinShader(shaders, cc.shadow_vert_shader,    EBuiltinShader::SHADOW_DEPTH_VERT);
        cc.shadow_frag_shader    = ensureBuiltinShader(shaders, cc.shadow_frag_shader,    EBuiltinShader::SHADOW_DEPTH_FRAG);

        const auto has_shader = [shaders](ShaderHandle h) {
            return shaders->get(h) != nullptr;
        };

        // The legacy finalize stage is removed from runtime execution.
        // Keep wire compatibility by accepting shadow_finalize_shader == invalid.
        const bool has_deprecated_finalize =
            cc.shadow_finalize_shader.is_null() || has_shader(cc.shadow_finalize_shader);

        if (!has_shader(cc.shadow_cull_shader)
            || !has_shader(cc.shadow_compact_shader)
            || !has_shader(cc.shadow_vert_shader)
            || !has_shader(cc.shadow_frag_shader)
            || !has_deprecated_finalize
            || (cc.shadow_clear_shader.valid() && !has_shader(cc.shadow_clear_shader)))
        {
            return {};
        }

        MeshShadowFeature::Config cfg{};
        cfg.shadow_cull_shader = cc.shadow_cull_shader;
        cfg.shadow_compact_shader = cc.shadow_compact_shader;
        cfg.shadow_clear_shader = cc.shadow_clear_shader;
        cfg.shadow_vert_shader = cc.shadow_vert_shader;
        cfg.shadow_frag_shader = cc.shadow_frag_shader;
        cfg.descriptor_layout_version = cc.descriptor_layout_version;
        cfg.extension_flags = cc.extension_flags;
        return sc->addFeature<MeshShadowFeature>(cfg);
    }

    const FeatureFactory kMeshShadowFeatureFactory = makeSimpleFactory(&meshShadowCreateFn, "MeshShadow");

} // namespace lux::render
