#include <lux/engine/systems/render/view/ViewCollector.hpp>
#include <lux/engine/systems/render/view/RenderViewComponent.hpp>
#include <cmath>

namespace lux::engine::render
{
    lux::render::RenderResult<lux::render::CameraInput>
    makeViewParameters(
        const scene::ViewComponent& view, const ecs::Transform3D& transform, const lux::render::TargetDesc& target
    ) noexcept
    {
        if (!target.width || !target.height)
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
        }
        const Eigen::Affine3d world = Eigen::Translation3d(transform.translation) * transform.rotation *
            Eigen::Scaling(transform.scale);
        const auto projection = scene::viewProjection(view, double(target.width) / target.height);
        const auto matrix = scene::viewMatrix(world, Eigen::Vector3d::Zero());
        if (!projection || !matrix)
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
        }
        const Eigen::Matrix4f projected = projection->cast<float>();
        const Eigen::Matrix4f viewed = matrix->cast<float>();
        const Eigen::Matrix4f inverse = (projected * viewed).inverse();
        if (!projected.allFinite() || !viewed.allFinite() || !inverse.allFinite())
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
        }
        lux::render::CameraInput result;
        std::copy_n(projected.data(), 16, result.projection.data());
        std::copy_n(viewed.data(), 16, result.view.data());
        std::copy_n(inverse.data(), 16, result.inverse_view_projection.data());
        const Eigen::Vector3f direction = -viewed.block<1, 3>(2, 0).transpose();
        for (std::size_t i = 0; i < 3; ++i)
        {
            result.position[i] = static_cast<float>(transform.translation[i]);
            result.direction[i] = direction[i];
        }
        std::visit([&](const auto& projection) noexcept
        {
            result.near_distance = static_cast<float>(projection.near_plane);
            result.far_distance = static_cast<float>(projection.far_plane);
        }, view.projection);
        result.background = view.background;
        result.exposure = std::exp2(view.exposure);
        return result;
    }

    lux::render::RenderResult<void>
    writeViewParameters(
        ecs::Registry& registry, ecs::Entity entity,
        lux::render::FrameDataBinding<lux::render::CameraInput> binding, lux::render::FrameWriter& frame
    ) noexcept
    {
        const bool present = registry.valid(entity) &&
            registry.all_of<scene::ViewComponent, ecs::Transform3D, RenderViewComponent>(entity);
        if (!present)
        {
            return {};
        }
        const auto& output = registry.get<RenderViewComponent>(entity);
        auto parameters = makeViewParameters(
            registry.get<scene::ViewComponent>(entity), registry.get<ecs::Transform3D>(entity), output.description
        );
        if (!parameters)
        {
            return cxx::unexpected(parameters.error());
        }
        binding.destination.scope = output.attachment;
        return frame.write(binding, *parameters);
    }
}
