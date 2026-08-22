#pragma once
/**
 * @file Spatial3DPartitionedContribution.hpp
 * @brief Scene contribution which installs cooked 3D spatial residency.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/spatial3d/SceneCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialDemand.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/contribution_visibility.h>

namespace lux::runtime
{
    /// Stable identity of a cooked source/channel/level band. Policy fields
    /// and canonical vector ordinals deliberately do not participate.
    [[nodiscard]] LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_CONTRIBUTION_PUBLIC
    lux::runtime::spatial_partition::SpatialDemandSourceId
    spatial3DDemandSourceNamespace(
        const lux::spatial3d::SceneCatalogBand& band);

    [[nodiscard]] LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_CONTRIBUTION_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeSpatial3DPartitionedContribution(
        const lux::ecs::ComponentTypeCatalog& components);
}
