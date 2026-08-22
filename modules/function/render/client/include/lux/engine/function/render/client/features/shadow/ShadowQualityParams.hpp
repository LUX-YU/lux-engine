#pragma once
// ============================================================================
//  ShadowQualityParams.hpp — live-tunable shadow quality parameters.
//
//  A reflected (LUX_CLASS + LUX_MEMBER) struct so the editor's
//  SceneSettingsPanel enumerates it generically (the feature-driven
//  quality seam, same machinery as TonemapParams / SpatialCullParams) — no
//  per-feature UI code, and quality tiers can pin its fields. Trivially
//  copyable, so the same struct doubles as the generic SetFeatureParams comm
//  payload body pushed through the shared FeatureParamsProxy.
//
//  Kept dependency-light (only <cstdint> + MetaAnnotations) so the editor-only
//  meta generator parses it cleanly — the literal defaults mirror the kDefault*
//  constants in ShadowMapTypes.hpp (NOT included here to stay light); the
//  authoritative live values come from the feature's config via paramData().
//  See .internal/feature-quality-tiers-design-2026-06-19.md.
// ============================================================================
#include <cstdint>

#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::render
{
    struct LUX_CLASS() ShadowQualityParams
    {
        LUX_MEMBER(display_name=Atlas Page Resolution, min=256, max=8192, tooltip=Shadow atlas page edge in texels (atlas rebuild on change))
        std::uint32_t atlas_page_resolution = 4096u;   ///< kDefaultShadowAtlasPageResolution

        LUX_MEMBER(display_name=Atlas Page Count, min=1, max=32, tooltip=Number of atlas pages (atlas rebuild on change))
        std::uint32_t atlas_page_count = 4u;           ///< kDefaultShadowAtlasPageCount

        LUX_MEMBER(display_name=Max Shadow Slices, min=1, max=512, tooltip=Slice budget across all shadow-casting lights (atlas rebuild on change))
        std::uint32_t max_shadow_slices = 128u;        ///< kDefaultMaxShadowSlices

        LUX_MEMBER(display_name=Non-Directional Max Distance, min=0.0, max=1000.0, tooltip=Spot/point shadows beyond this distance are skipped (0 = no limit))
        float non_directional_shadow_max_distance = 60.0f;

        LUX_MEMBER(display_name=Directional CSM, min=0, max=1, tooltip=0 = single directional slice / 1 = cascaded shadow maps)
        std::uint32_t enable_directional_csm = 0u;
    };

} // namespace lux::render
