#pragma once

#include <cstdint>
#include <limits>

namespace lux::render
{
    struct DeviceOptions
    {
        // Explicit enumeration index, or prefer a discrete eligible device.
        std::uint32_t physical_device_index{std::numeric_limits<std::uint32_t>::max()};
        bool multiple_queues{false};
        bool multiview{false};
        bool local_read{false};
        bool prefer_dedicated_queues{true};
        bool texture_compression_bc{false};
        bool texture_descriptors{false};
        // Select paged instance streams; false retains the contiguous-buffer path.
        bool buffer_device_address{false};
    };
} // namespace lux::render
