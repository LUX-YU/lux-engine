#pragma once
/**
 * @file ComponentSchemaId.hpp
 * @brief ECS-owned stable wire identity for one component schema.
 */

#include <lux/cxx/algorithm/hash.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace lux::ecs
{
    struct ComponentSchemaId final
    {
        std::uint64_t hash{};
        std::string name;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return hash != 0u && !name.empty();
        }

        friend bool operator==(
            const ComponentSchemaId&,
            const ComponentSchemaId&) = default;
    };

    [[nodiscard]] inline ComponentSchemaId componentSchemaId(
        std::string_view name)
    {
        return ComponentSchemaId{
            lux::cxx::algorithm::fnv1a(name),
            std::string{name}};
    }

    [[nodiscard]] inline bool isCanonicalComponentSchemaName(
        std::string_view name) noexcept
    {
        if (name.empty() || name.front() == '.' || name.back() == '.')
            return false;

        bool has_dot = false;
        bool previous_dot = false;
        for (const char value : name)
        {
            const bool dot = value == '.';
            if (dot)
            {
                if (previous_dot)
                    return false;
                has_dot = true;
            }
            else if (!((value >= 'a' && value <= 'z') ||
                       (value >= '0' && value <= '9') ||
                       value == '_' || value == '-'))
            {
                return false;
            }
            previous_dot = dot;
        }
        return has_dot;
    }

    [[nodiscard]] inline bool isValidComponentSchemaId(
        const ComponentSchemaId& id) noexcept
    {
        return static_cast<bool>(id) &&
            isCanonicalComponentSchemaName(id.name) &&
            id.hash == lux::cxx::algorithm::fnv1a(id.name);
    }

    [[nodiscard]] inline bool componentSchemaIdCollision(
        const ComponentSchemaId& lhs,
        const ComponentSchemaId& rhs) noexcept
    {
        return lhs.hash == rhs.hash && lhs.name != rhs.name;
    }
} // namespace lux::ecs
