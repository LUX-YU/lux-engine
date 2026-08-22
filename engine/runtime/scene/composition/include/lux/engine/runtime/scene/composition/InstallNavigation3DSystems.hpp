#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/runtime/scene/composition/navigation3d_visibility.h>
#include <lux/engine/runtime/spatial3d/navigation/Navigation3DPrepareService.hpp>

namespace lux::runtime
{
    [[nodiscard]] LUX_ENGINE_RUNTIME_NAVIGATION3D_SYSTEMS_PUBLIC
    bool installNavigation3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        spatial3d::Navigation3DPrepareClient preparation);
}
