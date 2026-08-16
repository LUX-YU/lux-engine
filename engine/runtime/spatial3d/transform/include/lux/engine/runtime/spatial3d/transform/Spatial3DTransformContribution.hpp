#pragma once
/**
 * @file Spatial3DTransformContribution.hpp
 * @brief Headless-capable 3D transform resolver contribution.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/spatial3d/transform/visibility.h>

#include <string_view>

namespace lux::runtime
{
    inline constexpr std::string_view kSpatial3DTransformContributionName =
        "org.lux.builtin.spatial3d.transform";

    [[nodiscard]] constexpr lux::extensions::ContributionIdView
    spatial3DTransformContributionId() noexcept
    {
        return lux::extensions::contributionId(
            kSpatial3DTransformContributionName);
    }

    [[nodiscard]] LUX_ENGINE_RUNTIME_SPATIAL3D_TRANSFORM_PUBLIC
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeSpatial3DTransformContribution(
        const lux::ecs::ComponentTypeCatalog& components);
}
