#include <lux/engine/runtime/spatial3d/transform/Spatial3DTransformContribution.hpp>

#include <lux/engine/ecs/transform/systems/Transform3DSystem.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lux::runtime
{
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makeSpatial3DTransformContribution(
        const lux::ecs::ComponentTypeCatalog& components)
    {
        constexpr std::string_view required_components[]{
            "lux.ecs.transform3dcomponent"};
        if (auto validated = lux::ecs::validateComponentSchemas(
                components, required_components); !validated)
        {
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::extensions::ContributionId{
            std::string{kSpatial3DTransformContributionName}};
        descriptor.display_name = "3D transform resolution";
        descriptor.provided_services = {
            lux::ecs::typeToken<lux::ecs::Transform3DSystem>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext&,
            ContributionConfig)
        {
            return builder.addServiceSystem(
                std::make_unique<lux::ecs::Transform3DSystem>());
        };
        return descriptor;
    }
}
