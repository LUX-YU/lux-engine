#include <lux/engine/simulation/ecs/HierarchySchema.hpp>

#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Parent.ecs_schema.hpp>
#include <lux/engine/simulation/ecs/Parent.ecs_snapshot.hpp>

namespace lux::simulation::ecs
{
    std::span<const ComponentSchema> hierarchyComponentSchemas()
    {
        return generated::hierarchyComponentSchemas();
    }

    ComponentSnapshotContribution hierarchyComponentSnapshotContribution() noexcept
    {
        return generated::hierarchyComponentSnapshotContribution();
    }
} // namespace lux::simulation::ecs
