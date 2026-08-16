#pragma once
/**
 * @file EShadowTechnique.hpp
 * @brief Shadow technique selector — drives variant pipelines + per-technique
 *        resources / passes.
 *
 * Each enumerator IS the integer ID injected as `-DLUX_SHADOW_TECHNIQUE_ID=N`
 * into the lighting fragment SPIR-V variant baker (see
 * deferred_lighting.frag / fr_pbr.frag / fr_stylized.frag). Adding a new
 * technique here means: bump kShadowTechniqueCount, add a variant suffix to
 * the cmake `_shadow_variants` list, add an embed prefix in
 * BuiltinShaderRegistry, and ship a new `<Name>ShadowTechnique` impl.
 *
 * Design rationale: .internal/plan/evsm-shadow-implementation-guide.md §2-§8.
 */

#include <cstdint>

namespace lux::render
{
    enum class EShadowTechnique : uint8_t
    {
        PCF  = 0, ///< Percentage-Closer Filtering — current default, depth-compare based.
        EVSM = 1, ///< Exponential Variance Shadow Maps — pre-filtered statistical, no bias tuning.
        // Reserved for future: MSM = 2, VSM = 3, PCSS = 4 ...
    };
    inline constexpr uint32_t kShadowTechniqueCount = 2;

    inline constexpr const char* toString(EShadowTechnique t) noexcept
    {
        switch (t) {
            case EShadowTechnique::PCF:  return "PCF";
            case EShadowTechnique::EVSM: return "EVSM";
        }
        return "Unknown";
    }
} // namespace lux::render
