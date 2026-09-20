#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/features/GpuDrivenMeshExtFlags.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp> // FeatureFactory / GenericOkReply
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/features/resources/mesh/RenderObjectTypes.hpp> // 核心实例标志位(不相交断言)
#include <lux/engine/function/render/features/client_visibility.h>

#include <lux/engine/function/render/client/core/RenderEntityId.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;

    /// Comm-layer config for HighlightFeature. All shader handles may be left
    /// null — HighlightFeature::init() defaults them to the builtins. The halo
    /// appearance defaults (UE-ish orange) are baked here so a `{}` config works.
    /// The halo color is the per-feature client knob (selection = orange, etc.).
    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(
        prefix = Highlight,
        id = lux.render.highlight.v1,
        display = Highlight,
        requires = lux.render.mesh_stack.v1,
        custom_create = true) HighlightCommConfig
    {
        ShaderHandle cull_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle compact_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle mask_vert LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle mask_frag LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle blur_frag LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle composite_frag LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        uint32_t descriptor_layout_version{0};
        GpuDrivenMeshExtFlags extension_flags{};
        // Halo appearance
        float LUX_NO_MEMBER() glow_color[3]{1.0f, 0.55f, 0.06f}; ///< UE-ish orange
        float glow_intensity{3.0f};              ///< scales the halo alpha
        float glow_radius{2.5f};                 ///< Gaussian per-tap step, in texels
    };
    static_assert(std::is_trivially_copyable_v<HighlightCommConfig>);

    // One owned blob replaces the whole set. An empty blob clears it.
    struct LUX_OP(lane = program, kind = blob, name = HighlightReplaceTargets, method = replaceTargets)
        HighlightReplaceTargetsPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        LUX_OP_BLOB() BlobRef targets{};
    };
    static_assert(std::is_trivially_copyable_v<HighlightReplaceTargetsPayload>);

} // namespace lux::render
