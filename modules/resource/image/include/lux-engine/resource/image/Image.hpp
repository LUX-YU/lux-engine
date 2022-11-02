#pragma once
#include <string>
#include <lux-engine/platform/system/visibility_control.h>
#include <lux-engine/resource/asset/LuxAsset.hpp>

namespace lux::engine::resource
{
    class Image : public LuxAsset
    {
    public:
        LUX_EXPORT Image(std::string path, bool flip_vertically = true);

        LUX_EXPORT ~Image();

        LUX_EXPORT bool isEnable();

        LUX_EXPORT int  width();

        LUX_EXPORT int  height();

        LUX_EXPORT int  channel();

        LUX_EXPORT void* data();

    private:
        void load(const char*);

        void*   _data{nullptr};
        int     _width;
        int     _height;
        int     _channel;
    };
} // namespace lux::engine::platform
