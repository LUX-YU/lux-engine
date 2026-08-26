#pragma once

#include <lux/engine/simulation/SystemEventDescription.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace lux::simulation
{
    struct SystemDescription final
    {
        std::string_view canonical_name;
        std::uint32_t version{};
        std::string_view configuration_schema_name;
        std::uint32_t configuration_schema_version{};
        std::span<const std::string_view> capabilities;
        std::span<const SystemExecutionPoint> execution_points;
        std::span<const SystemEventDescription> events;
    };

    [[nodiscard]] constexpr bool validSystemDescription(
        const SystemDescription& description
    ) noexcept
    {
        if (description.canonical_name.empty() || description.version == 0U)
            return false;
        if (description.configuration_schema_name.empty() !=
            (description.configuration_schema_version == 0U))
        {
            return false;
        }
        for (std::size_t index{}; index < description.capabilities.size(); ++index)
        {
            if (description.capabilities[index].empty())
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (description.capabilities[index] ==
                    description.capabilities[previous])
                {
                    return false;
                }
            }
        }
        for (std::size_t index{}; index < description.execution_points.size(); ++index)
        {
            if (description.execution_points[index].name.empty())
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (description.execution_points[index].name ==
                    description.execution_points[previous].name)
                {
                    return false;
                }
            }
        }
        for (std::size_t index{}; index < description.events.size(); ++index)
        {
            const auto& event = description.events[index];
            if (event.name.empty() || event.dispatch_point.empty() ||
                !event.payload_cpp_type.isValid())
            {
                return false;
            }
            if (event.payload_schema_name.empty() !=
                (event.payload_schema_version == 0U))
            {
                return false;
            }
            bool point_found{};
            for (const auto& point : description.execution_points)
                point_found = point_found || point.name == event.dispatch_point;
            if (!point_found)
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (event.name == description.events[previous].name)
                    return false;
            }
        }
        return true;
    }
}
