#include <lux/engine/editor/ui/SceneConfigurationElement.hpp>
#include <lux/engine/editor/metadata/EditorPlugin.hpp>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <cassert>
#include <cstdio>
#include <algorithm>

namespace
{
    lux::ui::NumericEdit* coordinateField(lux::object::LuxObject& object)
    {
        if (auto* field = dynamic_cast<lux::ui::NumericEdit*>(&object))
            if (field->id().name() == "Coordinate page size")
                return field;
        for (auto* child = object.firstChild(); child; child = child->nextSibling())
            if (auto* found = coordinateField(*child))
                return found;
        return nullptr;
    }
}

int main(int argc, char** argv)
{
    using namespace lux;
    assert(argc == 2);
    const std::filesystem::path installation{argv[1]};
    auto reflection = editor::acquireEditorReflection();
    lux::project::PluginCatalog catalog;
    assert(catalog.read(installation / "share/lux-engine/plugins/catalog.json", installation));
    const lux::project::MetadataIdentity selected{"lux.builtin.scene_render", 1};
    auto manager = lux::project::PluginManager::create(std::move(catalog), std::span(&selected, 1));
    assert(manager);
    std::vector<editor::EditorPlugin> extensions;
    std::vector<editor::ConfigurationEditorRegistration> configurations;
    auto draft = meta::ReflectionRegistry::beginDraft();
    for (const auto& library : manager->libraries())
    {
        auto extension =
            editor::loadEditorPlugin(*manager->catalog().find(library->identity().id), *library, extensions);
        assert(extension);
        if (extension->exports)
        {
            assert(draft.appendOnce(extension->exports->register_types, extension->code));
            for (auto configuration :
                 std::span{extension->exports->configurations, extension->exports->configuration_count})
            {
                configuration.code_lifetime = extension->code;
                configurations.push_back(std::move(configuration));
            }
        }
        extensions.push_back(std::move(*extension));
    }
    assert(draft.commit());
    // A creation form in an already open Editor reuses the same admitted reflection callbacks.
    auto second = meta::ReflectionRegistry::beginDraft();
    for (const auto& extension : extensions)
        if (extension.exports)
            assert(second.appendOnce(extension.exports->register_types, extension.code));
    assert(second.commit());
    auto registrations = lux::project::readSceneRegistrations({}, manager->libraries());
    assert(registrations);
    auto queue = object::ObjectMessageQueue::create(64);
    assert(queue);
    auto root = ui::Root::create(queue->dispatcherRef());
    assert(root);
    ui::Pane pane(**root, ui::PaneId{"scene-configuration"}, ui::PaneTypeId{"test"}, "Scene Configuration");
    ui::Layout layout(pane, ui::ElementId{"content"});
    pane.setContent(layout);
    constexpr editor::ui::SceneProviderOption providers[]{
        {"lux.render.runtime", "main-window"},
        {"lux.render.scene_bindings", "render-bindings"},
        {"lux.render.resources", "resources"},
        {"lux.render.assets", "assets"},
        {"lux.world.loading", "world-storage"}
    };
    editor::EditorResult<void> status;
    editor::ui::SceneConfigurationElement element(
        layout,
        ui::ElementId{"configuration"},
        manager->catalog(),
        *registrations,
        configurations,
        providers,
        status
    );
    assert(status);
    for (const auto preset :
         {editor::ui::ESceneContentPreset::TWO_DIMENSIONAL, editor::ui::ESceneContentPreset::THREE_DIMENSIONAL})
    {
        auto applied = element.applyPreset(preset);
        if (!applied)
            std::fprintf(stderr, "%s: %s\n", applied.error().domain.c_str(), applied.error().message.c_str());
        assert(applied);
        auto* field = coordinateField(element);
        assert(field);
        field->setValue(256.0);
        field->setValue(-1.0);
        assert(!element.build());
        field->setValue(256.0);
        for (const auto stage :
             {editor::ui::ESceneConfigurationStage::CONTENT,
              editor::ui::ESceneConfigurationStage::SIMULATION,
              editor::ui::ESceneConfigurationStage::SCENE,
              editor::ui::ESceneConfigurationStage::FEATURES,
              editor::ui::ESceneConfigurationStage::RELATIONSHIPS})
        {
            element.setStage(stage);
            ui::DrawData data;
            assert((*root)->update({{1100, 820}, 1.0F / 60}, &data));
        }
        auto built = element.build();
        assert(built);
        assert(built->simulation->systemCount() == 0 && built->scene.systemCount() == 3);
        const auto render = built->scene.systemAt(2);
        scene::RenderSystemConfiguration configuration;
        const auto registered =
            std::ranges::find(registrations->scene_systems, render.type(), &scene::SceneSystemRegistration::type);
        assert(registered != registrations->scene_systems.end());
        assert(registered->configuration.decode(render.configurationPayload(), &configuration));
        assert(configuration.coordinate_page_size == 256.0 && render.requirementBindingCount() == 4);
        const auto expected = preset == editor::ui::ESceneContentPreset::TWO_DIMENSIONAL ? "lux.render.canvas2d.v2"
                                                                                         : "lux.render.forward_mesh.v1";
        const auto feature = std::ranges::find_if(registrations->features, [&](const auto& value) {
            return value.factory.descriptor.canonical_name == expected;
        });
        assert(feature != registrations->features.end());
        assert(
            std::ranges::find(
                configuration.features,
                feature->factory.descriptor.type,
                &scene::RenderFeatureInstanceDescription::type
            ) != configuration.features.end()
        );
        const auto id = asset::AssetId{*uuids::uuid::from_string("b3e1486c-3744-476d-acb8-e4f93a737a3b")};
        auto package = scene::createScenePackage(id, built->name, built->schemas, built->simulation, built->scene);
        assert(package && package->world->data().partitioner().id.name == "lux.spatial.builtin.single");
        assert(
            package->scene->data().systemAt(2).requirementBindingAt(0).provider() ==
            render.requirementBindingAt(0).provider()
        );
        auto encoded = scene::encodeScenePackage(*package, 32 * 1024 * 1024);
        assert(encoded);
    }
}
