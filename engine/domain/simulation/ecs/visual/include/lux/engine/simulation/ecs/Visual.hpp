#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

#include <Eigen/Core>

#include <array>
#include <cstdint>

namespace lux::simulation::ecs
{
    struct LUX_COMPONENT(schema = "lux.ecs.Mesh3D", version = 1, snapshot = COPY) Mesh3D final
    {
        asset::AssetId mesh{};
        asset::AssetId material{};
        bool visible{true};
        bool cast_shadow{true};
        bool receive_shadow{true};
        bool two_sided_shadow{false};
    };

    enum class ELight3DType : std::uint8_t
    {
        DIRECTIONAL,
        POINT,
        SPOT,
        AREA,
    };

    struct LUX_COMPONENT(schema = "lux.ecs.Light3D", version = 1, snapshot = COPY) Light3D final
    {
        ELight3DType type{ELight3DType::POINT};
        Eigen::Vector3f color{Eigen::Vector3f::Ones()};
        float intensity{1.0F};
        float range{10.0F};
        float attenuation_constant{1.0F};
        float attenuation_linear{0.09F};
        float attenuation_quadratic{0.032F};
        float inner_cone_angle{0.5236F};
        float outer_cone_angle{0.7854F};
        Eigen::Vector2f area_size{Eigen::Vector2f::Ones()};
        std::uint32_t flags{0U};
        std::uint32_t shadow_map_size{1024U};
        float shadow_bias{0.005F};
        float shadow_normal_bias{0.01F};
        std::uint32_t cascade_count{4U};
        std::array<float, 8> cascade_splits{0.1F, 0.25F, 0.5F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    };
} // namespace lux::simulation::ecs

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/simulation/ecs/Visual.type_static_info.hpp>
#endif
