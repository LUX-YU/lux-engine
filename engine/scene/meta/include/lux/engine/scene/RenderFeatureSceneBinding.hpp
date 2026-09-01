#pragma once

#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::scene
{
    class RenderSyncStage;

    enum class ERenderSyncStageCreateError : std::uint8_t
    {
        INVALID_CONFIGURATION,
        ALLOCATION_FAILURE,
    };

    struct RenderSyncStageCreateFailure final
    {
        ERenderSyncStageCreateError code{ERenderSyncStageCreateError::INVALID_CONFIGURATION};
    };

    struct RenderSyncStageCreateInfo final
    {
        simulation::ecs::Registry& registry;
        render::RenderSceneId scene;
        const render::FeatureCatalog& catalog;
        render::FeatureTypeId feature{};
        render::FeatureHandle feature_handle{};
        double coordinate_page_size{1024.0};
        std::array<std::int64_t, 3> scene_origin_page{};
    };

    using CreateRenderSyncStageFn = lux::cxx::expected<
        std::unique_ptr<RenderSyncStage>,
        RenderSyncStageCreateFailure> (*)(const RenderSyncStageCreateInfo& info) noexcept;

    struct RenderFeatureSceneBinding final
    {
        system::SystemTypeId scene_system;
        render::FeatureTypeId feature{};
        std::span<const ComponentObservationSpec> observations{};
        CreateRenderSyncStageFn create_sync_stage{};
    };
} // namespace lux::scene
