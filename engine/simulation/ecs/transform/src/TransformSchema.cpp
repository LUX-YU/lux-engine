#include <lux/engine/simulation/ecs/TransformSchema.hpp>

#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>
#include <lux/engine/simulation/ecs/Transform.ecs_schema.hpp>
#include <lux/engine/simulation/ecs/Transform.ecs_snapshot.hpp>

namespace lux::simulation::ecs
{
    std::span<const ComponentSchema> transformComponentSchemas() noexcept
    {
        return generated::transformComponentSchemas();
    }

    ComponentSnapshotContribution
    transformComponentSnapshotContribution() noexcept
    {
        return generated::transformComponentSnapshotContribution();
    }
} // namespace lux::simulation::ecs
