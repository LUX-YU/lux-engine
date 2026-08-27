#pragma once

#include <lux/engine/simulation/ecs/ComponentSnapshotSet.hpp>

namespace lux::simulation::ecs::detail
{
    struct ComponentSnapshotSetAccess final
    {
        [[nodiscard]] static const ComponentSchemaSet& schemas(const ComponentSnapshotSet& set) noexcept;

        [[nodiscard]] static const ComponentSnapshotBinding*
        findStorage(const ComponentSnapshotSet& set, std::uint64_t storage) noexcept;

        static void clone(const ComponentSnapshotBinding& binding, const Registry& source, Registry& target)
        {
            binding.clone_(source, target);
        }
    };
} // namespace lux::simulation::ecs::detail
