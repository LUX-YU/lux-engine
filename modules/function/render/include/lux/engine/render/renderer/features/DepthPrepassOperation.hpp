#pragma once
#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>

#include <cstdint>
#include <type_traits>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;

    // =========================================================================
    //  Default shader name constants for DepthPrepassFeature
    // =========================================================================
    inline constexpr std::string_view kDepthPrepassFragShaderName = "depth_only.frag";

    /// Comm-layer config for DepthPrepassFeature.
    struct DepthPrepassCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
    };
    static_assert(std::is_trivially_copyable_v<DepthPrepassCommConfig>);

    /// DepthPrepass feature factory — registered at runtime via RegisterFeatureType.
    extern const FeatureFactory kDepthPrepassFeatureFactory;

} // namespace lux::render
