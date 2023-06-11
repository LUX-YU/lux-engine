#pragma once
#include <lux/engine/platform/visibility.h>

// TODO move to RHI module

namespace lux::window
{
    class LuxWindowImpl;

    class GraphicContext
    {
    public:
        LUX_PLATFORM_PUBLIC virtual bool init() = 0;
        LUX_PLATFORM_PUBLIC virtual void makeCurrentContext(LuxWindowImpl*)  = 0;
    };
} // namespace lux::window
