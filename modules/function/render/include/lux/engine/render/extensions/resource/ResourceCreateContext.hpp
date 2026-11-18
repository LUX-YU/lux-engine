#pragma once

#include <cstdint>

namespace lux::render::detail
{
    class DeviceSession;
    class FrameProgress;
    class ResourceUploader;
} // namespace lux::render::detail

namespace lux::render
{
    class ResourceRegistry;

    // Synchronous factory input. Instances retain only the dependencies they actually use.
    struct ResourceCreateContext final
    {
        detail::DeviceSession& device;
        detail::ResourceUploader* uploads;
        std::uint32_t frame_count;
        ResourceRegistry& resources;
        detail::FrameProgress* replica_progress = nullptr;
    };
} // namespace lux::render
