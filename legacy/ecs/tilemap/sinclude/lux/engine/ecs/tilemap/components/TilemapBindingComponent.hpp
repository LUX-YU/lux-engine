#pragma once

#include <lux/engine/ecs/tilemap/TilemapTypes.hpp>

namespace lux::ecs
{
    /// Scene-local, non-reflected binding owned by TilemapRuntime.
    /// Authored TilemapComponent data never exposes this handle.
    struct TilemapBindingComponent final
    {
        TilemapHandle runtime;
        /// System-owned bindings are retired by TilemapSystem. Explicit
        /// fixture/content adapters can borrow a pre-existing runtime handle
        /// by leaving this false; observer fold-in never destroys it.
        bool owned_by_system{false};
    };
} // namespace lux::ecs
