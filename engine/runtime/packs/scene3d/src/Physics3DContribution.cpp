#include <lux/engine/runtime/packs/spatial3d/Physics3DContribution.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/physics3d/Physics3DConfig.hpp>
#include <lux/engine/ecs/physics3d/systems/Physics3DScene.hpp>
#include <lux/engine/ecs/physics3d/systems/Physics3DSystem.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/scene/SceneAsyncContext.hpp>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DSystem.hpp>
#include <lux/engine/runtime/spatial3d/transform/Spatial3DTransformContribution.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace lux::runtime
{
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makePhysics3DContribution(
        const lux::ecs::ComponentTypeCatalog& components,
        spatial3d::StaticCollider3DPrepareClient preparation)
    {
        using namespace lux::ecs;
        constexpr std::string_view required_components[]{
            "lux.ecs.staticcolliderbatch3dcomponent",
            "lux.ecs.rigidbody3dcomponent",
            "lux.ecs.collider3dcomponent",
            "lux.ecs.charactercontroller3dcomponent",
            "lux.ecs.collisionfilter3dcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            std::string{kPhysics3DContributionName}};
        descriptor.display_name = "3D physics";
        descriptor.required_contributions.emplace_back(
            std::string{kSpatial3DTransformContributionName});
        descriptor.required_services = {
            typeToken<SceneAsyncContext>(),
            typeToken<entity_scene::ContentBlobClient>()};
        descriptor.provided_services = {
            typeToken<spatial3d::Physics3DSceneService>()};
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
            if (!blobs || !async)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::MISSING_SERVICE,
                        !blobs
                            ? typeToken<entity_scene::ContentBlobClient>()
                            : typeToken<SceneAsyncContext>()});
            }
            if (!preparation)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::BUILD_REJECTED});
            }

            const auto* configured = builder.findService<Physics3DConfig>(
                context);
            auto scene = Physics3DScene::create(
                configured ? *configured : Physics3DConfig{});
            if (!scene)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::BUILD_REJECTED});
            }
            auto shared_scene = std::move(*scene);
            if (auto published = builder.publishService(
                    std::make_unique<spatial3d::Physics3DSceneService>(
                        spatial3d::Physics3DSceneService{shared_scene}));
                !published)
            {
                return published;
            }
            if (auto added = builder.add(
                    std::make_unique<Physics3DSystem>(shared_scene),
                    kPhaseSimulation); !added)
            {
                return added;
            }
            return builder.add(
                std::make_unique<spatial3d::StaticCollider3DSystem>(
                    async->runtime(),
                    async->scope(),
                    preparation,
                    std::move(shared_scene),
                    *blobs),
                kPhaseSimulation);
        };
        return descriptor;
    }
}
