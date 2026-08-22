#include <lux/engine/runtime/packs/spatial2d/Physics2DContribution.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>
#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>
#include <lux/engine/ecs/physics2d/systems/Physics2DSystem.hpp>
#include <lux/engine/ecs/physics2d/systems/Physics2DWorld.hpp>
#include <lux/engine/runtime/packs/spatial2d/Simulation2DContribution.hpp>
#include <lux/engine/runtime/packs/spatial2d/Transform2DContribution.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace lux::runtime
{
    namespace
    {
        [[nodiscard]] SceneContributionBuildFailure missingService(
            lux::cxx::TypeToken type = {}) noexcept
        {
            return {ESceneContributionBuildError::MISSING_SERVICE, type};
        }
    }

    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makePhysics2DContribution(
        const lux::ecs::ComponentTypeCatalog& components)
    {
        using namespace lux::ecs;
        constexpr std::string_view required_components[]{
            "lux.ecs.collider2dcomponent",
            "lux.ecs.rigidbody2dcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            std::string{kPhysics2DContributionName}};
        descriptor.display_name = "2D physics";
        descriptor.required_contributions = {
            lux::scene::SceneFeatureId{
                std::string{kSimulation2DContributionName}},
            lux::scene::SceneFeatureId{
                std::string{kSpatial2DTransformContributionName}}};
        descriptor.required_services = {lux::cxx::typeToken<Simulation2DSystem>()};
        descriptor.provided_services = {lux::cxx::typeToken<Physics2DSystem>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext& context,
            ContributionConfig) -> lux::cxx::expected<
                void,
                SceneContributionBuildFailure>
        {
            const auto* configured = builder.findService<Physics2DConfig>(
                context);
            auto physics = builder.publishServiceAndGet(
                std::make_unique<Physics2DSystem>(
                    configured ? *configured : Physics2DConfig{}));
            if (!physics)
                return lux::cxx::unexpected(physics.error());
            auto* simulation = builder.findService<Simulation2DSystem>(
                context);
            if (!simulation)
            {
                return lux::cxx::unexpected(missingService(
                    lux::cxx::typeToken<Simulation2DSystem>()));
            }
            Physics2DSystem* const physics_owner = *physics;
            simulation->setPhase(
                Simulation2DSystem::Phase::SimulatePhysics,
                [physics_owner](
                    lux::ecs::Registry& registry,
                    float dt)
                {
                    physics_owner->step(registry, dt);
                });
            return {};
        };
        return descriptor;
    }

    SceneContributionDescriptor makeDemoPhysics2DContribution()
    {
        using namespace lux::ecs;
        SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            std::string{kDemoPhysics2DContributionName}};
        descriptor.display_name = "2D demo swept physics";
        descriptor.required_contributions = {
            lux::scene::SceneFeatureId{
                std::string{kSimulation2DContributionName}},
            lux::scene::SceneFeatureId{
                std::string{kSpatial2DTransformContributionName}}};
        descriptor.required_services = {
            lux::cxx::typeToken<Simulation2DSystem>(),
            lux::cxx::typeToken<CollisionProbes2D>()};
        descriptor.provided_services = {lux::cxx::typeToken<Physics2DWorld>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext& context,
            ContributionConfig) -> lux::cxx::expected<
                void,
                SceneContributionBuildFailure>
        {
            const auto* configured = builder.findService<Physics2DConfig>(
                context);
            auto physics = builder.publishServiceAndGet(
                std::make_unique<Physics2DWorld>(
                    configured ? *configured : Physics2DConfig{}));
            if (!physics)
                return lux::cxx::unexpected(physics.error());
            auto* simulation = builder.findService<Simulation2DSystem>(
                context);
            auto* probes = builder.findService<CollisionProbes2D>(context);
            if (!simulation || !probes)
                return lux::cxx::unexpected(missingService());
            Physics2DWorld* const physics_owner = *physics;
            for (ICollision2DProbe* probe : probes->probes)
                physics_owner->addProbe(probe);
            simulation->setPhase(
                Simulation2DSystem::Phase::SimulatePhysics,
                [physics_owner](
                    lux::ecs::Registry& registry,
                    float dt)
                {
                    physics_owner->step(registry, dt);
                });
            return {};
        };
        return descriptor;
    }
}
