#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstdint>

namespace lux::scene
{
    struct LUX_TYPE_INFO(both) TransformSystemConfiguration final
    {
        std::uint64_t entity_capacity{};
        std::uint64_t max_commands{};
        std::uint64_t max_payload_bytes{};
    };
} // namespace lux::scene
