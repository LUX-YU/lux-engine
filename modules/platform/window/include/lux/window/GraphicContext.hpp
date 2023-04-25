#pragma once
#include <lux/system/visibility_control.h>

// TODO move to RHI module

namespace lux::window
{
    class LuxWindowImpl;

    class GraphicContext
    {
    public:
        LUX_EXPORT virtual bool init() = 0;
        LUX_EXPORT virtual void makeCurrentContext(LuxWindowImpl*)  = 0;
    };
} // namespace lux::window
