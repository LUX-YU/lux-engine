#pragma once

namespace lux::render::detail
{
    class DeviceSession;
    class ResourceUploader;
    class FrameExecution;
} // namespace lux::render::detail

namespace lux::render
{
    class ResourceRegistry;

    // Synchronous factory input. Instances retain only the dependencies they actually use.
    struct StorageCreateContext final
    {
        detail::DeviceSession& device;
        detail::ResourceUploader* uploads;
        detail::FrameExecution* frames;
        ResourceRegistry& resources;
        bool paged_instances = false;
    };
} // namespace lux::render
