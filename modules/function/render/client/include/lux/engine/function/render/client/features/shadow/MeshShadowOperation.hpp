#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/features/GpuDrivenMeshExtFlags.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;

    inline constexpr uint32_t kMeshShadowCommConfigVersion =
        3u; // 3:删 shadow_finalize_shader 死字段(finalize 路径早已退役)
    inline constexpr uint32_t kMeshShadowDescriptorLayoutVersion = 1u;

    // =========================================================================
    //  Default shader name constants for MeshShadowFeature
    // =========================================================================
    inline constexpr std::string_view kMeshShadowCullShaderName = "mesh_cull_unified.comp";
    inline constexpr std::string_view kMeshShadowCompactShaderName = "mdc_compact.comp";
    inline constexpr std::string_view kMeshShadowClearShaderName = "clear_count_buffers.comp";
    // Depth-only _vp vert matching shadow_depth.frag's interface (vUV only) while
    // using the same bindless pool path. The fat `mesh_shadow_vp.vert` carries
    // vShadowNear / vShadowFar / vDepthPersp for the EVSM caster and is selected
    // via EVSMShadowTechnique::casterVertVariant() inside
    // MeshShadowFeature::ensureCasterPipeline(), independent of this default.
    inline constexpr std::string_view kMeshShadowVertShaderName = "shadow_depth_vp.vert";
    inline constexpr std::string_view kMeshShadowFragShaderName = "shadow_depth.frag";

    /// Comm-layer config for MeshShadowFeature.
    /// Client fills shader indices from CompileShader replies.
    /// requires 第二条(shadow_map):attach 期 find<ShadowResources> 决定阴影
    /// 缓冲尺寸;没有 ShadowMap 的 MeshShadow 不产任何阴影 —— 语义必需。
    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(
        prefix = MeshShadow,
        id = lux.render.mesh_shadow.v1,
        display = MeshShadow,
        requires = "lux.render.mesh_stack.v1,lux.render.shadow_map.v1",
        custom_create = true) MeshShadowCommConfig
    {
        ShaderHandle shadow_cull_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle shadow_compact_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle shadow_clear_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle shadow_vert_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle shadow_frag_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        uint32_t comm_config_version{kMeshShadowCommConfigVersion};
        uint32_t descriptor_layout_version{kMeshShadowDescriptorLayoutVersion};
        GpuDrivenMeshExtFlags extension_flags{};
    };
    static_assert(std::is_trivially_copyable_v<MeshShadowCommConfig>);

    /// MeshShadow feature factory — registered at runtime via RegisterFeatureType.

    /// 本特性产出的 render-graph pass 名(跨 feature 引用请用常量,理由同上)。
    /// DeferredLighting 与 EVSMShadowTechnique 都要排在它之后。
    inline constexpr std::string_view kMeshShadowDrawPassName = "MeshShadowDraw";
} // namespace lux::render
