#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/ComponentAnnotations.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <Eigen/Geometry>

#include <array>
#include <numbers>
#include <variant>

namespace lux::scene
{
    struct LUX_TYPE_INFO(static) PerspectiveProjection
    {
        double vertical_fov{std::numbers::pi / 3.0};
        double near_plane{0.05};
        double far_plane{100000.0};
    };

    struct LUX_TYPE_INFO(static) OrthographicProjection
    {
        double vertical_extent{10.0};
        double near_plane{0.05};
        double far_plane{100000.0};
    };

    using VProjection = std::variant<PerspectiveProjection, OrthographicProjection>;

    struct LUX_COMPONENT(
        schema = "lux.scene.ViewComponent",
        version = 1,
        snapshot = COPY, persistent = true,
        semantic = DOMAIN_CONTRACT,
        create = DEFAULT
    ) ViewComponent
    {
        VProjection projection{PerspectiveProjection{}};
        float exposure{};
        std::array<float, 4> background{0, 0, 0, 1};
    };

    enum class EViewError
    {
        INVALID_PROJECTION,
        INVALID_TRANSFORM
    };

    // Right-handed camera: local -Z is forward, +Y is up. Projection depth is
    // [0, 1], and NDC Y follows image coordinates (top to bottom).
    [[nodiscard]] lux::cxx::expected<Eigen::Matrix4d, EViewError>
    viewProjection(const ViewComponent& view, double aspect_ratio) noexcept;

    // Subtract the origin before narrowing for rendering. Picking keeps the
    // returned double matrix and adds the same origin to its resulting ray.
    [[nodiscard]] lux::cxx::expected<Eigen::Matrix4d, EViewError>
    viewMatrix(const Eigen::Affine3d& world, const Eigen::Vector3d& origin) noexcept;
} // namespace lux::scene

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/scene/ViewComponent.type_static_info.hpp>
#endif
