#pragma once
/**
 * @file SpatialInterest3DComponent.hpp
 * @brief User-authored three-dimensional content-residency interest facts.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstdint>

namespace lux::ecs
{
    /// The resolved transform supplies the current center. Distances and
    /// prediction offsets are ordinary scene facts; cells, Section tickets
    /// and runtime generations stay private to the spatial 3D capability.
    struct LUX_COMPONENT() SpatialInterest3DComponent final
    {
        LUX_MEMBER(display_name=Enabled)
        bool enabled{true};

        LUX_MEMBER(display_name=ActiveDistance, min=0.0)
        double active_distance{64.0};

        LUX_MEMBER(display_name=ResidentDistance, min=0.0)
        double resident_distance{128.0};

        LUX_MEMBER(display_name=PredictionOffsetX)
        double prediction_offset_x{0.0};
        LUX_MEMBER(display_name=PredictionOffsetY)
        double prediction_offset_y{0.0};
        LUX_MEMBER(display_name=PredictionOffsetZ)
        double prediction_offset_z{0.0};

        LUX_MEMBER(display_name=ActivePriority, min=1)
        std::uint32_t active_priority{100u};
        LUX_MEMBER(display_name=ResidentPriority, min=1)
        std::uint32_t resident_priority{1u};
    };
}
