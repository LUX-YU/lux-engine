#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/runtime/scene/composition/physics3d_visibility.h>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DPrepareService.hpp>

namespace lux::runtime
{
    [[nodiscard]] LUX_ENGINE_RUNTIME_PHYSICS3D_SYSTEMS_PUBLIC
    bool installPhysics3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        spatial3d::StaticCollider3DPrepareClient preparation);
}
