#pragma once

#include <lux/engine/ecs/pixel/PixelFieldTypes.hpp>

namespace lux::ecs
{
    /// Scene-local, non-reflected binding owned by PixelFieldRuntime.
    /// Authored PixelField2DComponent data never exposes this handle.
    struct PixelFieldBindingComponent final
    {
        PixelFieldHandle field;
        /// True only for handles created by PixelFieldSystem from authored
        /// facts. Legacy/content leaf adapters can bind an existing backing
        /// field with false; observer fold-in will never create or destroy it.
        bool owned_by_system{false};
    };
} // namespace lux::ecs
