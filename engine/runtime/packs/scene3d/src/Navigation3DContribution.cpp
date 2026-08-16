#include <lux/engine/runtime/packs/spatial3d/Navigation3DContribution.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/navigation/NavigationQueryService.hpp>
#include <lux/engine/ecs/navigation/systems/Navigation3DSystem.hpp>
#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/scene/SceneAsyncContext.hpp>
#include <lux/engine/runtime/spatial3d/navigation/Spatial3DNavigationAdapterSystem.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace lux::runtime
{
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeNavigation3DContribution(
        const lux::ecs::ComponentTypeCatalog& components,
        spatial3d::Navigation3DPrepareClient preparation)
    {
        using namespace lux::ecs;
        constexpr std::string_view required_components[]{
            "lux.ecs.navigationregion3dcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::extensions::ContributionId{
            std::string{kNavigation3DContributionName}};
        descriptor.display_name = "3D navigation";
        descriptor.required_services = {
            typeToken<SceneAsyncContext>(),
            typeToken<entity_scene::ContentBlobClient>()};
        descriptor.provided_services = {
            typeToken<NavigationQueryService>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [preparation](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext& context,
            ContributionConfig) -> lux::cxx::expected<
                void,
                SceneContributionBuildFailure>
        {
            auto* const blobs = builder.findService<
                entity_scene::ContentBlobClient>(context);
            auto* const async = builder.findService<SceneAsyncContext>(
                context);
            auto backend = lux::navigation::detour3d::
                Navigation3DBackend::create();
            if (!blobs || !async || !preparation || !backend)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        !blobs || !async
                            ? ESceneContributionBuildError::MISSING_SERVICE
                            : ESceneContributionBuildError::BUILD_REJECTED,
                        !blobs
                            ? typeToken<entity_scene::ContentBlobClient>()
                            : (!async
                                  ? typeToken<SceneAsyncContext>()
                                  : TypeToken{})});
            }

            auto navigation_owner =
                std::make_unique<Navigation3DSystem>(*backend);
            auto* const navigation = navigation_owner.get();
            if (auto added = builder.add(std::move(navigation_owner));
                !added)
            {
                return added;
            }
            if (auto published = builder.publishService(
                    std::make_unique<NavigationQueryService>(*navigation));
                !published)
            {
                return published;
            }
            return builder.add(
                std::make_unique<spatial3d::
                    Spatial3DNavigationAdapterSystem>(
                        async->runtime(),
                        async->scope(),
                        preparation,
                        *navigation,
                        *blobs));
        };
        return descriptor;
    }
}
