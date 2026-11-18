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
            {"background", rdesc::EDataScalar::FLOAT, offsetof(CameraParameters, background), 4, 1, 0, 0}
        };
        return layout;
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
            input.background
        };
        std::memcpy(destination.data(), &result, sizeof(result));
        return {};
    }
} // namespace lux::render::detail
