#pragma once

#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/transform/visibility.h>
#include <lux/cxx/compile_time/expected.hpp>

namespace lux::simulation::ecs
{
    enum class ETransformEvaluationError : std::uint8_t
    {
        INVALID_ENTITY,
        MISSING_TRANSFORM,
        INVALID_PARENT,
        HIERARCHY_CYCLE,
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_TRANSFORM_PUBLIC Eigen::Affine2d localTransformMatrix(
        const Transform2D& value
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_TRANSFORM_PUBLIC Eigen::Affine3d localTransformMatrix(
        const Transform3D& value
    ) noexcept;

    using Transform2DResult = lux::cxx::expected<Eigen::Affine2d, ETransformEvaluationError>;
    using Transform3DResult = lux::cxx::expected<Eigen::Affine3d, ETransformEvaluationError>;

    // Reads current local data, never the WorldTransform cache. Callers synchronize ancestor reads with writers.
    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_TRANSFORM_PUBLIC Transform2DResult
    computeWorldTransform2D(const Registry& registry, Entity entity) noexcept;
    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_TRANSFORM_PUBLIC Transform3DResult
    computeWorldTransform3D(const Registry& registry, Entity entity) noexcept;
}
