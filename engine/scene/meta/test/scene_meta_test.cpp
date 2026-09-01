#include <lux/engine/scene/SceneMetaManager.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureMetaModule.hpp>

#include <cassert>
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

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
    lux::render::initializeBuiltinRenderFeatureMeta();
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
    const auto builtin_features = lux::render::builtinRenderFeatureRegistrations();
    std::vector<lux::render::RenderFeatureRegistration> render_features(
        builtin_features.begin(),
        builtin_features.end()
    );
    auto manager = lux::scene::SceneMetaManager::build({
        std::move(*components),
        lux::simulation::SimulationSystemRegistry{},
        {registration},
        std::move(render_features),
        {}
    });
    assert(manager);
    assert(manager->getSystemMeta("lux.scene.probe"));
    assert(manager->getSceneSystemMeta(registration.type) != nullptr);
    assert(manager->systemsProvidingCapability("lux.scene.probe").size() == 1U);
    assert(manager->allSystems().size() == 1U);
    const auto* render_feature = manager->getRenderFeatureMeta("lux.render.tonemap.v1");
    assert(render_feature != nullptr && render_feature->configuration_reflection != nullptr);
    assert(!render_feature->default_configuration.empty());
    assert(manager->allRenderFeatures().size() == 34U);
    const auto* point_cloud = manager->getRenderFeatureMeta("lux.render.point_cloud_simple.v1");
    assert(point_cloud != nullptr && !point_cloud->scene_configurable);
    lux::meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
