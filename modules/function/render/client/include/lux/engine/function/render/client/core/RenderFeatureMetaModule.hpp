#pragma once

#include <lux/engine/function/visibility.h>

namespace lux::render
{
    // Calling this exported anchor ensures the generated runtime-reflection
    // sidecar is loaded before ReflectionRegistry initialization.
    LUX_FUNCTION_PUBLIC void initializeBuiltinRenderFeatureMeta() noexcept;
} // namespace lux::render
