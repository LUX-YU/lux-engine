#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace lux::render
{
    enum class EGraphicsBackend : std::uint8_t
    {
        VULKAN,
        D3D12
    };

    struct GraphQueueCosts final
    {
        std::uint64_t graphics_submit_ns = 0;
        std::uint64_t compute_submit_ns = 0;
        std::uint64_t queue_handoff_ns = 0;
    };

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
        // Texture, sampler and material-buffer layout limits. Zero selects the device limit minus reserve.
        std::array<std::uint32_t, 3> descriptor_layout_limits{};
        // Physical allocation grows independently from the immutable layout limit.
        std::array<std::uint32_t, 3> descriptor_initial_counts{256, 16, 64};
        std::array<std::uint32_t, 3> private_descriptor_reserve{0, 0, 4};
        // Number of zero-user native programs retained; active programs are not constrained by this budget.
        std::uint32_t cached_unused_programs = 256;
        EGraphicsBackend backend = EGraphicsBackend::VULKAN;
        // Measured on this adapter/backend. A zero submit cost marks calibration as unavailable.
        GraphQueueCosts graph_queue_costs;
    };
} // namespace lux::render
