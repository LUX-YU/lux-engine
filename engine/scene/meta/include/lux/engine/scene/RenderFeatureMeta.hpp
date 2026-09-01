#pragma once

#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>

#include <lux/engine/meta/Meta.hpp>

#include <span>
#include <string_view>

namespace lux::scene
{
    struct RenderFeatureMeta final
    {
        render::FeatureTypeId type{};
        std::string_view stable_name;
        std::string_view display_name;
        const render::RenderFeatureRegistration* registration{};
        const meta::RefClass* configuration_reflection{};
        std::span<const std::byte> default_configuration{};
        bool scene_configurable{true};
        CreateRenderSyncStageFn create_sync_stage{};
        std::span<const ComponentObservationSpec> observations{};
    };
} // namespace lux::scene
