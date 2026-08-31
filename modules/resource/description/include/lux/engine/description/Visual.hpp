#pragma once

#include <lux/engine/resource/identity/AssetId.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace lux::rdesc
{
    struct MeshVisualDescription final
    {
        asset::AssetId mesh{};
        asset::AssetId material{};
        bool visible{true};
        bool cast_shadow{true};
        bool receive_shadow{true};

        friend bool operator==(const MeshVisualDescription&, const MeshVisualDescription&) noexcept = default;
    };

    enum class ELightType : std::uint8_t
    {
        DIRECTIONAL,
        POINT,
        SPOT,
        AREA,
    };

    inline constexpr std::size_t kLightCascadeSlots = 8U;

    struct LightDescription final
    {
        ELightType type{ELightType::POINT};
        std::array<float, 3> color{1.0F, 1.0F, 1.0F};
        float intensity{1.0F};
        float range{10.0F};
        float attenuation_constant{1.0F};
        float attenuation_linear{0.09F};
        float attenuation_quadratic{0.032F};
        float inner_cone_angle{0.5236F};
        float outer_cone_angle{0.7854F};
        std::array<float, 2> area_size{1.0F, 1.0F};
        bool cast_shadow{false};
        std::uint32_t shadow_map_size{1024U};
        float shadow_bias{0.005F};
        float shadow_normal_bias{0.01F};
        std::uint32_t cascade_count{4U};
        std::array<float, kLightCascadeSlots> cascade_splits{
            0.1F, 0.25F, 0.5F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F
        };

        friend bool operator==(const LightDescription&, const LightDescription&) noexcept = default;
    };
}
