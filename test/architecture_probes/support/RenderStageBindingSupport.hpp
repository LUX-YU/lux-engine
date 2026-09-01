#pragma once

#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>

#include <algorithm>

namespace lux::scene::qualification
{
    [[nodiscard]] inline lux::cxx::expected<std::unique_ptr<RenderSyncStage>, RenderSyncStageCreateFailure>
    createBuiltinRenderStage(
        render::FeatureTypeId type,
        simulation::ecs::Registry& registry,
        render::RenderSceneId scene,
        const render::FeatureCatalog& catalog,
        double coordinate_page_size = 1024.0
    ) noexcept
    {
        const auto bindings = builtinRenderFeatureSceneBindings();
        const auto found = std::find_if(bindings.begin(), bindings.end(), [type](const auto& value) noexcept {
            return value.feature == type;
        });
        if (found == bindings.end())
        {
            return lux::cxx::unexpected(RenderSyncStageCreateFailure{
                ERenderSyncStageCreateError::INVALID_CONFIGURATION
            });
        }
        const RenderSyncStageCreateInfo info{
            registry,
            scene,
            catalog,
            type,
            render::FeatureHandle{1U, 1U},
            coordinate_page_size,
            {}
        };
        return found->create_sync_stage(info);
    }
} // namespace lux::scene::qualification
