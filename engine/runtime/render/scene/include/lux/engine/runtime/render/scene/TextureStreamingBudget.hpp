#pragma once

#include <cstdint>

namespace lux::runtime
{
    /**
     * Runtime-only texture streaming maintenance budget.
     *
     * Deployment/platform composition may override these values without
     * changing cooked resource identity. Render and Residency code must not
     * infer a product profile or operating system from this contract.
     */
    struct TextureStreamingBudget final
    {
        std::uint32_t query_interval_frames{4u};
        std::uint32_t maximum_demand_entries{64u};
        std::uint32_t maximum_replacement_tasks{8u};
        std::uint64_t maximum_replacement_bytes{16u * 1024u * 1024u};
    };
} // namespace lux::runtime
