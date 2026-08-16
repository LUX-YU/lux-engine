#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>

#include <Eigen/Core>

namespace lux::ecs
{
    /// Scene-wide exponential height fog authored as ordinary ECS content.
    /// A presentation contribution accepts zero or one such component. Runtime
    /// render handles and revision counters deliberately live outside it.
    struct LUX_COMPONENT() HeightFogComponent final
    {
        LUX_MEMBER(display_name=Enabled)
        bool enabled{false};

        LUX_MEMBER(display_name=Color, color=true, tooltip=Linear RGB in-scattering color)
        Eigen::Vector3f color{0.55f, 0.62f, 0.70f};

        LUX_MEMBER(display_name=Density, min=0.0)
        float density{0.0002f};

        LUX_MEMBER(display_name=Start Distance, min=0.0)
        float start_distance{0.0f};

        LUX_MEMBER(display_name=Reference Height)
        float reference_height{0.0f};

        LUX_MEMBER(display_name=Height Falloff, min=0.0)
        float height_falloff{0.01f};

        LUX_MEMBER(display_name=Maximum Opacity, min=0.0, max=1.0)
        float maximum_opacity{0.98f};
    };
}
