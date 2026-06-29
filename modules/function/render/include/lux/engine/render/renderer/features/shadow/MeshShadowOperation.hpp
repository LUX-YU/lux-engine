#pragma once
#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;

    inline constexpr uint32_t kMeshShadowCommConfigVersion = 2u;
    inline constexpr uint32_t kMeshShadowDescriptorLayoutVersion = 0u;
    inline constexpr uint32_t kMeshShadowExtFlagHZB = 1u << 0;
    inline constexpr uint32_t kMeshShadowExtFlagBindless = 1u << 1;

    // =========================================================================
    //  Default shader name constants for MeshShadowFeature
    // =========================================================================
    inline constexpr std::string_view kMeshShadowCullShaderName    = "mesh_cull_unified.comp";
    inline constexpr std::string_view kMeshShadowCompactShaderName = "mdc_compact.comp";
    inline constexpr std::string_view kMeshShadowClearShaderName   = "clear_count_buffers.comp";
    // Depth-only _vp vert matching shadow_depth.frag's interface (vUV only) while
    // using the same bindless pool path. The fat `mesh_shadow_vp.vert` carries
    // vShadowNear / vShadowFar / vDepthPersp for the EVSM caster and is selected
    // via EVSMShadowTechnique::casterVertVariant() inside
    // MeshShadowFeature::ensureCasterPipeline(), independent of this default.
    inline constexpr std::string_view kMeshShadowVertShaderName    = "shadow_depth_vp.vert";
    inline constexpr std::string_view kMeshShadowFragShaderName    = "shadow_depth.frag";

    /// Comm-layer config for MeshShadowFeature.
    /// Client fills shader indices from CompileShader replies.
    struct MeshShadowCommConfig
    {
        ShaderHandle shadow_cull_shader{};
        // Deprecated: finalize path is removed at runtime; kept for wire compatibility.
        ShaderHandle shadow_finalize_shader{};
        ShaderHandle shadow_compact_shader{};
        ShaderHandle shadow_clear_shader{};
        ShaderHandle shadow_vert_shader{};
        ShaderHandle shadow_frag_shader{};
        uint32_t comm_config_version{kMeshShadowCommConfigVersion};
        uint32_t descriptor_layout_version{kMeshShadowDescriptorLayoutVersion};
        uint32_t extension_flags{0};
    };
    static_assert(std::is_trivially_copyable_v<MeshShadowCommConfig>);

    /// MeshShadow feature factory — registered at runtime via RegisterFeatureType.
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kMeshShadowFeatureFactory;

} // namespace lux::render
