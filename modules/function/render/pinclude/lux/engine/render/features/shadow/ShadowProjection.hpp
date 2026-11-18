#pragma once

#include <lux/engine/render/features/shadow/ShadowMapFeature.hpp>
#include <lux/engine/render/resources/instance/Data.hpp>

namespace lux::render::detail
{
    struct ShadowCamera final
    {
        TransformMatrix inverse_view_projection;
        std::array<float, 4> position_near, direction_far;
        bool operator==(const ShadowCamera&) const noexcept = default;
    };

    struct ShadowLightInput final
    {
        VDirectLight value;
        std::uint32_t dense_index = 0;
        bool active = false;
    };

    struct ShadowProjectionInput final
    {
        ShadowMapShape shape;
        std::span<const ShadowLightSelection> casters;
        std::uint32_t pcf_radius = 1;
    };

    // Pure projection math. Storage/Runtime/GPU ownership stays outside this calculation.
    [[nodiscard]] RenderResult<void> validateShadowConfiguration(const ShadowProjectionInput&) noexcept;
    [[nodiscard]] RenderResult<void> projectShadows(
        const ShadowProjectionInput&,
        const ShadowCamera&,
        std::span<const ShadowLightInput>,
        std::span<MeshVisibilityView>,
        std::span<ShadowSliceGpu>,
        std::span<LightShadowRange>,
        const std::array<std::uint32_t, 3>& offsets
    ) noexcept;
} // namespace lux::render::detail
