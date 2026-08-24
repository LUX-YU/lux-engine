#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <cstdint>
#include <span>

namespace lux::ecs
{
    enum class EAccessMode : std::uint8_t
    {
        READ,
        WRITE,
    };

    struct ComponentAccess final
    {
        lux::cxx::TypeToken type;
        EAccessMode mode{EAccessMode::READ};
    };

    struct ExternalAccess final
    {
        lux::cxx::TypeToken type;
        EAccessMode mode{EAccessMode::READ};
    };

    struct SystemAccess final
    {
        std::span<const ComponentAccess> components;
        std::span<const ExternalAccess> external;
        bool structural{};
        bool complete{};
    };
} // namespace lux::ecs
