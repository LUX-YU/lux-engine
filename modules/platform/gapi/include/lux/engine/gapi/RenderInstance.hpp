#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <string_view>
#include "lux/engine/gapi/visibility.h"

namespace lux::gapi
{
    class RenderDevice;

    class RenderInstance
    {
    public:
        using DeviceList = std::vector<std::shared_ptr<RenderDevice>>;

        LUX_PLATFORM_GAPI_PUBLIC virtual ~RenderInstance() {}

        LUX_PLATFORM_GAPI_PUBLIC virtual DeviceList supportedDevices() = 0;
    };
}
