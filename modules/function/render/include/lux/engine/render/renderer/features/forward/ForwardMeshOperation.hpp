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

    inline constexpr uint32_t kForwardMeshCommConfigVersion = 3u;  // +graph_fragment
    inline constexpr uint32_t kForwardMeshDescriptorLayoutVersion = 0u;
    inline constexpr uint32_t kForwardMeshExtFlagHZB = 1u << 0;
    inline constexpr uint32_t kForwardMeshExtFlagBindless = 1u << 1;

    // =========================================================================
    //  Default shader name constants for ForwardMeshFeature
    // =========================================================================
    inline constexpr std::string_view kForwardCullShaderName         = "mesh_cull_unified.comp";
    inline constexpr std::string_view kForwardCompactShaderName      = "mdc_compact.comp";
    inline constexpr std::string_view kForwardVertShaderName         = "forward_mesh.vert";
    inline constexpr std::string_view kForwardUnlitFragShaderName    = "fr_unlit.frag";
    inline constexpr std::string_view kForwardPbrFragShaderName      = "fr_pbr.frag";
    inline constexpr std::string_view kForwardStylizedFragShaderName = "fr_stylized.frag";

    /// Comm-layer config for ForwardMeshFeature.
    /// Client fills shader indices from CompileShader replies.
    struct ForwardMeshCommConfig
    {
        ShaderHandle forward_cull_shader{};
        ShaderHandle forward_compact_shader{};
        ShaderHandle forward_vert_shader{};
        ShaderHandle unlit_fragment{};
        ShaderHandle pbr_fragment{};
        ShaderHandle stylized_fragment{};
        // Optional: node-graph (Graph family) forward frag. No builtin fallback —
        // null => Graph family gets no forward pipeline. Supplied via compileShader.
        ShaderHandle graph_fragment{};
        uint32_t comm_config_version{kForwardMeshCommConfigVersion};
        uint32_t descriptor_layout_version{kForwardMeshDescriptorLayoutVersion};
        uint32_t extension_flags{0};
    };
    static_assert(std::is_trivially_copyable_v<ForwardMeshCommConfig>);

    /// ForwardMesh feature factory — registered at runtime via RegisterFeatureType.
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kForwardMeshFeatureFactory;

} // namespace lux::render
