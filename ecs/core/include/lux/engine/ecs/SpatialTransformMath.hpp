#pragma once

#include <lux/engine/math/Position.hpp>
#include <lux/engine/math/RelativePosition.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cmath>
#include <optional>

namespace lux::ecs
{
    inline constexpr float kDefaultRelativeSpatialExtent = 16'777'216.0f;

    struct PhysicsRelativePose3D final
    {
        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
    };

    [[nodiscard]] inline std::optional<Eigen::Vector2f> relativePosition(
        const lux::math::Position2d& position,
        const lux::math::Position2d& origin,
        float maximum_extent) noexcept
    {
        const auto relative = lux::math::relativeFloat(
            position,
            origin,
            maximum_extent
        );
        if (!relative)
            return std::nullopt;
        return Eigen::Vector2f{(*relative)[0u], (*relative)[1u]};
    }

    [[nodiscard]] inline std::optional<Eigen::Vector3f> relativePosition(
        const lux::math::Position3d& position,
        const lux::math::Position3d& origin,
        float maximum_extent) noexcept
    {
        const auto relative = lux::math::relativeFloat(
            position,
            origin,
            maximum_extent
        );
        if (!relative)
            return std::nullopt;
        return Eigen::Vector3f{
            (*relative)[0u],
            (*relative)[1u],
            (*relative)[2u]};
    }

    [[nodiscard]] inline bool offsetByScaledPosition(
        lux::math::Position2d& position,
        const lux::math::Position2d& delta,
        const Eigen::Vector2f& factor) noexcept
    {
        if (!lux::math::isFinite(position) ||
            !lux::math::isFinite(delta) || !factor.allFinite())
        {
            return false;
        }
        const auto next = lux::math::Position2d{
            position.x + delta.x * static_cast<double>(factor.x()),
            position.y + delta.y * static_cast<double>(factor.y())};
        if (!lux::math::isFinite(next))
            return false;
        position = next;
        return true;
    }

    [[nodiscard]] inline std::optional<Eigen::Matrix4f> relativeTransform(
        const lux::math::Position3d& position,
        const Eigen::Matrix3f& linear,
        const lux::math::Position3d& origin,
        float maximum_extent) noexcept
    {
        const auto relative = relativePosition(
            position,
            origin,
            maximum_extent
        );
        if (!relative || !linear.allFinite())
            return std::nullopt;
        Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
        result.block<3, 3>(0, 0) = linear;
        result.block<3, 1>(0, 3) = *relative;
        return result;
    }

    [[nodiscard]] inline std::optional<Eigen::Matrix4f> relativeTransform(
        const lux::math::Position2d& position,
        const Eigen::Matrix2f& linear,
        const lux::math::Position2d& origin,
        float maximum_extent) noexcept
    {
        const auto relative = relativePosition(
            position,
            origin,
            maximum_extent
        );
        if (!relative || !linear.allFinite())
            return std::nullopt;
        Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
        result.block<2, 2>(0, 0) = linear;
        result.block<2, 1>(0, 3) = *relative;
        return result;
    }

    [[nodiscard]] inline std::optional<PhysicsRelativePose3D>
    makePhysicsRelativePose(
        const lux::math::Position3d& position,
        const Eigen::Quaternionf& rotation,
        const lux::math::Position3d& origin,
        float maximum_extent) noexcept
    {
        const auto relative = relativePosition(
            position,
            origin,
            maximum_extent
        );
        if (!relative || !rotation.coeffs().allFinite() ||
            rotation.squaredNorm() <= 0.0f)
        {
            return std::nullopt;
        }
        return PhysicsRelativePose3D{*relative, rotation.normalized()};
    }
} // namespace lux::ecs
