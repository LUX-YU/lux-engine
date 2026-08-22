#pragma once
/**
 * @file InstallSpatial3DStreamingSystems.hpp
 * @brief Direct assembly of cooked Spatial3D residency Systems.
 */

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/spatial3d/SceneCatalog.hpp>
#include <lux/engine/ecs/entity_scene/residency/SectionResidencyDemand.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/systems_visibility.h>

namespace lux::runtime
{
    /// Stable identity of a cooked source/channel/level band. Policy fields
    /// and canonical vector ordinals deliberately do not participate.
    [[nodiscard]] LUX_ENGINE_RUNTIME_SPATIAL3D_STREAMING_SYSTEMS_PUBLIC
    lux::ecs::entity_scene::residency::SectionDemandSourceId
    spatial3DDemandSourceNamespace(
        const lux::spatial3d::SceneCatalogBand& band);

    [[nodiscard]] LUX_ENGINE_RUNTIME_SPATIAL3D_STREAMING_SYSTEMS_PUBLIC
    bool installSpatial3DStreamingSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components);
}
