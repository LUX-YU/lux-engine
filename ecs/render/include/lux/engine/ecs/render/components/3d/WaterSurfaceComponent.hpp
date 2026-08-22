#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/asset/Asset.hpp>

#include <Eigen/Core>

namespace lux::ecs
{
    /// Authoring data for one finite water plane. Placement comes from the
    /// entity's ordinary Transform3DComponent and optional ParentComponent.
    struct LUX_COMPONENT() WaterSurfaceComponent final
    {
        LUX_MEMBER(display_name=Half Extent, min=0.01)
        Eigen::Vector2f half_extent{50.0f, 50.0f};

        LUX_MEMBER(display_name=Normal Scroll A)
        Eigen::Vector2f normal_scroll_a{0.015f, 0.006f};

        LUX_MEMBER(display_name=Normal Scroll B)
        Eigen::Vector2f normal_scroll_b{-0.008f, 0.012f};

        LUX_MEMBER(display_name=Absorption Color, color=true)
        Eigen::Vector3f absorption_color{0.03f, 0.16f, 0.19f};

        LUX_MEMBER(display_name=Absorption Distance, min=0.001)
        float absorption_distance{8.0f};

        LUX_MEMBER(display_name=Roughness, min=0.0, max=1.0)
        float roughness{0.12f};

        LUX_MEMBER(display_name=Normal Strength, min=0.0, max=4.0)
        float normal_strength{0.35f};

        LUX_MEMBER(display_name=Wave Scale, min=0.0001)
        float wave_scale{0.08f};

        LUX_MEMBER(display_name=Normal Texture, asset_type=texture)
        lux::asset::asset_id_t normal_texture{};
    };
} // namespace lux::ecs
