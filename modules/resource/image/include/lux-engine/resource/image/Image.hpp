#pragma once
#include <string>
#include <lux-engine/platform/system/visibility_control.h>
#include <lux-engine/resource/asset/LuxAsset.hpp>
#include <memory>

namespace lux::engine::resource
{
    class Image : public LuxAsset
    {
    public:
        LUX_EXPORT Image(std::string path, bool flip_vertically = true);

        LUX_EXPORT bool isEnable() const;

        LUX_EXPORT int  width() const;

        LUX_EXPORT int  height() const;

        LUX_EXPORT int  channel() const;

        LUX_EXPORT void* data();

    private:

        class Impl;
        std::shared_ptr<Impl> _impl;
    };
} // namespace lux::engine::platform
