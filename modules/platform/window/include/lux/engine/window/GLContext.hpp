#pragma once
#include "GraphicContext.hpp"

namespace lux::window
{
    class GLContext : public GraphicContext
    {
    public:
        LUX_PLATFORM_PUBLIC GLContext(int major, int minor);

        LUX_PLATFORM_PUBLIC ~GLContext() override;
        
        /**
         * @return bool return true if success. if context already initialized, return true;
         * 
         */
        LUX_PLATFORM_PUBLIC bool acceptVisitor(ContextVisitor* visitor) override;

        LUX_PLATFORM_PUBLIC bool apiInit() override;

        LUX_PLATFORM_PUBLIC void cleanUp() override;

        LUX_PLATFORM_PUBLIC int  majorVersion() const;

        LUX_PLATFORM_PUBLIC int  minorVersion() const;

    private:
        bool    _init{false};
        int     _major_version;
        int     _minor_version;
    };
} // namespace lux::window
