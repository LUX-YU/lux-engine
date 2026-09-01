#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/features/GpuDrivenMeshExtFlags.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    /// Feature-owned instance bit.  The mesh bridge is the only writer of the
    /// complete flag word; the feature only reads this bit.
    inline constexpr std::uint32_t kInstanceFlagStreamingFeedback = 1u << 4;
    static_assert(
        (kInstanceFlagStreamingFeedback &
         (kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible)) == 0,
        "streaming feedback bit overlaps core instance flags");

    enum class EStreamingFeedbackPattern : std::uint32_t
    {
        MOSAIC_DITHER = 0,
        DIAGONAL_DITHER = 1
    };

    /// Scene-level default style.  Entity state carries a stable style id; v1
    /// renders the built-in ids through this default implementation.  A plugin
    /// can replace the feature factory without changing ECS data.
    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(
        prefix = StreamingFeedback,
        id = lux.render.streaming_feedback.v1,
        display = StreamingFeedback,
        requires = lux.render.mesh_stack.v1,
        custom_create = true) StreamingFeedbackCommConfig
    {
        ShaderHandle cull_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle compact_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle mask_vert LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle mask_frag LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle composite_frag LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        std::uint32_t descriptor_layout_version{0};
        GpuDrivenMeshExtFlags extension_flags{};
        float tile_size{18.0f};
        float speed{1.6f};
        float intensity{0.72f};
        float LUX_NO_MEMBER() color[3]{0.18f, 0.72f, 1.0f};
        EStreamingFeedbackPattern pattern{EStreamingFeedbackPattern::MOSAIC_DITHER};
    };
    static_assert(std::is_trivially_copyable_v<StreamingFeedbackCommConfig>);
}
