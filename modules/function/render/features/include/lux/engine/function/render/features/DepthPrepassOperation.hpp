#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>

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
    // No custom_create: the createFn this feature needs is the plain same-name
    // field copy the generator emits. It used to be hand-written, which meant a
    // new config field silently kept its default until someone remembered to add
    // the assignment by hand — the exact failure the generator exists to remove.
    // (DepthPrepassFeature::Config also carries `depth_target`, which the wire
    //  config deliberately does not expose; fields with no CommConfig counterpart
    //  keep their defaults, same as the hand-written version did.)
    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(
        prefix = DepthPrepass,
        id = lux.render.depth_prepass.v1,
        display = DepthPrepass,
        feature = DepthPrepassFeature,
        feature_header = lux / engine / render / renderer / features / DepthPrepassFeature.hpp) DepthPrepassCommConfig
    {
        ShaderHandle vertex_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle fragment_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
    };
    static_assert(std::is_trivially_copyable_v<DepthPrepassCommConfig>);

    /// DepthPrepass feature factory — registered at runtime via RegisterFeatureType.

} // namespace lux::render
