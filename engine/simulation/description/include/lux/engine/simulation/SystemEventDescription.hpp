#pragma once

#include <lux/engine/simulation/SystemExecutionPoint.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lux::simulation
{
    struct SystemEventDescription final
    {
        std::string_view name;
        std::string_view dispatch_point;
        std::string_view payload_schema_name;
        std::uint32_t payload_schema_version{};
        lux::cxx::TypeToken payload_cpp_type;
    };

    template <class Payload>
    [[nodiscard]] constexpr SystemEventDescription makeSystemEvent(
        std::string_view name,
        SystemExecutionPoint dispatch_point,
        std::string_view payload_schema_name,
        std::uint32_t payload_schema_version
    ) noexcept
    {
        if constexpr (std::is_void_v<Payload>)
        {
            return SystemEventDescription{
                name,
                dispatch_point.name,
                {},
                0U,
                lux::cxx::typeToken<void>()};
        }
        else
        {
            return SystemEventDescription{
                name,
                dispatch_point.name,
                payload_schema_name,
                payload_schema_version,
                lux::cxx::typeToken<Payload>()};
        }
    }
}
