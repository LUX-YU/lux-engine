#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/function/render/client/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowFeature.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/renderer/features/MeshCommConfigValidation.hpp>

#include <limits>

namespace lux::render
{

    // ── Uniform factory interface ────────────────────────────────────────

    Expected<FeatureHandle> MeshShadowCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        // 回填/校验收敛为一张表(语义与原手写逐字段等价)。
        //  · vert 默认薄身 SHADOW_DEPTH_VERT(仅 vUV 接口);EVSM caster 在
        //    MeshShadowFeature 内部惰性拉肥版 MESH_SHADOW_VERT,此默认不影响
        //    EVSM 正确性
        //  · clear 为"回填 + 可选":回填后非空必须可解析(原判据
        //    valid() && !has_shader 的等价形)
        //  · finalize 已退役,wire 兼容:空合法,非空必须可解析
        using MSFill = BuiltinShaderFill<MeshShadowCommConfig>;
        static constexpr MSFill kShaderFills[] = {
            { .field = &MeshShadowCommConfig::shadow_cull_shader,     .builtin = EBuiltinShader::MESH_CULL_UNIFIED_COMP },
            { .field = &MeshShadowCommConfig::shadow_compact_shader,  .builtin = EBuiltinShader::MDC_COMPACT_COMP },
            { .field = &MeshShadowCommConfig::shadow_clear_shader,    .builtin = EBuiltinShader::CLEAR_COUNT_BUFFERS_COMP, .optional = true },
            { .field = &MeshShadowCommConfig::shadow_vert_shader,     .builtin = EBuiltinShader::SHADOW_DEPTH_VERT },
            { .field = &MeshShadowCommConfig::shadow_frag_shader,     .builtin = EBuiltinShader::SHADOW_DEPTH_FRAG },
        };
        static constexpr MeshCommConfigExpectation kExpectation{
            .comm_version              = kMeshShadowCommConfigVersion,
            .descriptor_layout_version = kMeshShadowDescriptorLayoutVersion,
            .known_ext_flags           = GpuDrivenMeshExtFlags{EGpuDrivenMeshExt::HZB} |
                                         GpuDrivenMeshExtFlags{EGpuDrivenMeshExt::Bindless},
        };

        auto& shaders = sc->renderContext().globalRegistry().must<ShaderResources>();
        auto  prepared =
            prepareMeshCommConfig(shaders, param, param_size, kExpectation, kShaderFills);
        if (!prepared)
            return lux::cxx::unexpected(prepared.error());

        const MeshShadowCommConfig& cc = *prepared;

        MeshShadowFeature::Config cfg{};
        cfg.shadow_cull_shader        = cc.shadow_cull_shader;
        cfg.shadow_compact_shader     = cc.shadow_compact_shader;
        cfg.shadow_clear_shader       = cc.shadow_clear_shader;
        cfg.shadow_vert_shader        = cc.shadow_vert_shader;
        cfg.shadow_frag_shader        = cc.shadow_frag_shader;
        cfg.descriptor_layout_version = cc.descriptor_layout_version;
        cfg.extension_flags           = cc.extension_flags;
        return sc->addFeature<MeshShadowFeature>(cfg);
    }

} // namespace lux::render
