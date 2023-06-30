#pragma once
#include <lux/engine/platform/visibility.h>
#include "ContextVisitor.hpp"

// TODO move to RHI module

namespace lux::window
{
    class LuxWindowImpl;

    class GraphicContext
    {
    public:
        LUX_PLATFORM_PUBLIC virtual bool acceptVisitor(ContextVisitor* visitor) = 0;

        // invoke after window is been created
        LUX_PLATFORM_PUBLIC virtual bool apiInit() = 0;
        LUX_PLATFORM_PUBLIC virtual void cleanUp() = 0;
    };
} // namespace lux::window
