#pragma once

#include <lux/engine/render/device/DeviceSession.hpp>

namespace lux::render::detail
{
    inline void releaseAfterGpu(DeviceSession& session, gapi::vk::VmaBuffer& buffer) noexcept
    {
        if (!buffer)
        {
            return;
        }
        session.deferRelease([value = std::move(buffer), allocator = session.allocator().native()]() mutable noexcept
                             { value.release(allocator); });
    }
} // namespace lux::render::detail
