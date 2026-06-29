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

    inline constexpr uint32_t kDeferredGBufferCommConfigVersion = 3u;  // +gbuffer_graph_fragment_shader
    inline constexpr uint32_t kDeferredGBufferDescriptorLayoutVersion = 0u;
    inline constexpr uint32_t kDeferredGBufferExtFlagHZB = 1u << 0;
    inline constexpr uint32_t kDeferredGBufferExtFlagBindless = 1u << 1;

    // =========================================================================
    //  Default shader name constants for DeferredGBufferFeature
    // =========================================================================
    inline constexpr std::string_view kGBufferVertShaderName         = "gbuffer.vert";
    inline constexpr std::string_view kGBufferUnlitFragShaderName    = "gbuffer_unlit.frag";
    inline constexpr std::string_view kGBufferPbrFragShaderName      = "gbuffer_pbr.frag";
    inline constexpr std::string_view kGBufferStylizedFragShaderName = "gbuffer_stylized.frag";
    inline constexpr std::string_view kGBufferCullShaderName         = "mesh_cull_unified.comp";
    inline constexpr std::string_view kGBufferCompactShaderName      = "mdc_compact.comp";

    /// Comm-layer config for DeferredGBufferFeature.
    /// Client fills shader indices from CompileShader replies.
    struct DeferredGBufferCommConfig
    {
        ShaderHandle gbuffer_vertex_shader{};
        ShaderHandle gbuffer_unlit_fragment_shader{};
        ShaderHandle gbuffer_pbr_fragment_shader{};
        ShaderHandle gbuffer_stylized_fragment_shader{};
        // Optional: node-graph (Graph family) gbuffer frag. No builtin fallback —
        // when null, the Graph family simply gets no pipeline (graph materials,
        // if any, won't draw). Supplied via the override conduit (compileShader).
        ShaderHandle gbuffer_graph_fragment_shader{};
        ShaderHandle cull_compute_shader{};
        // Deprecated: finalize path is removed at runtime; kept for wire compatibility.
        ShaderHandle finalize_compute_shader{};
        ShaderHandle compact_compute_shader{};
        uint32_t comm_config_version{kDeferredGBufferCommConfigVersion};
        uint32_t descriptor_layout_version{kDeferredGBufferDescriptorLayoutVersion};
        uint32_t extension_flags{0};
    };
    static_assert(std::is_trivially_copyable_v<DeferredGBufferCommConfig>);

    /// DeferredGBuffer feature factory — registered at runtime via RegisterFeatureType.
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kDeferredGBufferFeatureFactory;

} // namespace lux::render
