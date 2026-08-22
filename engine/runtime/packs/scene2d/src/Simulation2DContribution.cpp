#include <lux/engine/runtime/packs/spatial2d/Simulation2DContribution.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>
#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>

#include <memory>
#include <string>

namespace lux::runtime
{
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeSimulation2DContribution(
        const lux::ecs::ComponentTypeCatalog&)
    {
        using namespace lux::ecs;
        SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            std::string{kSimulation2DContributionName}};
        descriptor.display_name = "2D fixed-step simulation";
        descriptor.provided_services = {
            lux::cxx::typeToken<Simulation2DSystem>(),
            lux::cxx::typeToken<CollisionProbes2D>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext& context,
            ContributionConfig) -> lux::cxx::expected<
                void,
                SceneContributionBuildFailure>
        {
            const auto* configured = builder.findService<FixedStepConfig>(
                context);
            if (auto added = builder.addServiceSystem(
                    std::make_unique<Simulation2DSystem>(
                        configured ? *configured : FixedStepConfig{}));
                !added)
            {
                return added;
            }
            return builder.publishService(
                std::make_unique<CollisionProbes2D>());
        };
        return descriptor;
    }
}
