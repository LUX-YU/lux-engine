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
        return true;
    }
} // namespace lux::system
