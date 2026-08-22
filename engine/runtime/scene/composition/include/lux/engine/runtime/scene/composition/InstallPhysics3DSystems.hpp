#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/physics3d/streaming/StaticCollider3DPreparePort.hpp>
#include <lux/engine/runtime/scene/composition/physics3d_visibility.h>

namespace lux::runtime
{
    [[nodiscard]] LUX_ENGINE_RUNTIME_PHYSICS3D_SYSTEMS_PUBLIC
    bool installPhysics3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        lux::ecs::physics3d::streaming::StaticCollider3DPrepareClient
            preparation);
}
