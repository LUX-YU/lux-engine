#include <lux/engine/ecs/TransformSchema.hpp>

#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>
#include <lux/engine/ecs/Transform.ecs_schema.hpp>
#include <lux/engine/ecs/Transform.ecs_persistence.hpp>

namespace lux::ecs
{
    std::span<const ComponentSchema> transformComponentSchemas() noexcept
    {
        return generated::transformComponentSchemas();
    }

    ComponentPersistenceContribution transformPersistenceContribution() noexcept
    {
        return generated::transformPersistenceContribution();
    }
} // namespace lux::ecs
