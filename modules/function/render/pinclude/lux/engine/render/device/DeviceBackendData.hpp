#pragma once

#include <Extensions/NRIWrapperD3D12.h>
#include <Extensions/NRIWrapperVK.h>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>

namespace lux::render::detail
{
    struct DeviceSession::BackendData final
    {
        nri::WrapperVKInterface vulkan{};
        nri::WrapperD3D12Interface d3d12{};
    };
} // namespace lux::render::detail
