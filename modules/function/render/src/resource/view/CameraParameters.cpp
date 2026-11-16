#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/resource/view/CameraParameters.hpp>

#include <cstring>

namespace lux::render::detail
{
    rdesc::ShaderBlock cameraParameterLayout() noexcept
    {
        rdesc::ShaderBlock layout{sizeof(CameraParameters), 16};
        layout.fields = {
            {"view", rdesc::EDataScalar::FLOAT, offsetof(CameraParameters, view), 4, 4, 0, 16},
            {"projection", rdesc::EDataScalar::FLOAT, offsetof(CameraParameters, projection), 4, 4, 0, 16},
            {"inverse_view_projection",
             rdesc::EDataScalar::FLOAT,
             offsetof(CameraParameters, inverse_view_projection),
             4,
             4,
             0,
             16},
            {"position_near", rdesc::EDataScalar::FLOAT, offsetof(CameraParameters, position_near), 4, 1, 0, 0},
            {"direction_far", rdesc::EDataScalar::FLOAT, offsetof(CameraParameters, direction_far), 4, 1, 0, 0},
            {"viewport", rdesc::EDataScalar::FLOAT, offsetof(CameraParameters, viewport), 4, 1, 0, 0},
            {"background", rdesc::EDataScalar::FLOAT, offsetof(CameraParameters, background), 4, 1, 0, 0},
            {"exposure", rdesc::EDataScalar::FLOAT, offsetof(CameraParameters, exposure), 1, 1, 0, 0}
        };
        return layout;
    }

    RenderResult<void>
    projectCameraInput(cxx::type_info type, std::span<const std::byte> source, std::span<std::byte> destination) noexcept
    {
        const auto camera_type = cxx::make_basic_type_info<CameraInput>();
        const bool is_wrong_type = type != camera_type || type.name() != camera_type.name();
        const bool is_wrong_size = source.size() != sizeof(CameraInput);
        if (is_wrong_type || is_wrong_size)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        CameraInput input;
        std::memcpy(&input, source.data(), sizeof(input));
        return projectCameraParameters(input, destination);
    }

    RenderResult<void> projectCameraParameters(const CameraInput& input, std::span<std::byte> destination) noexcept
    {
        if (destination.size() != sizeof(CameraParameters))
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const CameraParameters result{
            input.view,
            input.projection,
            input.inverse_view_projection,
            {input.position[0], input.position[1], input.position[2], input.near_distance},
            {input.direction[0], input.direction[1], input.direction[2], input.far_distance},
            input.viewport,
            input.background,
            input.exposure
        };
        std::memcpy(destination.data(), &result, sizeof(result));
        return {};
    }
} // namespace lux::render::detail
