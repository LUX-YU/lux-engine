#include <lux/engine/ecs/HierarchySchema.hpp>

#include <lux/engine/ecs/Parent.hpp>
#include <lux/engine/ecs/Parent.ecs_schema.hpp>
#include <lux/engine/ecs/Parent.ecs_persistence.hpp>

namespace lux::ecs
{
    std::span<const ComponentSchema> hierarchyComponentSchemas()
    {
        return generated::hierarchyComponentSchemas();
    }

    ComponentPersistenceContribution hierarchyPersistenceContribution() noexcept
    {
        return generated::hierarchyPersistenceContribution();
    }
} // namespace lux::ecs
