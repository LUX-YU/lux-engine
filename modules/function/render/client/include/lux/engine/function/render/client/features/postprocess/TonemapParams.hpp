#pragma once
// ============================================================================
//  TonemapParams.hpp — live-tunable tone-map parameters
//
//  The FIRST adopter of the RenderFeature param seam (feature-driven quality
//  system). A plain reflected struct (LUX_CLASS + LUX_MEMBER) so the editor's
//  SceneSettingsPanel enumerates it generically and quality tiers can pin
//  its fields — no per-feature UI code. Trivially copyable, so the same struct
//  doubles as the TonemapSetParams comm payload body.
//
//  Kept dependency-light (only <cstdint> + MetaAnnotations) so the editor-only
//  meta generator parses it cleanly.  See
//  .internal/feature-quality-tiers-design-2026-06-19.md.
// ============================================================================
#include <cstdint>

#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::render
{
    // 双标记:LUX_CLASS 喂编辑器反射(luxref),LUX_PASS_SCALARS 让 PassParams
    // 生成器把同一份布局烤进 GLSL PC 块与逐偏移断言(luxpass;经
    // TonemapPassParams.hpp 的 include + PARSE_INCLUDED_MARKED 可见)。
    struct LUX_CLASS() LUX_PASS_SCALARS() TonemapParams
    {
        LUX_MEMBER(display_name=Exposure, min=0.0, max=16.0, tooltip=HDR exposure multiplier applied before tone mapping)
        float exposure = 1.0f;

        LUX_MEMBER(display_name=Gamma, min=1.0, max=3.0, tooltip=Output gamma encoding (sRGB is ~2.2))
        float gamma = 2.2f;

        // Tone-map operator: 0=Reinhard, 1=ACES Filmic, 2=Uncharted2, 3=None.
        // Mirrors EToneMapOperator (TonemapFeature.hpp); a plain uint32 here so a
        // generic combo widget can drive it (INC-3 adds the labelled dropdown).
        LUX_MEMBER(display_name=Operator, min=0, max=3, tooltip=Tone-mapping curve: 0 Reinhard / 1 ACES / 2 Uncharted2 / 3 None)
        std::uint32_t tone_map_op = 1u;
    };

} // namespace lux::render
