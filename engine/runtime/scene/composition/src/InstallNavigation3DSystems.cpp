#include <lux/engine/runtime/scene/composition/InstallNavigation3DSystems.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/navigation/NavigationQueryService.hpp>
#include <lux/engine/ecs/navigation/streaming/Spatial3DNavigationAdapterSystem.hpp>
#include <lux/engine/ecs/navigation/systems/Navigation3DSystem.hpp>
#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>

#include <memory>
#include <string_view>

namespace lux::runtime
{
    bool installNavigation3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        lux::ecs::navigation::streaming::Navigation3DPrepareClient preparation)
    {
        using namespace lux::ecs;
        constexpr std::string_view required_components[]{
            "lux.ecs.navigationregion3dcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return false;
        }

        const auto checkpoint = builder.checkpoint();
        auto* const blobs = builder.services().borrow<
            lux::ecs::entity_scene::ContentBlobClient>();
        auto backend = lux::navigation::detour3d::Navigation3DBackend::create();
        if (!blobs || !preparation || !backend)
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto navigation_owner =
            std::make_unique<Navigation3DSystem>(*backend);
        auto* const navigation = navigation_owner.get();
        if (!builder.add(std::move(navigation_owner)) ||
            !builder.services().emplace<NavigationQueryService>(
                *navigation) ||
            !builder.add(
                std::make_unique<lux::ecs::navigation::streaming::
                    Spatial3DNavigationAdapterSystem>(
                        preparation,
                        *navigation,
                        *blobs)))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }
}
