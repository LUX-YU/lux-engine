#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include <lux/engine/editor/scene/ConfigurationEditor.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/project/PluginRendering.hpp>
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
    std::vector<editor::extensions::EditorExtension> extensions;
    auto queue = object::ObjectMessageQueue::create(64);
    assert(queue);
    lux::editor::desktop::EditorContext editor_context{queue->dispatcherRef()};
    auto& commands = editor_context.commands();
    editor::extensions::ContributionRegistry contributions(queue->dispatcherRef(), editor_context);
    for (const auto& library : manager->libraries())
    {
        auto extension = editor::extensions::EditorExtension::load(
            *manager->catalog().find(library->identity().id),
            *library,
            extensions
        );
        assert(extension);
        extensions.push_back(std::move(*extension));
    }
    // Preserve the original duplicate-registration case through the real V7 owner batch.
    for (int attempt = 0; attempt != 2; ++attempt)
    {
        editor::extensions::ContributionDraft draft;
        for (const auto& extension : extensions)
        {
            auto supplied = extension.contributions();
            assert(supplied);
            auto append = [](auto& to, auto& from) {
                to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
            };
            append(draft.code, supplied->code);
            append(draft.reflection, supplied->reflection);
            append(draft.configurations, supplied->configurations);
        }
        auto prepared = editor::extensions::ContributionSnapshot::prepare(std::move(draft));
        assert(prepared);
        assert(contributions.enqueue(*prepared) && contributions.applyPending());
    }
    auto registrations = lux::project::readSceneRegistrations({}, manager->libraries());
    assert(registrations);
    auto root = ui::Root::create(queue->dispatcherRef());
    assert(root);
    ui::Pane pane(
        (*root)->dispatcherRef(),
        ui::PaneId{"scene-configuration"},
        ui::PaneTypeId{"test"},
        "Scene Configuration"
    );
    ui_test::mount(**root, pane);
    ui::Layout layout(pane, ui::ElementId{"content"});
    assert(pane.setContent(layout));
    constexpr editor::scene::SceneProviderOption providers[]{
        {"lux.render.runtime", "main-window"},
        {"lux.render.scene_bindings", "render-bindings"},
        {"lux.render.resources", "resources"},
        {"lux.render.assets", "assets"},
        {"lux.world.loading", "world-storage"}
    };
    editor::scene::SceneConfigurationResult<void> status;
    editor::scene::SceneConfigurationElement element(
        layout,
        ui::ElementId{"configuration"},
        {manager->catalog(),
         registrations->components,
         *registrations->simulation_systems,
         registrations->scene_systems,
         registrations->features,
         providers,
         [snapshot = contributions.snapshot()](
             ui::Element& parent,
             std::string_view name,
             std::uint32_t version,
             const serialization::PortableValueCodec&,
             std::optional<std::span<const std::byte>> initial
         ) -> editor::scene::SceneConfigurationResult<editor::scene::ConfigurationControl> {
             for (const auto& descriptor : snapshot.configurations())
                 if (descriptor.value.schema_name == name && descriptor.value.schema_version == version)
                     return editor::scene::makeConfigurationControl(descriptor, parent, ui::ElementId{name}, initial);
             return editor::scene::ConfigurationControl{};
         }, registrations->render_bindings},
        status
    );
    assert(status);
    for (const auto preset :
         {editor::scene::ESceneContentPreset::TWO_DIMENSIONAL, editor::scene::ESceneContentPreset::THREE_DIMENSIONAL})
    {
        auto applied = element.applyPreset(preset);
        if (!applied)
            std::fprintf(stderr, "%s: %s\n", applied.error().domain.c_str(), applied.error().message.c_str());
        assert(applied);
        auto captured_draft = element.capture();
        assert(captured_draft);
        const editor::scene::SceneConfigurationRegistrations inputs{
            registrations->components, *registrations->simulation_systems,
            registrations->scene_systems, registrations->features, providers, registrations->render_bindings
        };
        auto pure_draft = editor::scene::makeSceneConfigurationPreset(preset, "lux.spatial.builtin.single", 1, inputs);
        assert(pure_draft && captured_draft->schemas == pure_draft->schemas);
        for (auto* draft : {&*captured_draft, &*pure_draft})
            for (auto& row : draft->systems)
                std::ranges::sort(row.providers, {}, &editor::scene::SceneProviderBinding::requirement);
        for (std::size_t index{}; index < captured_draft->systems.size(); ++index)
        {
            const auto& shown = captured_draft->systems[index];
            const auto& prepared = pure_draft->systems[index];
            if (shown != prepared)
                std::fprintf(stderr, "configuration row %zu %s: name=%d bytes=%d providers=%d\n", index,
                    shown.type.name.c_str(), shown.name == prepared.name,
                    shown.configuration == prepared.configuration, shown.providers == prepared.providers);
        }
        assert(captured_draft->systems == pure_draft->systems);
        auto* field = coordinateField(element);
        assert(field);
        field->setValue(256.0);
        field->setValue(-1.0);
        assert(!element.build());
        field->setValue(256.0);
        for (const auto stage :
             {editor::scene::ESceneConfigurationStage::CONTENT,
              editor::scene::ESceneConfigurationStage::SIMULATION,
              editor::scene::ESceneConfigurationStage::SCENE,
              editor::scene::ESceneConfigurationStage::FEATURES,
              editor::scene::ESceneConfigurationStage::RELATIONSHIPS})
        {
            element.setStage(stage);
            ui::DrawData data;
            assert((*root)->update({{1100, 820}, 1.0F / 60}, &data));
        }
        if (preset == editor::scene::ESceneContentPreset::THREE_DIMENSIONAL)
        {
            const auto find_schema = [&](auto&& self, object::LuxObject& owner) -> ui::CheckBox* {
                if (auto* field = dynamic_cast<ui::CheckBox*>(&owner);
                    field && field->id().name() == "lux.ecs.Mesh3D")
                    return field;
                for (auto* child = owner.firstChild(); child; child = child->nextSibling())
                    if (auto* field = self(self, *child))
                        return field;
                return nullptr;
            };
            auto* mesh = find_schema(find_schema, element);
            assert(mesh && mesh->value());
            mesh->setValue(false);
            const auto denied = element.build();
            assert(!denied && denied.error().domain == "scene.feature.author-input");
            mesh->setValue(true);
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
        const auto expected = preset == editor::scene::ESceneContentPreset::TWO_DIMENSIONAL
                                  ? "lux.render.canvas2d.v2"
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
