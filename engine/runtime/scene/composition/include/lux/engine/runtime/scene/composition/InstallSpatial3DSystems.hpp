#pragma once
/**
 * @file InstallSpatial3DSystems.hpp
 * @brief Direct assembly of cooked Spatial3D residency Systems.
 */

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/runtime/scene/composition/spatial3d_visibility.h>

namespace lux::runtime
{
    [[nodiscard]] LUX_ENGINE_RUNTIME_SPATIAL3D_SYSTEMS_PUBLIC
    bool installSpatial3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components);
}
