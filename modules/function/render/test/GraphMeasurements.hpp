#pragma once

#include <cstddef>
#include <lux/engine/render/visibility.h>

namespace lux::render::detail
{
    class DeviceSession;
    struct PreparedRenderGraph;

    LUX_RENDER_PUBLIC void startGraphMeasurements() noexcept;
    LUX_RENDER_PUBLIC std::size_t stopGraphMeasurements() noexcept;
}

namespace foundation_test
{
    int measureGraph(lux::render::detail::DeviceSession&, const lux::render::detail::PreparedRenderGraph&);
}
