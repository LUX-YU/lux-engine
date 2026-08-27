#pragma once

#include <lux/engine/simulation/SystemHookPoint.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lux::simulation
{
    enum class ESystemEventTarget : std::uint8_t
    {
        GLOBAL,
        ENTITY_TARGETED,
    };

    struct SystemEventDescription final
    {
        std::string_view name;
        std::string_view dispatch_hook;
        ESystemEventTarget target{ESystemEventTarget::GLOBAL};
        std::string_view payload_schema_name;
        std::uint32_t payload_schema_version{};
        lux::cxx::TypeToken payload_cpp_type;
    };

    template <class Payload>
    [[nodiscard]] constexpr SystemEventDescription makeSystemEvent(
        std::string_view name,
        SystemHookPoint dispatch_hook,
        ESystemEventTarget target,
        std::string_view payload_schema_name,
        std::uint32_t payload_schema_version
    ) noexcept
    {
        if constexpr (std::is_void_v<Payload>)
        {
            return SystemEventDescription{
                name,
                dispatch_hook.name,
                target,
                {},
                0U,
                lux::cxx::typeToken<void>()};
        }
        else
        {
            return SystemEventDescription{
                name,
                dispatch_hook.name,
                target,
                payload_schema_name,
                payload_schema_version,
                lux::cxx::typeToken<Payload>()};
        }
    }
}
