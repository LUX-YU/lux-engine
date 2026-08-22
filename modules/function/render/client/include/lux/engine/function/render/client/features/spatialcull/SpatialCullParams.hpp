#pragma once
// ============================================================================
//  SpatialCullParams.hpp — live-tunable spatial coarse-cull parameters.
//
//  A reflected (LUX_CLASS + LUX_MEMBER) struct so the editor's
//  SceneSettingsPanel enumerates it generically (the feature-driven
//  quality seam, same machinery as TonemapParams) — no per-feature UI code.
//  These are a true image-quality-vs-performance trade-off (view distance / cell granularity), so
//  they belong in the settings panel. Trivially copyable, so the same struct
//  doubles as the generic SetFeatureParams comm payload body.
//
//  Kept dependency-light (only MetaAnnotations) so the editor-only meta
//  generator parses it cleanly. See
//  .internal/feature-classification-and-2d-coupling-2026-06-21.md §3 / §6.
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::render
{
    struct LUX_CLASS() SpatialCullParams
    {
        LUX_MEMBER(display_name=Cell Size, min=8.0, max=4096.0, tooltip=Spatial cell edge length in world units)
        float cell_size = 128.0f;

        LUX_MEMBER(display_name=Cull Distance, min=16.0, max=16384.0, tooltip=Cells farther than this from every active camera go dormant)
        float cull_distance = 512.0f;
    };

} // namespace lux::render
