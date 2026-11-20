#pragma once


#include <array>
#include <lux/engine/description/LightDescription.hpp>
#include <lux/engine/render/core/ResourceId.hpp>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace lux::render
{
    inline constexpr std::string_view kDirectLightsInput = "lux.render.input.direct_lights";

    enum class EDirectLightingBuffer : std::uint8_t
    {
        DIRECTIONAL,
        POINT,
        SPOT,
        COUNTS
    };

    // Scene-local world space; linear radiance. Directions are unit vectors pointing from the light.
    struct DirectionalLightGpu final
    {
        std::array<float, 4> radiance{1, 1, 1, 0};
        std::array<float, 4> direction{0, 0, -1, 0};
    };

    struct PointLightGpu final
    {
        std::array<float, 4> position_range{0, 0, 1, 10};
        std::array<float, 4> radiance{1, 1, 1, 0};
        std::array<float, 4> attenuation{1, 0, 1, 0};
    };

    struct SpotLightGpu final
    {
        std::array<float, 4> position_range{0, 0, 1, 10};
        std::array<float, 4> radiance{1, 1, 1, 0};
        std::array<float, 4> direction_outer{0, 0, -1, 0.7F};
        std::array<float, 4> attenuation_inner{1, 0, 1, 0.9F};
    };

    static_assert(sizeof(DirectionalLightGpu) == 32 && sizeof(PointLightGpu) == 48 && sizeof(SpotLightGpu) == 64);
    using VDirectLight = std::variant<DirectionalLightGpu, PointLightGpu, SpotLightGpu>;
    struct DirectLightTag;
    using LightId = ResourceId<DirectLightTag>;

    struct LightShadowSettings final
    {
        bool enabled = false;
        float bias = 0, normal_bias = 0;
        float depth_bias_constant = 0, depth_bias_slope = 0;
        std::uint32_t cascade_count = 1;
        std::array<float, rdesc::kLightCascadeSlots> cascade_splits{1};
        bool operator==(const LightShadowSettings&) const noexcept = default;
    };

    struct DirectLightUpdate final
    {
        LightId light;
        VDirectLight value;
        LightShadowSettings shadows;
    };

    struct ShadowLightRecord final
    {
        LightId id;
        VDirectLight value; // Spatial projection fields. Shading-only fields use their default values.
        LightShadowSettings settings;
        std::uint32_t dense_index = 0;
    };

    struct ShadowLightSnapshot final
    {
        // All lighting records contribute to the dense lighting-table offsets.
        std::array<std::uint32_t, 3> counts{};
        std::vector<ShadowLightRecord> casters;
        std::uint64_t revision = 0;
    };

    struct LightData final
    {
        std::uint64_t sequence = 0;
        std::span<const VDirectLight> values;
        std::span<const LightShadowSettings> shadows; // Empty explicitly disables shadows for this creation.
    };

} // namespace lux::render
