#pragma once

#include <lux/engine/ecs/SystemRelations.hpp>

#include <span>

namespace lux::ecs::detail
{
    struct SystemRelationEdge final
    {
        SystemId before{};
        SystemId after{};

        [[nodiscard]] bool operator==(
            const SystemRelationEdge&
        ) const noexcept = default;
    };

    struct SystemRelationsAccess final
    {
        [[nodiscard]] static SystemRelationsId scope(
            const SystemRelations& relations
        ) noexcept;

        [[nodiscard]] static std::span<const SystemRelationEdge> edges(
            const SystemRelations& relations
        ) noexcept;
    };
}
