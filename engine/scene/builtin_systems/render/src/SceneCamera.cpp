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

    RenderResult<CameraInput> sceneParameters(
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
        CameraInput result;
        Eigen::Map<Eigen::Matrix4f>{result.view.data()} = prepared.view.cast<float>();
        Eigen::Map<Eigen::Matrix4f>{result.projection.data()} = projection->cast<float>();
        Eigen::Map<Eigen::Matrix4f>{result.inverse_view_projection.data()} =
            (*projection * prepared.view).inverse().cast<float>();
        const auto& position = prepared.position;
        const auto& forward = prepared.direction;
        const float near_distance = prepared.near_distance, far_distance = prepared.far_distance;
        result.position = {float(position.x()), float(position.y()), float(position.z())};
        result.background = input.background;
        result.viewport = {input.viewport.x, input.viewport.y, input.viewport.width, input.viewport.height};
        result.exposure = input.exposure;
        result.direction = {float(forward.x()), float(forward.y()), float(forward.z())};
        result.near_distance = near_distance;
        result.far_distance = far_distance;
        return result;
    }

    bool sameParameters(const CameraInput& left, const CameraInput& right) noexcept
    {
        return left == right;
    }
} // namespace lux::scene::detail
