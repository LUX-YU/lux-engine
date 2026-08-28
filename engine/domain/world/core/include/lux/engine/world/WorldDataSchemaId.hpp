#pragma once

#include <lux/engine/world/visibility.h>

#include <lux/cxx/core/StableNameId.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lux::world
{
    struct WorldDataSchemaId final
    {
        std::uint64_t hash{};
        std::string name;

        [[nodiscard]] bool valid() const noexcept
        {
            return !name.empty() && hash == lux::cxx::Fnv1a64::hash(name);
        }

        friend bool operator==(const WorldDataSchemaId&, const WorldDataSchemaId&) noexcept = default;
    };

    struct WorldDataSchemaIdLess final
    {
        [[nodiscard]] bool operator()(const WorldDataSchemaId& left, const WorldDataSchemaId& right) const noexcept
        {
            return left.hash < right.hash || (left.hash == right.hash && left.name < right.name);
        }
    };

    struct WorldDataSchemaIdHash final
    {
        [[nodiscard]] std::size_t operator()(const WorldDataSchemaId& value) const noexcept
        {
            std::size_t result = static_cast<std::size_t>(value.hash);
            if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t))
                result ^= static_cast<std::size_t>(value.hash >> 32U);
            return result;
        }
    };

    [[nodiscard]] LUX_ENGINE_WORLD_PUBLIC WorldDataSchemaId worldDataSchemaId(std::string_view name);
}
