#pragma once
/**
 * @file Navigation.hpp
 * @brief Backend-independent navigation requests and results.
 *
 * This vocabulary describes an algorithmic problem.  Content lifecycle and
 * backend storage terminology deliberately stay outside this contract.
 */

#include <lux/engine/resource/spatial/Spatial.hpp>

#include <cmath>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lux::navigation
{
    struct NavigationRegionId final
    {
        std::uint64_t high{0u};
        std::uint64_t low{0u};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return high != 0u || low != 0u;
        }

        friend bool operator==(const NavigationRegionId&, const NavigationRegionId&) = default;
        friend auto operator<=>(const NavigationRegionId&, const NavigationRegionId&) = default;
    };

    struct NavigationPortalId final
    {
        std::uint64_t high{0u};
        std::uint64_t low{0u};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return high != 0u || low != 0u;
        }

        friend bool operator==(const NavigationPortalId&, const NavigationPortalId&) = default;
    };

    /// Physical limits used by a navigation implementation to choose a
    /// compatible representation.  They describe the actor, not a backend.
    struct NavigationAgentConstraints final
    {
        float radius{0.5f};
        float height{1.8f};
        float maximum_climb{0.5f};
        float maximum_slope_degrees{45.0f};

        friend bool operator==(const NavigationAgentConstraints&, const NavigationAgentConstraints&) = default;
    };

    /// A semantic connection between two independently resident regions.
    struct NavigationPortal final
    {
        NavigationPortalId id;
        NavigationRegionId first_region;
        NavigationRegionId second_region;
        lux::spatial::Position3D first_position;
        lux::spatial::Position3D second_position;
        float traversal_cost_scale{1.0f};
        bool bidirectional{true};
    };

    enum class ENavigationPathStatus : std::uint8_t
    {
        COMPLETE,
        PARTIAL,
        PENDING,
        FAILED
    };

    enum class ENavigationPathFailure : std::uint8_t
    {
        NONE,
        INVALID_REQUEST,
        UNSUPPORTED_AGENT,
        LOCATION_NOT_FOUND,
        SEARCH_CAPACITY_EXHAUSTED,
        BACKEND_FAILURE
    };

    struct NavigationPathRequest final
    {
        lux::spatial::Position3D start;
        lux::spatial::Position3D destination;
        NavigationAgentConstraints agent;
        std::optional<NavigationRegionId> start_region;
        std::optional<NavigationRegionId> destination_region;
        float nearest_horizontal_extent{2.0f};
        float nearest_vertical_extent{4.0f};
        std::uint32_t maximum_path_nodes{2048u};
        std::uint32_t maximum_path_points{512u};
    };

    struct NavigationPathResult final
    {
        ENavigationPathStatus status{ENavigationPathStatus::FAILED};
        ENavigationPathFailure failure{ENavigationPathFailure::NONE};
        std::vector<lux::spatial::Position3D> points;
        std::vector<NavigationRegionId> missing_regions;
        std::string detail;
        std::uint64_t generation{0u};
    };

    [[nodiscard]] inline bool
    valid(const NavigationAgentConstraints& value) noexcept
    {
        return std::isfinite(value.radius) && std::isfinite(value.height) &&
               std::isfinite(value.maximum_climb) &&
               std::isfinite(value.maximum_slope_degrees) &&
               value.radius > 0.0f && value.height > 0.0f &&
               value.maximum_climb >= 0.0f &&
               value.maximum_slope_degrees >= 0.0f &&
               value.maximum_slope_degrees < 90.0f;
    }

    [[nodiscard]] inline bool valid(const NavigationPathRequest& value) noexcept
    {
        return lux::spatial::isFinite(value.start) &&
               lux::spatial::isFinite(value.destination) &&
               valid(value.agent) &&
               std::isfinite(value.nearest_horizontal_extent) &&
               std::isfinite(value.nearest_vertical_extent) &&
               value.nearest_horizontal_extent > 0.0f &&
               value.nearest_vertical_extent > 0.0f &&
               value.maximum_path_nodes > 0u &&
               value.maximum_path_points > 0u &&
               (!value.start_region || value.start_region->valid()) &&
               (!value.destination_region || value.destination_region->valid());
    }
} // namespace lux::navigation
