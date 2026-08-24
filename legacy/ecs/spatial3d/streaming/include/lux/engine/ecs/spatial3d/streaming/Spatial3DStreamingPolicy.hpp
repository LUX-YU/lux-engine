#pragma once
/**
 * @file Spatial3DStreamingPolicy.hpp
 * @brief Lightweight Spatial3D residency policy shared by cook and runtime.
 */

#include <cstdint>
#include <string_view>

namespace lux::ecs::spatial3d::streaming
{
    inline constexpr std::string_view kResidentDemandChannelName =
        "lux.spatial3d.resident";
    inline constexpr std::string_view kVisualLodDemandChannelName =
        "lux.spatial3d.visual_lod";

    struct ResidencyCapacity final
    {
        std::uint64_t maximum_decoded_bytes{1024ull * 1024ull * 1024ull};
        std::uint64_t maximum_entities{1'000'000u};
        std::uint32_t maximum_interest_sources{8u};
        std::uint32_t maximum_sections_per_interest{4096u};

        [[nodiscard]] bool valid() const noexcept
        {
            return maximum_decoded_bytes != 0u &&
                maximum_entities != 0u &&
                maximum_interest_sources != 0u &&
                maximum_sections_per_interest != 0u;
        }

        friend bool operator==(const ResidencyCapacity&, const ResidencyCapacity&) =
            default;
    };
} // namespace lux::ecs::spatial3d::streaming
