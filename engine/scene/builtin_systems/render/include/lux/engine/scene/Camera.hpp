#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <Eigen/Geometry>

#include <numbers>
#include <variant>

namespace lux::scene
{
struct LUX_TYPE_INFO(static) PerspectiveProjection
{
    double LUX_MEMBER(display_name = VerticalFov) vertical_fov{std::numbers::pi / 3.0};
    double LUX_MEMBER(display_name = Near) near_plane{0.05};
    double LUX_MEMBER(display_name = Far) far_plane{100000.0};
};

struct LUX_TYPE_INFO(static) OrthographicProjection
{
    double LUX_MEMBER(display_name = VerticalExtent) vertical_extent{10.0};
    double LUX_MEMBER(display_name = Near) near_plane{0.05};
    double LUX_MEMBER(display_name = Far) far_plane{100000.0};
};

struct LUX_COMPONENT(schema = "lux.scene.Camera", version = 1, snapshot = COPY, semantic = DOMAIN_CONTRACT,
                     editor = true) Camera
{
    std::variant<PerspectiveProjection, OrthographicProjection> LUX_MEMBER(display_name = Projection) projection{
        PerspectiveProjection{}};
    bool LUX_MEMBER(display_name = Primary) primary{};
};

enum class ECameraError
{
    INVALID_PROJECTION,
    INVALID_TRANSFORM,
    NO_PRIMARY_CAMERA,
    MULTIPLE_PRIMARY_CAMERAS
};

// Right-handed camera: local -Z is forward, +Y is up. Vulkan depth is
// [0, 1], and NDC Y follows image coordinates (top to bottom).
[[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC lux::cxx::expected<Eigen::Matrix4d, ECameraError> cameraProjection(
    const Camera &camera, double aspect_ratio) noexcept;

// Subtract the origin before narrowing for rendering. Picking keeps the
// returned double matrix and adds the same origin to its resulting ray.
[[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC lux::cxx::expected<Eigen::Matrix4d, ECameraError> cameraView(
    const Eigen::Affine3d &world, const Eigen::Vector3d &origin) noexcept;
} // namespace lux::scene

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/scene/Camera.type_static_info.hpp>
#endif
