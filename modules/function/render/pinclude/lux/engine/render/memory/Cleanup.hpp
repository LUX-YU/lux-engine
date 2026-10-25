#pragma once

#include <lux/engine/render/extensions/device/DeviceSession.hpp>

namespace lux::render::detail
{
    inline void releaseAfterGpu(DeviceSession& session, nri::Buffer*& buffer) noexcept
    {
        if (!buffer)
        {
            return;
        }
        session.deferRelease([value = std::exchange(buffer, nullptr),
                              destroy = session.nriCore().DestroyBuffer]() noexcept { destroy(value); });
    }
} // namespace lux::render::detail
