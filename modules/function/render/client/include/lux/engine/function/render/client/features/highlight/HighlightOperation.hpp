#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/features/GpuDrivenMeshExtFlags.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp> // FeatureFactory / GenericOkReply
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp> // 核心实例标志位(不相交断言)
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;

    /// Per-instance flag bit marking an instance as highlighted. OWNED by this
    /// feature, NOT the core protocol — it occupies bit 3 of the instance-flags word
    /// (RenderProtocol.hpp reserves bits 3+ for features). Single writer: the gameplay
    /// bridge adapter ORs it into the flags of highlighted entities; HighlightFeature's
    /// mask shader (highlight_mask.frag) tests it. The core renderer never decodes it,
    /// so a consumer that wants no highlight simply never sets the bit and omits this
    /// feature. The editor's "selection" is just one client that drives this bit.
    inline constexpr uint32_t kInstanceFlagHighlight = 1u << 3;
    static_assert(
        (kInstanceFlagHighlight & (kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible)) == 0,
        "feature instance-flag must not overlap core protocol bits"
        " (register new bits in RenderProtocol.hpp's ledger)");

    /// Comm-layer config for HighlightFeature. All shader handles may be left
    /// null — HighlightFeature::init() defaults them to the builtins. The halo
    /// appearance defaults (UE-ish orange) are baked here so a `{}` config works.
    /// The halo color is the per-feature client knob (selection = orange, etc.).
    struct LUX_COMM_CONFIG(
        prefix = Highlight,
        id = lux.render.highlight.v1,
        display = Highlight,
        requires = lux.render.mesh_stack.v1,
        custom_create = true) HighlightCommConfig
    {
        ShaderHandle cull_shader{};
        ShaderHandle compact_shader{};
        ShaderHandle mask_vert{};
        ShaderHandle mask_frag{};
        ShaderHandle blur_frag{};
        ShaderHandle composite_frag{};
        uint32_t descriptor_layout_version{0};
        GpuDrivenMeshExtFlags extension_flags{};
        // Halo appearance
        float glow_color[3]{1.0f, 0.55f, 0.06f}; ///< UE-ish orange
        float glow_intensity{3.0f};              ///< scales the halo alpha
        float glow_radius{2.5f};                 ///< Gaussian per-tap step, in texels
    };
    static_assert(std::is_trivially_copyable_v<HighlightCommConfig>);

    /// Highlight-outline feature factory (object highlight; editor selection is one client).

} // namespace lux::render
