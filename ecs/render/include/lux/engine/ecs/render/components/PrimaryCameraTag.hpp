#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{
    /// Authored selection of the scene's primary camera. This is content
    /// intent, not render-session wiring; ViewPresentComponent remains the
    /// runtime-only statement that a camera currently drives an output.
    struct LUX_COMPONENT() PrimaryCameraTag final {};
}
