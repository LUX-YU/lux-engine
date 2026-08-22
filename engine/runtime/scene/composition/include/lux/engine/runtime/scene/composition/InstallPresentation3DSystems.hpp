#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/runtime/scene/composition/presentation3d_visibility.h>
#include <lux/engine/ecs/render/SceneGeometryPreparation.hpp>

#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>

#include <memory>
#include <string_view>
#include <vector>

namespace lux::runtime
{
    [[nodiscard]] LUX_ENGINE_RUNTIME_PRESENTATION3D_SYSTEMS_PUBLIC
    bool installPresentation3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components);

    [[nodiscard]] LUX_ENGINE_RUNTIME_PRESENTATION3D_SYSTEMS_PUBLIC
    bool installPresentation3DRendering(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        lux::ecs::ClassicMeshPreparePort classic_mesh_preparation,
        lux::ecs::TerrainPreparePort terrain_preparation,
        std::vector<std::unique_ptr<lux::ecs::RenderStage>>& stages,
        std::vector<std::string_view>& feature_roots,
        lux::ecs::ResidencySubsystem& residency);
}
