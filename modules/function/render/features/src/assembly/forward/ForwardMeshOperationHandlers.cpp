#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/function/render/client/features/forward/ForwardMeshOperation.hpp>
#include <lux/engine/render/renderer/features/forward/ForwardMeshFeature.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/renderer/features/MeshCommConfigValidation.hpp>

#include <limits>

namespace lux::render
{
    // ── Uniform factory interface ────────────────────────────────────────
    Expected<FeatureHandle> ForwardMeshCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        // 回填/校验收敛为一张表(语义与原手写逐字段等价)。
        //  · cull 为条件条目:HZB 旗标选 set-1 变体,与 GpuDrivenMeshFeatureBase
        //    的管线布局分支成对
        //  · pbr/stylized 默认 PCF 变体(EVSM 变体由 ShadowTechnique 选)
        //  · graph_fragment 可选无内置:空=跳过该族,非空必须可解析
        using FMFill = BuiltinShaderFill<ForwardMeshCommConfig>;
        static constexpr FMFill kShaderFills[] = {
            { .field = &ForwardMeshCommConfig::forward_cull_shader,
              .builtin = EBuiltinShader::MESH_CULL_UNIFIED_COMP,
              .flag_mask = EGpuDrivenMeshExt::HZB,
              .builtin_alt = EBuiltinShader::MESH_CULL_UNIFIED_COMP_HZB },
            { .field = &ForwardMeshCommConfig::forward_compact_shader, .builtin = EBuiltinShader::MDC_COMPACT_COMP },
            { .field = &ForwardMeshCommConfig::forward_vert_shader,    .builtin = EBuiltinShader::FORWARD_MESH_VERT },
            { .field = &ForwardMeshCommConfig::unlit_fragment,         .builtin = EBuiltinShader::FORWARD_UNLIT_FRAG },
            { .field = &ForwardMeshCommConfig::pbr_fragment,           .builtin = EBuiltinShader::FORWARD_PBR_FRAG_PCF },
            { .field = &ForwardMeshCommConfig::stylized_fragment,      .builtin = EBuiltinShader::FORWARD_STYLIZED_FRAG_PCF },
            { .field = &ForwardMeshCommConfig::graph_fragment,         .fill = false, .optional = true },
        };
        static constexpr MeshCommConfigExpectation kExpectation{
            .comm_version              = kForwardMeshCommConfigVersion,
            .descriptor_layout_version = kForwardMeshDescriptorLayoutVersion,
            .known_ext_flags           = GpuDrivenMeshExtFlags{EGpuDrivenMeshExt::HZB} |
                                         GpuDrivenMeshExtFlags{EGpuDrivenMeshExt::Bindless},
        };

        auto& shaders = sc->renderContext().globalRegistry().must<ShaderResources>();
        auto  prepared =
            prepareMeshCommConfig(shaders, param, param_size, kExpectation, kShaderFills);
        if (!prepared)
            return lux::cxx::unexpected(prepared.error());

        const ForwardMeshCommConfig& cc = *prepared;

        ForwardMeshFeature::Config cfg{};
        cfg.forward_cull_shader       = cc.forward_cull_shader;
        cfg.forward_compact_shader    = cc.forward_compact_shader;
        cfg.forward_vert_shader       = cc.forward_vert_shader;
        cfg.unlit_fragment            = cc.unlit_fragment;
        cfg.pbr_fragment              = cc.pbr_fragment;
        cfg.stylized_fragment         = cc.stylized_fragment;
        cfg.graph_fragment            = cc.graph_fragment;   // optional (may be null)
        cfg.descriptor_layout_version = cc.descriptor_layout_version;
        cfg.extension_flags           = cc.extension_flags;
        return sc->addFeature<ForwardMeshFeature>(cfg);
    }

} // namespace lux::render
