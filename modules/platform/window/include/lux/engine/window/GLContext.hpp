#pragma once
#include "GraphicContext.hpp"

namespace lux::window
{
    class GLContext : public GraphicContext
    {
    public:
        LUX_PLATFORM_PUBLIC GLContext(int major, int minor);
        
        /**
         * @return bool return true if success. if context already initialized, return true;
         * 
        */
        LUX_PLATFORM_PUBLIC bool init() override;
        LUX_PLATFORM_PUBLIC void makeCurrentContext(LuxWindowImpl*)  override;

    private:
        bool    _init{false};
        int     _major_version;
        int     _minor_version;
    };
} // namespace lux::window
