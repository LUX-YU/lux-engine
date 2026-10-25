#pragma once

#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/storage/DirectLighting.hpp>
#include <lux/engine/render/features/ShadowInputs.hpp>

namespace lux::render
{
    struct ShadowLightSelection final
    {
        LightId light;
        EDirectLightingBuffer kind = EDirectLightingBuffer::DIRECTIONAL;
        std::vector<float> cascade_far_distances{25}; // Absolute camera-view distances; directional lights only.
        float near_distance = 0.05F;                  // Point/spot perspective near plane.
        float caster_distance = 100; // Directional near-plane extension towards the light, in world units.
        float normal_offset = 0;
        float comparison_offset = 0;
        float depth_bias_constant = 0, depth_bias_slope = 0; // Native raster bias units; not a depth/world offset.
        bool operator==(const ShadowLightSelection&) const noexcept = default;
    };

    struct ShadowMapFeatureConfig final
    {
        ShadowMapShape shape;
        std::string lights = "direct_lights";
        std::vector<ShadowLightSelection> casters;
        std::uint32_t camera_offset = 0; // ShadowCamera within the immutable attachment input for this frame.
        std::uint32_t pcf_radius = 1;
        bool operator==(const ShadowMapFeatureConfig&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<ShadowMapFeatureConfig> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const ShadowMapFeatureConfig&) noexcept;
    };

} // namespace lux::render
