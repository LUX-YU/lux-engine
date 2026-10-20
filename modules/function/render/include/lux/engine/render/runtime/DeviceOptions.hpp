#pragma once

#include <cstdint>
#include <limits>
#include <span>

namespace lux::render
{
    struct DeviceOptions
    {
        std::span<const char* const> extensions{};
        // Explicit enumeration index, or prefer a discrete eligible device.
        std::uint32_t physical_device_index{std::numeric_limits<std::uint32_t>::max()};
        bool dynamic_rendering{true};
        bool multiple_queues{false};
        bool multiview{false};
        bool local_read{false};
        bool separate_depth_stencil{false};
        bool prefer_dedicated_queues{true};
        bool swapchain_maintenance{false};
        bool texture_compression_bc{false};
        bool texture_descriptors{false};
        bool host_query_reset{false};
        bool buffer_device_address{false};
        bool optional_texture_descriptors{false};
        bool optional_texture_compression_bc{false};
    };
} // namespace lux::render
