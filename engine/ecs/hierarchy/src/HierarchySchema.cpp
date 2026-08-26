#include <lux/engine/ecs/HierarchySchema.hpp>

#include <lux/engine/ecs/Parent.hpp>
#include <lux/engine/ecs/Parent.ecs_schema.hpp>
#include <lux/engine/ecs/Parent.ecs_snapshot.hpp>

namespace lux::ecs
{
    std::span<const ComponentSchema> hierarchyComponentSchemas()
    {
        return generated::hierarchyComponentSchemas();
    }

    ComponentSnapshotContribution
    hierarchyComponentSnapshotContribution() noexcept
    {
        return generated::hierarchyComponentSnapshotContribution();
    }
} // namespace lux::ecs
