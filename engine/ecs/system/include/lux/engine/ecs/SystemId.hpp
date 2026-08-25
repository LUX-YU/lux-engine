#pragma once

#include <lux/cxx/container/SlotMap.hpp>

#include <cstdint>

namespace lux::ecs
{
    struct SystemTag;
    using SystemId = lux::cxx::SlotKey<
        SystemTag,
        std::uint32_t,
        std::uint32_t
    >;
}
