#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/integration/presentation2d/visibility.h>

namespace lux::ecs
{
    [[nodiscard]] LUX_ECS_PRESENTATION2D_PUBLIC
    bool installPresentation2DSystems(
        ScheduleBuilder& builder,
        const ComponentTypeCatalog& components);
}
