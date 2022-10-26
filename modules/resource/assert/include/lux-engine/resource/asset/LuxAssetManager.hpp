#pragma once
#include <memory>
#include <lux-engine/platform/system/visibility_control.h>

namespace lux::engine::resource
{
    class LuxAssertManager
    {
    public:
        LUX_EXPORT LuxAssertManager();

        LUX_EXPORT ~LuxAssertManager();

        LUX_EXPORT void registAssert();
        
    private:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };
}
