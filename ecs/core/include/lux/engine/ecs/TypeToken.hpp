#pragma once
/**
 * @file TypeToken.hpp
 * @brief Small RTTI-free type identity shared by ECS assembly metadata.
 *
 * The hash is the machine-friendly identity. The compile-time type name is
 * retained as a collision guard and for diagnostics; it is never parsed and
 * never used as the primary key.
 */

#include <lux/cxx/compile_time/type_info.hpp>

#include <cstdint>
#include <string_view>

namespace lux::ecs
{
    struct TypeToken
    {
        std::uint64_t    hash{};
        std::string_view name{};

        [[nodiscard]] constexpr explicit operator bool() const noexcept
        {
            return hash != 0 && !name.empty();
        }
    };

    template <class T>
    [[nodiscard]] constexpr TypeToken typeToken() noexcept
    {
        return TypeToken{lux::cxx::type_hash<T>(), lux::cxx::type_name<T>()};
    }

    [[nodiscard]] constexpr bool sameTypeToken(
        TypeToken lhs, TypeToken rhs) noexcept
    {
        return lhs.hash == rhs.hash && lhs.name == rhs.name;
    }

} // namespace lux::ecs
