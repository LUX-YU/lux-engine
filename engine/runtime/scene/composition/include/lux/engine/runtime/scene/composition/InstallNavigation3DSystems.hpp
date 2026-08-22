#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/navigation/streaming/Navigation3DPreparePort.hpp>
#include <lux/engine/runtime/scene/composition/navigation3d_visibility.h>

namespace lux::runtime
{
    [[nodiscard]] LUX_ENGINE_RUNTIME_NAVIGATION3D_SYSTEMS_PUBLIC
    bool installNavigation3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        lux::ecs::navigation::streaming::Navigation3DPrepareClient
            preparation);
}
