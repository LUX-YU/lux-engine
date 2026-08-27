#pragma once
#include <string_view>
#include "lux/engine/gapi/visibility.h"

namespace lux::gapi
{
    enum class EDeviceType
    {
        OTHER = 0,
        INTEGRATED_GPU = 1,
        DISCRETE_GPU = 2,
        VIRTUAL_GPU = 3,
        CPU = 4
    };

    class RenderDevice
    {
    public:
        LUX_PLATFORM_GAPI_PUBLIC virtual ~RenderDevice()
        {
        }

        LUX_PLATFORM_GAPI_PUBLIC virtual EDeviceType deviceType() = 0;

        LUX_PLATFORM_GAPI_PUBLIC virtual std::string_view deviceName() = 0;

        bool isDeviceType(EDeviceType type)
        {
            return deviceType() == type;
        }
    };
}
