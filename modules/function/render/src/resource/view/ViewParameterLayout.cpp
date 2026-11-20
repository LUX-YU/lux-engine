#include <lux/engine/render/resource/view/ViewParameterStorage.hpp>

#include <cstddef>

namespace lux::render::detail
{
    rdesc::ShaderBlock viewParameterLayout() noexcept
    {
        rdesc::ShaderBlock layout{sizeof(ViewGpuParameters), 16};
        layout.fields = {
            {"view", rdesc::EDataScalar::FLOAT, offsetof(ViewGpuParameters, view), 4, 4, 0, 16},
            {"projection", rdesc::EDataScalar::FLOAT, offsetof(ViewGpuParameters, projection), 4, 4, 0, 16},
            {"inverse_view_projection",
             rdesc::EDataScalar::FLOAT,
             offsetof(ViewGpuParameters, inverse_view_projection),
             4,
             4,
             0,
             16},
            {"position_near", rdesc::EDataScalar::FLOAT, offsetof(ViewGpuParameters, position_near), 4, 1, 0, 0},
            {"direction_far", rdesc::EDataScalar::FLOAT, offsetof(ViewGpuParameters, direction_far), 4, 1, 0, 0},
            {"viewport", rdesc::EDataScalar::FLOAT, offsetof(ViewGpuParameters, viewport), 4, 1, 0, 0},
            {"background", rdesc::EDataScalar::FLOAT, offsetof(ViewGpuParameters, background), 4, 1, 0, 0}
        };
        return layout;
    }
} // namespace lux::render::detail
