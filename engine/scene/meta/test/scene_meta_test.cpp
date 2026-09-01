#include <lux/engine/scene/SceneMetaManager.hpp>

#include <cassert>
#include <array>
#include <string_view>
#include <utility>

namespace
{
    struct SceneProbe final
    {
        inline static constexpr std::array Capabilities{std::string_view{"lux.scene.probe"}};
        inline static constexpr lux::system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.probe",
            .version = 1U,
            .capabilities = Capabilities
        };
    };

    lux::cxx::expected<void, lux::scene::SceneSystemBuildFailure> installProbe(
        lux::scene::SceneBuilder&,
        lux::scene::SceneSystemView
    ) noexcept
    {
        return {};
    }
}

int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    lux::scene::SceneSystemRegistration registration{
        .type = lux::system::systemTypeId(SceneProbe::Description.canonical_name),
        .cpp_type = lux::cxx::typeToken<SceneProbe>(),
        .description = &SceneProbe::Description,
        .configuration = lux::serialization::noPortableValueCodec(),
        .install = &installProbe
    };
    auto components = lux::simulation::ecs::ComponentSchemaSet::build({});
    assert(components);
    auto manager = lux::scene::SceneMetaManager::build({
        std::move(*components),
        lux::simulation::SimulationSystemRegistry{},
        {registration}
    });
    assert(manager);
    assert(manager->getSystemMeta("lux.scene.probe"));
    assert(manager->getSceneSystemMeta(registration.type) != nullptr);
    assert(manager->systemsProvidingCapability("lux.scene.probe").size() == 1U);
    assert(manager->allSystems().size() == 1U);
    lux::meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
