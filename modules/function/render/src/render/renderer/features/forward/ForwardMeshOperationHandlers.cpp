#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/renderer/features/forward/ForwardMeshOperation.hpp>
#include <lux/engine/render/renderer/features/forward/ForwardMeshFeature.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/renderer/features/MeshCommConfigValidation.hpp>

#include <limits>

namespace lux::render
{
    // ── Uniform factory interface ────────────────────────────────────────
    static FeatureHandle forwardMeshCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        constexpr uint32_t kInvalidFeatureId = std::numeric_limits<uint32_t>::max();

        ForwardMeshCommConfig cc{};
        if (!validateMeshCommConfig("ForwardMeshOperationHandlers", param, param_size,
                kForwardMeshCommConfigVersion, kForwardMeshDescriptorLayoutVersion,
                kForwardMeshExtFlagHZB | kForwardMeshExtFlagBindless, cc))
            return {};

        auto* shaders = sc->renderContext().globalRegistry().find<ShaderResources>();
        if (!shaders)
            return {};

        // Fill null handles with embedded builtin shaders
        // HZB-on picks the set-1 cull variant; HZB-off picks the set-0-only one
        // (matches the pipeline-layout branch in GpuDrivenMeshFeatureBase).
        cc.forward_cull_shader    = ensureBuiltinShader(shaders, cc.forward_cull_shader,
            (cc.extension_flags & kForwardMeshExtFlagHZB)
                ? EBuiltinShader::MESH_CULL_UNIFIED_COMP_HZB
                : EBuiltinShader::MESH_CULL_UNIFIED_COMP);
        cc.forward_compact_shader = ensureBuiltinShader(shaders, cc.forward_compact_shader, EBuiltinShader::MDC_COMPACT_COMP);
        cc.forward_vert_shader    = ensureBuiltinShader(shaders, cc.forward_vert_shader,    EBuiltinShader::FORWARD_MESH_VERT);
        cc.unlit_fragment         = ensureBuiltinShader(shaders, cc.unlit_fragment,         EBuiltinShader::FORWARD_UNLIT_FRAG);
        // C1: default to PCF variant; C2 will let the active ShadowTechnique pick.
        cc.pbr_fragment           = ensureBuiltinShader(shaders, cc.pbr_fragment,           EBuiltinShader::FORWARD_PBR_FRAG_PCF);
        cc.stylized_fragment      = ensureBuiltinShader(shaders, cc.stylized_fragment,      EBuiltinShader::FORWARD_STYLIZED_FRAG_PCF);

        const auto has_shader = [shaders](ShaderHandle h) {
            return shaders->get(h) != nullptr;
        };

        // The Graph family frag is optional (no builtin): null is fine (family
        // skipped); if supplied it must resolve to a real shader.
        const bool has_valid_graph =
            cc.graph_fragment.is_null() || has_shader(cc.graph_fragment);

        if (!has_shader(cc.forward_cull_shader)
            || !has_shader(cc.forward_compact_shader)
            || !has_shader(cc.forward_vert_shader)
            || !has_shader(cc.unlit_fragment)
            || !has_shader(cc.pbr_fragment)
            || !has_shader(cc.stylized_fragment)
            || !has_valid_graph)
        {
            return {};
        }

        ForwardMeshFeature::Config cfg{};
        cfg.forward_cull_shader = cc.forward_cull_shader;
        cfg.forward_compact_shader  = cc.forward_compact_shader;
        cfg.forward_vert_shader = cc.forward_vert_shader;
        cfg.unlit_fragment      = cc.unlit_fragment;
        cfg.pbr_fragment        = cc.pbr_fragment;
        cfg.stylized_fragment   = cc.stylized_fragment;
        cfg.graph_fragment      = cc.graph_fragment;   // optional (may be null)
        cfg.descriptor_layout_version = cc.descriptor_layout_version;
        cfg.extension_flags = cc.extension_flags;
        return sc->addFeature<ForwardMeshFeature>(cfg);
    }

    // Stable identity: re-registration (editor preview bring-up) dedups via
    // AlreadyRegistered instead of growing the registry per bring-up.
    static constexpr FeatureDescriptor kForwardMeshDescriptor{
        .type              = featureId("lux.render.forward_mesh.v1"),
        .name              = "ForwardMesh",
        .contributes_graph = true,
    };
    const FeatureFactory kForwardMeshFeatureFactory =
        makeSimpleFactory(&forwardMeshCreateFn, "ForwardMesh", kForwardMeshDescriptor);

} // namespace lux::render
