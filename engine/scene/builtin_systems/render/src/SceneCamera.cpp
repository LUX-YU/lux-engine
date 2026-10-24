#include <lux/engine/scene/SceneCamera.hpp>

namespace lux::scene::detail
{
    using namespace render;

    CameraRecipeKey cameraRecipeKey(const Camera& camera, const CameraTarget& output, bool shadows) noexcept
    {
        CameraRecipeKey key{
            output.target,
            output.description.format,
            output.description.kind,
            output.description.samples,
            output.description.sampled
        };
        if (shadows)
        {
            std::visit(
                [&](const auto& projection) noexcept
                {
                    key.shadow_near = static_cast<float>(projection.near_plane);
                    key.shadow_far = static_cast<float>(projection.far_plane);
                },
                camera.projection
            );
        }
        return key;
    }

    RenderResult<SceneCamera> prepareSceneCamera(const Camera& camera, const Eigen::Affine3d& transform) noexcept
    {
        auto view = cameraView(transform, Eigen::Vector3d::Zero());
        if (!view)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto [near_distance, far_distance] = std::visit(
            [](const auto& value) noexcept { return std::pair{float(value.near_plane), float(value.far_plane)}; },
            camera.projection
        );
        return SceneCamera{
            *view,
            transform.translation(),
            -view->row(2).head<3>().transpose(),
            near_distance,
            far_distance
        };
    }

    RenderResult<SceneParameters> sceneParameters(
        const Camera& camera,
        const SceneCamera& prepared,
        const CameraTarget& input
    ) noexcept
    {
        const bool invalid_target =
            input.target.isNull() || input.description.width == 0 || input.description.height == 0;
        if (invalid_target || !input.viewport.validate())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto aspect = double(input.description.width) * input.viewport.width /
                            (double(input.description.height) * input.viewport.height);
        const auto projection = cameraProjection(camera, aspect);
        if (!projection)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        SceneParameters result;
        Eigen::Map<Eigen::Matrix4f>{result.mesh.view.data()} = prepared.view.cast<float>();
        Eigen::Map<Eigen::Matrix4f>{result.mesh.projection.data()} = projection->cast<float>();
        Eigen::Map<Eigen::Matrix4f>{result.lighting.inverse_view_projection.data()} =
            (*projection * prepared.view).inverse().cast<float>();
        const auto& position = prepared.position;
        const auto& forward = prepared.direction;
        const float near_distance = prepared.near_distance, far_distance = prepared.far_distance;
        result.lighting.camera_position = {float(position.x()), float(position.y()), float(position.z()), 1};
        result.lighting.background = input.background;
        result.lighting.viewport = {input.viewport.x, input.viewport.y, input.viewport.width, input.viewport.height};
        result.tone.exposure = input.exposure;
        result.shadow.inverse_view_projection = result.lighting.inverse_view_projection;
        result.shadow.position_near = {float(position.x()), float(position.y()), float(position.z()), near_distance};
        result.shadow.direction_far = {float(forward.x()), float(forward.y()), float(forward.z()), far_distance};
        result.clusters.inverse_view_projection = result.shadow.inverse_view_projection;
        result.clusters.position_near = result.shadow.position_near;
        result.clusters.direction_far = result.shadow.direction_far;
        return result;
    }

    bool sameParameters(const SceneParameters& left, const SceneParameters& right) noexcept
    {
        return left.mesh.view == right.mesh.view && left.mesh.projection == right.mesh.projection &&
               left.lighting.inverse_view_projection == right.lighting.inverse_view_projection &&
               left.lighting.camera_position == right.lighting.camera_position &&
               left.lighting.viewport == right.lighting.viewport &&
               left.lighting.background == right.lighting.background &&
               left.lighting.environment == right.lighting.environment && left.tone.exposure == right.tone.exposure &&
               left.shadow == right.shadow;
    }
} // namespace lux::scene::detail
