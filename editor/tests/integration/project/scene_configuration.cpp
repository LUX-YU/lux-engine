#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
#include <lux/engine/editor/scene/ConfigurationEditor.hpp>
#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/scene/SceneCreationView.hpp>
#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/sessions/SessionServices.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>

namespace
{
    lux::ui::NumericEdit* coordinateField(lux::object::LuxObject& object)
    {
        if (auto* field = dynamic_cast<lux::ui::NumericEdit*>(&object))
        {
            if (field->id().name() == "Coordinate page size")
            {
                return field;
            }
        }
        for (auto* child = object.firstChild(); child; child = child->nextSibling())
        {
            if (auto* found = coordinateField(*child))
            {
                return found;
            }
        }
        return nullptr;
    }
} // namespace

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
            auto append = [](auto& to, auto& from)
            { to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end())); };
            append(draft.code, supplied->code);
            append(draft.reflection, supplied->reflection);
            append(draft.services, supplied->services);
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
    auto prepared_inputs = editor::scene::makeSceneConfigurationInputs(
        manager->catalog(),
        {registrations->components, *registrations->simulation_systems, registrations->scene_systems,
         registrations->features, providers, registrations->render_bindings},
        contributions.snapshot().services()
    );
    assert(prepared_inputs);
    auto form_inputs = std::move(*prepared_inputs);
    // Replacement affects only later forms. Existing controls keep their captured immutable definitions,
    // including real extension code; no callback reads the mutable contribution table again.
    {
        auto original = contributions.snapshot();
        auto definitions = editor::scene::sceneEditorDefinitions(original.services());
        assert(definitions && !definitions->empty() && !definitions->front()->configurations.empty());
        const auto registration = definitions->front()->configurations.front();
        auto empty = editor::extensions::ContributionSnapshot::prepare({});
        assert(empty && contributions.enqueue(*empty) && contributions.applyPending());
        auto next_inputs = editor::scene::makeSceneConfigurationInputs(
            manager->catalog(), form_inputs.registrations(), contributions.snapshot().services()
        );
        assert(next_inputs);
        auto old_control = form_inputs.configuration(
            layout, registration.value.schema_name, registration.value.schema_version, registration.value.codec, {}
        );
        auto new_control = next_inputs->configuration(
            layout, registration.value.schema_name, registration.value.schema_version, registration.value.codec, {}
        );
        assert(old_control && bool(*old_control) && new_control && !bool(*new_control));
        std::vector<std::byte> frozen;
        assert(old_control->encode(frozen));
        const std::shared_ptr<const services::ServiceEntry> invalid[]{{}};
        auto rejected = editor::scene::makeSceneConfigurationInputs(
            manager->catalog(), form_inputs.registrations(), invalid
        );
        assert(!rejected && rejected.error().code == editor::scene::ESceneConfigurationError::CONTROL_FAILURE);
        assert(contributions.enqueue(original) && contributions.applyPending());
    }
    editor::scene::SceneConfigurationElement element(layout, ui::ElementId{"configuration"}, form_inputs, status);
    assert(status);
    for (const auto preset :
         {editor::scene::ESceneContentPreset::TWO_DIMENSIONAL, editor::scene::ESceneContentPreset::THREE_DIMENSIONAL})
    {
        auto applied = element.applyPreset(preset);
        if (!applied)
        {
            std::fprintf(stderr, "%s: %s\n", applied.error().domain.c_str(), applied.error().message.c_str());
        }
        assert(applied);
        auto captured_draft = element.capture();
        assert(captured_draft);
        const editor::scene::SceneConfigurationRegistrations inputs{
            registrations->components,
            *registrations->simulation_systems,
            registrations->scene_systems,
            registrations->features,
            providers,
            registrations->render_bindings
        };
        auto pure_draft = editor::scene::makeSceneConfigurationPreset(preset, "lux.spatial.builtin.single", 1, inputs);
        assert(pure_draft && captured_draft->schemas == pure_draft->schemas);
        for (auto* draft : {&*captured_draft, &*pure_draft})
        {
            for (auto& row : draft->systems)
            {
                std::ranges::sort(row.providers, {}, &editor::scene::SceneProviderBinding::requirement);
            }
        }
        for (std::size_t index{}; index < captured_draft->systems.size(); ++index)
        {
            const auto& shown = captured_draft->systems[index];
            const auto& prepared = pure_draft->systems[index];
            if (shown != prepared)
            {
                std::fprintf(
                    stderr,
                    "configuration row %zu %s: name=%d bytes=%d providers=%d\n",
                    index,
                    shown.type.name.c_str(),
                    shown.name == prepared.name,
                    shown.configuration == prepared.configuration,
                    shown.providers == prepared.providers
                );
            }
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
            const auto find_schema = [&](auto&& self, object::LuxObject& owner) -> ui::CheckBox*
            {
                if (auto* field = dynamic_cast<ui::CheckBox*>(&owner); field && field->id().name() == "lux.ecs.Mesh3D")
                {
                    return field;
                }
                for (auto* child = owner.firstChild(); child; child = child->nextSibling())
                {
                    if (auto* field = self(self, *child))
                    {
                        return field;
                    }
                }
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
        const auto feature = std::ranges::find_if(
            registrations->features,
            [&](const auto& value) { return value.factory.descriptor.canonical_name == expected; }
        );
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
    // The actual declared factory creates a complete off-tree form. No Host or Application is needed.
    // Root owns only the mounted window; SessionStore independently owns both newly created scenes.
    {
        namespace author = editor::scene;
        namespace sessions = editor::sessions;
        namespace desktop = editor::desktop;
        auto& scope = editor_context.scope();
        auto entries = contributions.snapshot().services();
        std::vector<std::shared_ptr<const services::ServiceEntry>> services(entries.begin(), entries.end());
        services.push_back(services::ServiceEntry::bind<sessions::kSessionStoreService>(object::CodeLease::builtin()));
        assert(editor_context.services().publish(std::move(services)));
        auto store_owner = editor_context.services().get<sessions::SessionStore>(scope);
        assert(store_owner);
        auto& store = **store_owner;
        editor::persistence::WriteCoordinator writes;
        editor::persistence::SaveService saves{writes};
        std::vector<sessions::InstalledSession> installed;
        unsigned requests{};
        sessions::SessionCreation receive = [&](sessions::SessionPreparation input
                                            ) -> editor::commands::CommandResult<editor::commands::DispatchReceipt>
        {
            ++requests;
            if (requests == 1)
            {
                return cxx::unexpected(
                    editor::commands::CommandFailure{editor::commands::ECommandError::BUSY, "scene.test.creation"}
                );
            }
            auto prepared = std::move(input).prepare(store, saves);
            assert(prepared);
            auto published = prepared->publish();
            assert(published);
            installed.push_back(std::move(*published));
            return editor::commands::DispatchReceipt{editor::commands::ImmediateCompletion{}};
        };
        assert(scope.provide(services::ServiceNameView{"lux.project.plugins"}, *manager));
        assert(scope.provide(services::ServiceNameView{"lux.simulation.components"}, registrations->components));
        assert(scope.provide(services::ServiceNameView{"lux.simulation.systems"}, registrations->simulation_systems));
        assert(scope.provide(services::ServiceNameView{"lux.scene.systems"}, registrations->scene_systems));
        assert(scope.provide(services::ServiceNameView{"lux.render.features"}, registrations->features));
        assert(scope.provide(services::ServiceNameView{"lux.render.scene.bindings"}, registrations->render_bindings));
        assert(scope.provide(sessions::kSessionCreation, receive));
        auto catalog = desktop::UiCatalog::prepare(
            {desktop::UiEntry::bind<author::kSceneCreationView>(object::CodeLease::builtin()),
             desktop::UiEntry::bind<author::kSceneConfigurationView>(object::CodeLease::builtin())}
        );
        assert(catalog && editor_context.ui().publish(*catalog));
        auto factory = catalog->at(0);
        assert(factory);
        desktop::UiCreateInfo input{queue->dispatcherRef(), ui::PaneId{"declared-creation"}};
        input.configuration.bytes = {std::byte{1}};
        const auto invalid = editor_context.ui().create(*factory, scope, input);
        assert(!invalid && invalid.error().code == desktop::EUiError::INVALID_CONFIGURATION);
        assert(requests == 0 && store.size() == 0);
        input.configuration = {};
        auto owner = editor_context.ui().create(*factory, scope, input);
        assert(owner && !(*owner)->attachedRoot() && requests == 0 && store.size() == 0);
        auto* form = static_cast<author::SceneCreationView*>(owner->get());
        assert((*root)->addSubPane(std::move(*owner)));
        const auto handle = (*root)->identify(*form);
        assert(handle);
        for (auto preset :
             {author::ESceneContentPreset::TWO_DIMENSIONAL, author::ESceneContentPreset::THREE_DIMENSIONAL})
        {
            assert(form->configuration().applyPreset(preset));
            auto expected = form->configuration().build();
            assert(expected);
            const auto before = installed.size();
            form->requestCreate();
            assert((*root)->update({{1100, 820}, 1.0F / 60}, nullptr));
            if (requests == 1)
            {
                assert(installed.empty() && !form->status());
                assert(form->status().error().code == author::ESceneConfigurationError::BUSY);
                assert((*root)->update({{1100, 820}, 1.0F / 60}, nullptr));
            }
            assert(form->status() && installed.size() == before + 1);
            const auto info = store.describe(installed.back().id());
            assert(info && info->dirty && !info->binding);
            const auto key = store.key<author::SceneSession>(installed.back().id());
            assert(key);
            auto model = store.access<author::SceneSession>().read(*key);
            assert(model);
            auto snapshot = model->get().capture();
            assert(snapshot && snapshot->content() == info->current && snapshot->objects().empty());
            assert(snapshot->configuration().simulation->data().systemCount() == expected->simulation->systemCount());
            assert(snapshot->configuration().scene->data().systemCount() == expected->scene.systemCount());
            assert(snapshot->configuration().world->data().partitioner().id.name == "lux.spatial.builtin.single");
        }
        assert(requests == 3 && store.size() == 2);
        {
            const auto id = installed.front().id();
            const auto prior = store.describe(id);
            assert(prior);
            auto configuration_factory = catalog->find(author::kSceneConfigurationView.type);
            assert(configuration_factory);
            input.instance = ui::PaneId{"declared-configuration"};
            input.content = {{id}, id};
            auto candidate = editor_context.ui().create(*configuration_factory, scope, input);
            assert(candidate && !(*candidate)->attachedRoot());
            auto* configured = static_cast<author::SceneConfigurationView*>(candidate->get());
            assert(configured->status() && configured->form() && configured->content() == input.content);
            assert((*root)->addSubPane(std::move(*candidate)));
            const auto target = (*root)->identify(*configured);
            assert(target);
            auto access = store.access<author::SceneSession>();
            auto key = access.key(id);
            assert(key);
            auto model = access.read(*key);
            assert(model);
            auto read = model->get().read();
            assert(read && read->withRead([&](const author::SceneReadView&) -> author::SceneEditResult<void>
            {
                auto busy = editor_context.ui().prepareClose(**root, std::span{&*target, 1});
                assert(!busy && busy.error().code == desktop::EUiError::BUSY);
                assert((*root)->findPane(*target) && configured->form());
                return {};
            }));
            auto* original_form = configured->form();
            {
                auto abandoned = editor_context.ui().prepareClose(**root, std::span{&*target, 1});
                assert(abandoned && configured->form() == original_form && (*root)->findPane(*target));
            }
            assert((*root)->update({{1100, 820}, 1.0F / 60}, nullptr));
            assert(configured->status() && configured->form() && configured->content() == input.content);
            auto permit = editor_context.ui().prepareClose(**root, std::span{&*target, 1});
            assert(permit && (*root)->commit(*permit));
            static_cast<void>(queue->collectRetired());
            auto after = store.describe(id);
            assert(after && after->current == prior->current && after->dirty == prior->dirty);
        }

        auto close = editor_context.ui().prepareClose(**root, std::span{&*handle, 1});
        assert(close && (*root)->commit(*close));
        assert((*root)->update({{1100, 820}, 1.0F / 60}, nullptr));
        assert(!(*root)->findPane(*handle) && store.size() == 2);
        for (auto& session : installed)
        {
            auto info = store.describe(session.id());
            assert(info && session.close(info->current));
        }
        assert(store.size() == 0);
        installed.clear();
        store_owner->reset();
        assert(scope.release());
        for (int batch{}; batch != 16 && !scope.drained(); ++batch)
        {
            static_cast<void>(queue->collectRetired());
        }
        assert(scope.drained());
    }
}
