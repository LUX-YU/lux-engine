#pragma once

#include <lux/engine/RenderContext.hpp>

namespace lux::engine
{
    class EngineContext;
    // Cold complete assembly; device policy and diagnostics belong to the rendering facilities.
    [[nodiscard]] RenderContext::Result initializeRendering(
        EngineContext&,
        std::span<const char* const> instance_extensions = {}
    ) noexcept;
}
