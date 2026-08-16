#include <lux/engine/runtime/packs/spatial2d/Transform2DContribution.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/transform/systems/Transform2DSystem.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace lux::runtime
{
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeSpatial2DTransformContribution(
        const lux::ecs::ComponentTypeCatalog& components)
    {
        using namespace lux::ecs;
        constexpr std::string_view required_components[]{
            "lux.ecs.transform2dcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::extensions::ContributionId{
            std::string{kSpatial2DTransformContributionName}};
        descriptor.display_name = "2D transform resolution";
        descriptor.provided_services = {typeToken<Transform2DSystem>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext&,
            ContributionConfig)
        {
            return builder.addServiceSystem(
                std::make_unique<Transform2DSystem>());
        };
        return descriptor;
    }
}
