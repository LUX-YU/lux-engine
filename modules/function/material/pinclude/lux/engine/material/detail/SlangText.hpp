#pragma once
#include <charconv>
#include <lux/engine/material/graph/Types.hpp>
#include <string>

namespace lux::material::detail
{
    inline std::string slangType(EValueType type)
    {
        return type == EValueType::FLOAT ? "float" : "float" + std::to_string(static_cast<unsigned>(type) + 1);
    }

    inline std::string slangNumber(float value)
    {
        char buffer[64];
        const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
        std::string result(buffer, converted.ptr);
        if (result.find_first_of(".eE") == std::string::npos)
        {
            result += ".0";
        }
        return result;
    }

    inline std::string slangLiteral(EValueType type, const float* values)
    {
        std::string result = slangType(type) + "(";
        for (unsigned index = 0; index <= static_cast<unsigned>(type); ++index)
        {
            if (index)
            {
                result += ",";
            }
            result += slangNumber(values[index]);
        }
        return result + ")";
    }

    // Existing mesh interpolant ABI; vertex color has no supported producer in this path.
    inline constexpr std::int32_t kMaterialInputLocations[]{3, 0, 1, 5, -1};
} // namespace lux::material::detail
