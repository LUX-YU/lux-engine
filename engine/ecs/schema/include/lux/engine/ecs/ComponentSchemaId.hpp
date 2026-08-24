#pragma once

#include <lux/cxx/core/StableNameId.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace lux::ecs
{
    struct ComponentSchemaId final
    {
        std::uint64_t hash{};
        std::string name;

        [[nodiscard]] bool valid() const noexcept
        {
            return !name.empty() &&
                hash == lux::cxx::Fnv1a64::hash(name);
        }

        [[nodiscard]] bool operator==(
            const ComponentSchemaId& other
        ) const noexcept = default;
    };

    [[nodiscard]] inline ComponentSchemaId componentSchemaId(
        std::string_view name
    )
    {
        return ComponentSchemaId{
            lux::cxx::Fnv1a64::hash(name),
            std::string(name)};
    }
} // namespace lux::ecs
