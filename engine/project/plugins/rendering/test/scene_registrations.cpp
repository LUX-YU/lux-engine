#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <cassert>
#include <cstdio>

int main(int argc, char** argv)
{
    assert(argc == 3);
    using namespace lux;
    project::SceneRegistrations retained;
    std::size_t expected_components{}, expected_scene_systems{}, expected_features{};
    {
        project::PluginCatalog catalog;
        assert(catalog.read(argv[1], argv[2]));
        const project::MetadataIdentity selected{"lux.builtin.scene_render", 1};
        auto manager = project::PluginManager::create(std::move(catalog), std::span{&selected, 1});
        assert(manager);
        for (const auto& library : manager->libraries())
        {
            expected_components += library->components().size();
            expected_scene_systems += library->sceneSystems().size();
            auto graphics = project::readPluginRendering(*library, manager->libraries());
            assert(graphics);
            expected_features += graphics->features.size();
        }
        auto registrations = project::readSceneRegistrations({}, manager->libraries());
        assert(registrations && registrations->components.all().size() == expected_components);
        assert(registrations->scene_systems.size() == expected_scene_systems);
        assert(registrations->features.size() == expected_features && !registrations->render_bindings.empty());
        assert(registrations->simulation_systems);
        // A duplicated runtime schema remains a real error, not a silently replaced provider.
        assert(!registrations->components.all().empty());
        const auto schema = registrations->components.all().front();
        auto duplicate = project::readSceneRegistrations(std::span{&schema, 1}, manager->libraries());
        assert(!duplicate && duplicate.error().code == project::EPluginError::REGISTRATION_FAILURE);
        retained = std::move(*registrations);
    }
    // Values outlive the module catalog/manager. Their verified code ownership is sufficient.
    for (const auto& schema : retained.components.all())
    {
        assert(schema.code_lifetime);
        if (schema.create)
            assert(schema.create(schema.code_lifetime));
    }
    assert(retained.features.size() == expected_features && retained.scene_systems.size() == expected_scene_systems);
    std::puts("PASS runtime scene assembly and retained plugin schema callbacks without Editor metadata");
}
