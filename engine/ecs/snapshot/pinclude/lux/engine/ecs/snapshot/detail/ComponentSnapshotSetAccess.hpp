#pragma once

#include <lux/engine/ecs/ComponentSnapshotSet.hpp>

namespace lux::ecs::detail
{
    struct ComponentSnapshotSetAccess final
    {
        [[nodiscard]] static const ComponentSchemaSet& schemas(
            const ComponentSnapshotSet& set
        ) noexcept;

        [[nodiscard]] static const ComponentSnapshotBinding* findStorage(
            const ComponentSnapshotSet& set,
            std::uint64_t storage
        ) noexcept;

        static void clone(
            const ComponentSnapshotBinding& binding,
            const World& source,
            WorldMutation& target
        )
        {
            binding.clone_(source, target);
        }
    };
} // namespace lux::ecs::detail
