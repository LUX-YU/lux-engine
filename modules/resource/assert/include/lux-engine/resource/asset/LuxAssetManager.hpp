#pragma once
#include "LuxAsset.hpp"

#include <memory>
#include <lux-engine/platform/system/visibility_control.h>

namespace lux::engine::resource
{
    class LuxAssetManager
    {
    public:
        LUX_EXPORT LuxAssetManager();

        LUX_EXPORT ~LuxAssetManager();

        LUX_EXPORT void registAssert();
        
    private:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };
}
