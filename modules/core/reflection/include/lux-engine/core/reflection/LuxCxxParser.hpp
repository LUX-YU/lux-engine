#pragma once
#include <memory>
#include <lux-engine/platform/system/visibility_control.h>

namespace lux::engine::core
{
    class LuxCxxParserImpl;
    
    class LuxCxxParser
    {
    public:
        LUX_EXPORT LuxCxxParser();

        LUX_EXPORT ~LuxCxxParser();

    private:
        std::unique_ptr<LuxCxxParserImpl> _impl;
    };
} // namespace lux::engine::core
