#include <lux/engine/function/render/client/features/view_camera/ViewCameraOperation.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <span>

namespace lux::render
{
    void viewCameraUpdate(
        ViewCameraProxy proxy,
        RenderSceneId scene_id,
        ViewHandle view,
        const float view_matrix[16],
        const float proj_matrix[16],
        const RenderLargePosition3D& render_origin,
        float coordinate_page_size
    )
    {
        ViewCameraUpdatePayload payload{};
        payload.scene_id = scene_id;
        payload.view = view;
        std::memcpy(
            payload.view_matrix,
            view_matrix,
            sizeof(payload.view_matrix)
        );
        // The GPU contract carries only camera rotation. Translation is
        // reconstructed from render_origin in page space.
        payload.view_matrix[12] = 0.0f;
        payload.view_matrix[13] = 0.0f;
        payload.view_matrix[14] = 0.0f;
        std::memcpy(
            payload.proj_matrix,
            proj_matrix,
            sizeof(payload.proj_matrix)
        );
        payload.render_origin = render_origin;
        payload.coordinate_page_size = coordinate_page_size;
        proxy.update(
            std::span<const ViewCameraUpdatePayload>{&payload, 1}
        );
    }

    void viewCameraUpdateTransient(
        ViewCameraProxy proxy,
        RenderSceneId scene_id,
        ViewHandle view,
        const float view_matrix[16],
        const float proj_matrix[16],
        const float camera_position[3],
        float coordinate_page_size
    )
    {
        RenderLargePosition3D origin{};
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const double value = static_cast<double>(camera_position[axis]);
            const double page = std::floor(value / coordinate_page_size);
            if (page < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
                page > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
            {
                return;
            }
            origin.page_delta[axis] = static_cast<std::int32_t>(page);
            origin.local[axis] = static_cast<float>(
                value - page * static_cast<double>(coordinate_page_size));
        }
        viewCameraUpdate(
            proxy,
            scene_id,
            view,
            view_matrix,
            proj_matrix,
            origin,
            coordinate_page_size
        );
    }
} // namespace lux::render
