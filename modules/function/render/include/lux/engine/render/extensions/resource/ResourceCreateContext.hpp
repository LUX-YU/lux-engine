#pragma once

#include <cstdint>

namespace lux::render::detail
{
    class DeviceSession;
    class DeviceEntry;
    class TransformStorage;
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

    private:
        friend class detail::DeviceEntry;
        friend class detail::TransformStorage;
        explicit ResourceCreateContext(detail::DeviceEntry&) noexcept;
        const detail::FrameProgress& retirement_progress_;
    };
} // namespace lux::render
