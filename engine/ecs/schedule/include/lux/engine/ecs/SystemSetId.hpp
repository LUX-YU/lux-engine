#pragma once

#include <lux/cxx/core/StableNameId.hpp>

#include <string_view>

namespace lux::ecs
{
    struct SystemSetTag;

    struct SystemSetId final
    {
        lux::cxx::StableNameIdView<SystemSetTag> id;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return id.isValid();
        }

        [[nodiscard]] constexpr bool operator==(
            const SystemSetId& other
        ) const noexcept = default;
    };

    [[nodiscard]] constexpr SystemSetId systemSetId(
        std::string_view name
    ) noexcept
    {
        return SystemSetId{
            lux::cxx::StableNameIdView<SystemSetTag>{name}
        };
    }
} // namespace lux::ecs
