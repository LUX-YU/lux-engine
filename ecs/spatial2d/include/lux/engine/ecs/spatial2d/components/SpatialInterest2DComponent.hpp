#pragma once
/**
 * @file SpatialInterest2DComponent.hpp
 * @brief User-authored two-dimensional content-residency interest facts.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstdint>

namespace lux::ecs
{
    /// The resolved transform supplies the current center. This component is
    /// pure selection intent; chunk coordinates, Section tickets and runtime
    /// generations remain private to the spatial 2D capability.
    struct LUX_COMPONENT() SpatialInterest2DComponent final
    {
        LUX_MEMBER(display_name=Enabled)
        bool enabled{true};

        /// Base priority for the active window. The resident-only halo uses a
        /// lower, implementation-owned priority derived from this value.
        LUX_MEMBER(display_name=Priority)
        std::uint32_t priority{100u};
    };
}
