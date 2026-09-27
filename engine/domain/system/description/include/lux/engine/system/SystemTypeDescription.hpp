#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace lux::system
{
    enum class ESystemMultiplicity : std::uint8_t
    {
        MULTIPLE = 0,
        SINGLE_PER_OWNER = 1,
    };

    struct SystemTypeDescription final
    {
        std::string_view canonical_name;
        std::uint32_t version{};
        std::string_view configuration_schema_name;
        std::uint32_t configuration_schema_version{};
        std::span<const std::string_view> capabilities;
        ESystemMultiplicity multiplicity{ESystemMultiplicity::MULTIPLE};
        // Exact World partitioner names, or the single value "*" for partition-independent systems.
        std::span<const std::string_view> supported_world_types;
    };

    [[nodiscard]] constexpr bool validSystemTypeDescription(const SystemTypeDescription& value) noexcept
    {
        if (value.canonical_name.empty() || value.version == 0U)
        {
            return false;
        }
        const bool has_schema_name = !value.configuration_schema_name.empty();
        const bool has_schema_version = value.configuration_schema_version != 0U;
        if (has_schema_name != has_schema_version)
        {
            return false;
        }
        if (value.multiplicity != ESystemMultiplicity::MULTIPLE &&
            value.multiplicity != ESystemMultiplicity::SINGLE_PER_OWNER)
        {
            return false;
        }
        for (std::size_t index{}; index < value.capabilities.size(); ++index)
        {
            if (value.capabilities[index].empty())
            {
                return false;
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (value.capabilities[index] == value.capabilities[previous])
                {
                    return false;
                }
            }
        }
        if (value.supported_world_types.empty())
            return false;
        for (std::size_t index{}; index < value.supported_world_types.size(); ++index)
        {
            const auto name = value.supported_world_types[index];
            const bool invalid_name = name.empty() || (name == "*" && value.supported_world_types.size() != 1);
            if (invalid_name)
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
                if (name == value.supported_world_types[previous])
                    return false;
        }
        return true;
    }

    [[nodiscard]] constexpr bool supportsWorldType(const SystemTypeDescription& value, std::string_view name) noexcept
    {
        for (const auto supported : value.supported_world_types)
            if (supported == "*" || supported == name)
                return true;
        return false;
    }
} // namespace lux::system
