#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <toml++/toml.hpp>

namespace lux::material::detail
{
    inline toml::array floats(std::span<const float> values)
    {
        toml::array result;
        for (const auto value : values)
        {
            result.push_back(static_cast<double>(value));
        }
        return result;
    }

    inline bool readFloats(const toml::node_view<const toml::node>& value, std::span<float> destination) noexcept
    {
        const auto* array = value.as_array();
        if (!array || array->size() != destination.size())
        {
            return false;
        }
        for (std::size_t index{}; index < destination.size(); ++index)
        {
            const auto number = (*array)[index].value<double>();
            if (!number)
            {
                return false;
            }
            destination[index] = static_cast<float>(*number);
            if (!std::isfinite(destination[index]))
            {
                return false;
            }
        }
        return true;
    }

    inline bool fields(const toml::table& table, std::initializer_list<std::string_view> allowed) noexcept
    {
        return std::all_of(
            table.begin(),
            table.end(),
            [&](const auto& item)
            { return std::find(allowed.begin(), allowed.end(), item.first.str()) != allowed.end(); }
        );
    }

    inline std::optional<std::uint32_t> integer(
        const toml::node_view<const toml::node>& value,
        std::uint32_t maximum
    ) noexcept
    {
        const auto number = value.value<std::int64_t>();
        if (!number || *number < 0 || static_cast<std::uint64_t>(*number) > maximum)
        {
            return {};
        }
        return static_cast<std::uint32_t>(*number);
    }

} // namespace lux::material::detail
