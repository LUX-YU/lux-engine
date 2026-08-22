#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/runtime/scene/composition/presentation3d_visibility.h>
#include <lux/engine/runtime/render/scene/SceneGeometryPrepareService.hpp>

namespace lux::runtime
{
    [[nodiscard]] LUX_ENGINE_RUNTIME_PRESENTATION3D_SYSTEMS_PUBLIC
    bool installPresentation3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        ClassicMeshPrepareClient classic_mesh_preparation,
        TerrainPrepareClient terrain_preparation);
}
