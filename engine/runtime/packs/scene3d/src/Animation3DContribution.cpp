#include <lux/engine/runtime/packs/spatial3d/Animation3DContribution.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/animation/systems/AnimationSystem.hpp>
#include <lux/engine/ecs/animation/systems/SkeletalAnimationResolver.hpp>
#include <lux/engine/runtime/assets/SceneAssetServices.hpp>
#include <lux/engine/runtime/spatial3d/transform/Spatial3DTransformContribution.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lux::runtime
{
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeAnimation3DContribution(
        const lux::ecs::ComponentTypeCatalog& components)
    {
        using namespace lux::ecs;
        constexpr std::string_view required_components[]{
            "lux.ecs.animatorcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::extensions::ContributionId{
            std::string{kAnimation3DContributionName}};
        descriptor.display_name = "3D animation";
        descriptor.required_contributions.emplace_back(
            std::string{kSpatial3DTransformContributionName});
        descriptor.required_services = {
            typeToken<lux::asset_runtime::SceneAssetServices>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext& context,
            ContributionConfig) -> lux::cxx::expected<
                void,
                SceneContributionBuildFailure>
        {
            auto* const assets = builder.findService<
                lux::asset_runtime::SceneAssetServices>(context);
            if (!assets)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::MISSING_SERVICE,
                        typeToken<lux::asset_runtime::
                            SceneAssetServices>()});
            }
            const auto request_load = [client = assets->loads](
                const lux::asset::asset_id_t& id) noexcept
            {
                static_cast<void>(client.request(id));
            };
            if (auto added = builder.add(
                    std::make_unique<SkeletalAnimationResolver>(
                        assets->manager,
                        request_load),
                    kPhasePreTransform); !added)
            {
                return added;
            }
            return builder.add(std::make_unique<AnimationSystem>());
        };
        return descriptor;
    }
}
