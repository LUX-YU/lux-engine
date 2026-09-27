#include <lux/engine/simulation/ecs/TransformEvaluation.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>

namespace lux::simulation::ecs
{
    Eigen::Affine2d localTransformMatrix(const Transform2D& value) noexcept
    {
        Eigen::Affine2d result = Eigen::Affine2d::Identity();
        result.translate(value.translation);
        result.rotate(value.rotation);
        result.scale(value.scale);
        return result;
    }

    Eigen::Affine3d localTransformMatrix(const Transform3D& value) noexcept
    {
        Eigen::Affine3d result = Eigen::Affine3d::Identity();
        result.translate(value.translation);
        result.rotate(value.rotation);
        result.scale(value.scale);
        return result;
    }

    namespace
    {
        using ParentResult = lux::cxx::expected<Entity, ETransformEvaluationError>;

        [[nodiscard]] ParentResult parentOf(const Registry& registry, Entity entity) noexcept
        {
            if (entity == NullEntity)
            {
                return NullEntity;
            }
            const auto* parent = registry.try_get<Parent>(entity);
            if (parent == nullptr)
            {
                return NullEntity;
            }
            if (!registry.valid(parent->entity))
            {
                return lux::cxx::unexpected(ETransformEvaluationError::INVALID_PARENT);
            }
            return parent->entity;
        }

        [[nodiscard]] lux::cxx::expected<void, ETransformEvaluationError> validateChain(
            const Registry& registry,
            Entity entity
        ) noexcept
        {
            Entity slow = entity;
            Entity fast = entity;
            do
            {
                const auto next = parentOf(registry, slow);
                if (!next)
                {
                    return lux::cxx::unexpected(next.error());
                }
                slow = *next;
                for (unsigned step{}; step < 2U; ++step)
                {
                    const auto ahead = parentOf(registry, fast);
                    if (!ahead)
                    {
                        return lux::cxx::unexpected(ahead.error());
                    }
                    fast = *ahead;
                }
                if (slow != NullEntity && slow == fast)
                {
                    return lux::cxx::unexpected(ETransformEvaluationError::HIERARCHY_CYCLE);
                }
            } while (fast != NullEntity);
            return {};
        }

        template <class Local, class Matrix>
        [[nodiscard]] lux::cxx::expected<Matrix, ETransformEvaluationError> compute(
            const Registry& registry,
            Entity entity
        ) noexcept
        {
            if (!registry.valid(entity))
            {
                return lux::cxx::unexpected(ETransformEvaluationError::INVALID_ENTITY);
            }
            const auto* local = registry.try_get<Local>(entity);
            if (local == nullptr)
            {
                return lux::cxx::unexpected(ETransformEvaluationError::MISSING_TRANSFORM);
            }
            if (const auto valid = validateChain(registry, entity); !valid)
            {
                return lux::cxx::unexpected(valid.error());
            }
            Matrix result = localTransformMatrix(*local);
            while (const auto* parent = registry.try_get<Parent>(entity))
            {
                entity = parent->entity;
                local = registry.try_get<Local>(entity);
                if (local == nullptr)
                {
                    break; // A missing same-dimensional local transform breaks inheritance.
                }
                result = localTransformMatrix(*local) * result;
            }
            return result;
        }
    }

    Transform2DResult computeWorldTransform2D(const Registry& registry, Entity entity) noexcept
    {
        return compute<Transform2D, Eigen::Affine2d>(registry, entity);
    }

    Transform3DResult computeWorldTransform3D(const Registry& registry, Entity entity) noexcept
    {
        return compute<Transform3D, Eigen::Affine3d>(registry, entity);
    }
}
