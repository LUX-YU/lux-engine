#pragma once

#include <lux/engine/render/features/ShadowMapFeature.hpp>

namespace lux::render::detail
{
    struct ShadowLightInput final
    {
        VDirectLight value;
        std::uint32_t dense_index = 0;
        bool active = false;
    };

    // Pure projection math. Storage/Runtime/GPU ownership stays outside this calculation.
    [[nodiscard]] RenderResult<void> validateShadowConfiguration(const ShadowMapFeatureConfig&) noexcept;
    [[nodiscard]] RenderResult<void> projectShadows(
        const ShadowMapFeatureConfig&,
        const ShadowCamera&,
        std::span<const ShadowLightInput>,
        std::span<MeshVisibilityView>,
        std::span<ShadowSliceGpu>,
        std::span<LightShadowRange>,
        const std::array<std::uint32_t, 3>& offsets
    ) noexcept;
} // namespace lux::render::detail
