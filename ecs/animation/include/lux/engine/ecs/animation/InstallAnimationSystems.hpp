#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/animation/systems/AnimationSystem.hpp>
#include <lux/engine/ecs/animation/systems/SkeletalAnimationAssetResolver.hpp>
#include <lux/engine/resource/asset/AssetServices.hpp>

#include <memory>
#include <string_view>

namespace lux::ecs
{
    [[nodiscard]] inline bool installAnimation3DSystems(
        ScheduleBuilder& builder,
        const ComponentTypeCatalog& components)
    {
        constexpr std::string_view required_components[]{
            "lux.ecs.animatorcomponent"};
        if (!validateComponentSchemas(components, required_components))
            return false;
        const auto checkpoint = builder.checkpoint();
        if (!checkpoint.valid())
            return false;
        auto* const assets = builder.services().borrow<lux::asset::AssetServices>();
        if (!assets ||
            !builder.add(
                std::make_unique<SkeletalAnimationAssetResolver>(
                    assets->manager,
                    assets->loads),
                kPhasePreTransform) ||
            !builder.add(std::make_unique<AnimationSystem>()))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }
}
