#pragma once
#include <string>
#include <lux-engine/platform/cxx/visibility_control.h>

namespace lux::engine::platform
{
    class Image
    {
    public:
        LUX_EXPORT Image(std::string path, bool flip_vertically = true);

        LUX_EXPORT ~Image();

        LUX_EXPORT bool isEnable();

        LUX_EXPORT int width();

        LUX_EXPORT int height();

        LUX_EXPORT int channel();

        LUX_EXPORT void* data();

    private:
        void load(const char*);

        void* _data{nullptr};
        int _width;
        int _height;
        int _channel;
    };

    class ImageLoader
    {
    public:
        ImageLoader();

        ~ImageLoader();
    };
} // namespace lux::engine::platform
