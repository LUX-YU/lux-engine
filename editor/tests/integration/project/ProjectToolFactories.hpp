#pragma once

#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/editor/project/ProjectCreationView.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/project/RecentProjectsView.hpp>
#include <lux/engine/editor/project/ResultsView.hpp>
#include <lux/engine/editor/project/SettingsView.hpp>
#include <lux/engine/editor/project/WorkspaceView.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

inline void projectToolFactories(const std::filesystem::path& artifacts)
{
    using namespace lux;
    using namespace lux::editor;
    const auto take = [](auto result)
    {
        assert(result);
        return std::move(*result);
    };
    const auto directory = std::filesystem::absolute(artifacts) /
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(directory);
    const asset::AssetId project_id{*uuids::uuid::from_string("67b2a3c4-17d3-4771-a278-dd95bb084c21")};
    const asset::AssetId asset_id{*uuids::uuid::from_string("07948e66-dcad-417c-aa62-8f91256372e2")};
    {
        std::ofstream manifest(directory / "Project.luxproject");
        manifest << take(encodeProjectManifest({project_id, "EC4 UI factories", {}, {}}));
        std::ofstream source(directory / "triangle.obj");
        source << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    auto execution =
        take(process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}}));
    auto messages = take(object::ObjectMessageQueue::create(64));
    process::TaskScope project_tasks{execution};
    asset::AssetVfs vfs;
    auto prepared = take(prepareProjectOpen(directory / "Project.luxproject"));
    auto project =
        take(ProjectStorage::open(prepared, vfs, *execution.blocking(), project_tasks, messages.dispatcherRef()));
    storage::FileArtifactStore files{directory};
    persistence::WriteCoordinator writes;
    persistence::SaveService saves{writes};
    persistence::SaveExecution publishing{execution, saves, writes, files};
    assets::ModelImporter importer{*project, execution, writes, files, publishing};
    RecentProjects recent{directory / "user", directory / "Project.luxproject", execution, writes, files, publishing};
    auto plugins = take(lux::project::PluginManager::create({}, {}));
    lux::editor::project::PluginSelectionRequests plugin_requests;
    unsigned plugin_requests_received{};
    lux::editor::project::ResultsView::Observe results_observe;
    lux::editor::project::ResultsView::Request results_request;
    lux::editor::project::WorkspaceView::Observe workspace_observe;
    lux::editor::project::WorkspaceView::Request workspace_request;
    cxx::move_only_function<lux::editor::project::ProjectCreationRequests()> creation_requests;
    services::ServiceRegistry services{messages.dispatcherRef()};
    auto scope = take(services.createScope());
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.catalog"}, project->catalogModel()));
    assert(scope.provide(services::ServiceNameView{"lux.editor.assets.importer"}, importer));
    desktop::UiRegistry windows{messages.dispatcherRef(), services};
    const auto catalog = take(desktop::UiCatalog::prepare(
        {desktop::UiEntry::bind<lux::editor::project::kProjectView>(object::CodeLease::builtin()),
         desktop::UiEntry::bind<lux::editor::project::kImportView>(object::CodeLease::builtin()),
         desktop::UiEntry::bind<lux::editor::project::kRecentProjectsView>(object::CodeLease::builtin()),
         desktop::UiEntry::bind<lux::editor::project::kSettingsView>(object::CodeLease::builtin()),
         desktop::UiEntry::bind<lux::editor::project::kResultsView>(object::CodeLease::builtin()),
         desktop::UiEntry::bind<lux::editor::project::kWorkspaceView>(object::CodeLease::builtin()),
         desktop::UiEntry::bind<lux::editor::project::kProjectCreationView>(object::CodeLease::builtin())}
    ));
    assert(windows.publish(catalog));
    auto root = take(ui::Root::create(messages.dispatcherRef()));
    const auto import_factory = take(catalog.find(views::ViewTypeIdView{"lux.editor.import"}));
    const auto recent_factory = take(catalog.find(views::ViewTypeIdView{"lux.editor.recent-projects"}));
    const desktop::UiCreateInfo import_input{messages.dispatcherRef(), ui::PaneId{"import"}, {}, {}};
    const desktop::UiCreateInfo recent_input{messages.dispatcherRef(), ui::PaneId{"recent"}, {}, {}};
    auto invalid = import_input;
    invalid.configuration.bytes.push_back(std::byte{1});
    auto invalid_window = windows.create(import_factory, scope, invalid);
    assert(!invalid_window && invalid_window.error().code == desktop::EUiError::INVALID_CONFIGURATION);
    const auto revision = root->windowRevision();
    auto rejected = windows.mount(*root, scope, {{import_factory, import_input}, {recent_factory, recent_input}});
    assert(!rejected && rejected.error().code == desktop::EUiError::DEPENDENCY);
    assert(root->panes().empty() && root->windowRevision() == revision && !importer.currentRequest());
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.recent"}, recent));
    assert(windows.mount(*root, scope, {{import_factory, import_input}, {recent_factory, recent_input}}));
    auto information = take(windows.describe(*root));
    assert(information.size() == 2);
    auto* import_pane = root->findPane(ui::PaneIdView{"import"});
    auto* recent_pane = root->findPane(ui::PaneIdView{"recent"});
    assert(import_pane && recent_pane);
    assert(import_pane->ownership() == object::EObjectOwnership::PARENT_OWNED);
    assert(recent_pane->ownership() == object::EObjectOwnership::PARENT_OWNED);
    const auto import_handle = take(root->identify(*import_pane));
    const auto recent_handle = take(root->identify(*recent_pane));
    std::size_t browse_count{};
    auto connection = take(object::LuxObject::connect(
        static_cast<lux::editor::project::ImportView*>(import_pane),
        &lux::editor::project::ImportView::browseRequested,
        [&]() noexcept { ++browse_count; }
    ));
    std::optional<assets::ModelImportId> operation;
    auto request = [&](ui::Pane& pane)
    {
        auto& view = static_cast<lux::editor::project::ImportView&>(pane);
        operation = take(view.importModel({asset_id, directory / "triangle.obj", "Content/Triangle", {}}));
    };
    assert(connection.connected() && root->withPane(import_handle, request) && operation);
    const std::array handles{import_handle, recent_handle};
    auto close = take(windows.prepareClose(*root, handles));
    assert(root->commit(close));
    assert(!root->findPane(import_handle) && !root->findPane(recent_handle));
    assert(messages.collectRetired() >= 2);
    assert(!connection.connected() && browse_count == 0);
    assert(importer.currentRequest() == operation); // Closing both windows leaves the accepted IO owner intact.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        assert(execution.collectCompletions());
        importer.update();
        saves.adoptCompletions();
        assert(publishing.submitReady());
        const auto status = take(importer.status(*operation));
        if (const auto* failure = std::get_if<EditorFailure>(&status))
        {
            std::cerr << failure->domain << ": " << failure->message << '\n';
            assert(false && "Accepted import must complete after window teardown");
        }
        if (std::holds_alternative<assets::ModelImportSucceeded>(status))
        {
            break;
        }
        std::this_thread::yield();
    }
    const auto* published = project->asset(asset_id);
    assert(published && std::filesystem::exists(directory / published->cooked_path));
    assert(importer.acknowledge(*operation));
    assert(windows.mount(*root, scope, {{import_factory, import_input}}));
    auto* replacement = root->findPane(ui::PaneIdView{"import"});
    const auto replacement_handle = take(root->identify(*replacement));
    assert(replacement_handle != import_handle && !root->findPane(import_handle));
    auto final_close = take(windows.prepareClose(*root, std::span{&replacement_handle, 1}));
    assert(root->commit(final_close));
    assert(messages.collectRetired() >= 1);
    // Factory-time connections are owned by their actual window and already active off-tree.
    // Only a mounted import may issue a native request, which retains Root's original generation.
    const auto project_factory = take(catalog.find(views::ViewTypeIdView{"lux.editor.project"}));
    const desktop::UiCreateInfo project_input{messages.dispatcherRef(), ui::PaneId{"assets"}, {}, {}};
    unsigned asset_opens{}, recent_opens{}, browse_requests{};
    std::optional<ui::PaneHandle> browse_target;
    lux::editor::project::ProjectView::Open asset_open;
    lux::editor::project::ImportView::Browse browse = [&](ui::PaneHandle id) noexcept
    {
        ++browse_requests;
        browse_target = id;
    };
    lux::editor::project::RecentProjectsView::Open recent_open = [&](const std::filesystem::path& path
                                                                 ) -> EditorResult<void>
    {
        ++recent_opens;
        assert(path == directory / "Project.luxproject");
        return cxx::unexpected(EditorFailure{EEditorError::BUSY, "actual.project.owner"});
    };
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.open"}, asset_open));
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.import.browse"}, browse));
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.recent.open"}, recent_open));
    const auto unchanged_revision = root->windowRevision();
    auto missing_open = windows.mount(*root, scope, {{import_factory, import_input}, {project_factory, project_input}});
    assert(!missing_open && missing_open.error().code == desktop::EUiError::INVALID_CONFIGURATION);
    assert(root->windowRevision() == unchanged_revision && take(windows.describe(*root)).empty());
    asset_open = [&](const AssetReference& reference) noexcept
    {
        ++asset_opens;
        const auto expected = project->catalogModel().reference(asset_id);
        assert(reference.project_instance == expected.project_instance);
        assert(reference.asset == expected.asset);
        assert(reference.catalog_revision == expected.catalog_revision);
    };
    auto browse_candidate = take(windows.create(import_factory, scope, import_input));
    auto* browse_view = static_cast<lux::editor::project::ImportView*>(browse_candidate.get());
    auto detached_browse = browse_view->requestBrowse();
    assert(!detached_browse && detached_browse.error().code == EEditorError::INVALID_STATE);
    assert(browse_requests == 0); // No identity is invented for an off-tree window.
    assert(root->addSubPane(std::move(browse_candidate)));
    const auto first_browse = take(root->identify(*browse_view));
    assert(browse_view->requestBrowse());
    assert(browse_requests == 1 && browse_target == first_browse);
    assert(windows.mount(*root, scope, {{project_factory, project_input}, {recent_factory, recent_input}}));
    auto* assets_view = static_cast<lux::editor::project::ProjectView*>(root->findPane(ui::PaneIdView{"assets"}));
    assert(assets_view && asset_opens == 0 && recent_opens == 0);
    assert(assets_view->requestOpen(project->catalogModel().reference(asset_id)) && asset_opens == 1);
    auto* recent_view =
        static_cast<lux::editor::project::RecentProjectsView*>(root->findPane(ui::PaneIdView{"recent"}));
    assert(recent_view->requestOpen(directory / "Project.luxproject"));
    assert(recent_opens == 1);
    std::vector<ui::PaneHandle> wired;
    for (const auto& info : take(windows.describe(*root)))
    {
        wired.push_back(info.handle);
    }
    auto detach_wired = take(windows.prepareClose(*root, wired));
    assert(root->commit(detach_wired));
    assert(messages.collectRetired() >= 3);
    assert(!root->findPane(*browse_target));
    assert(windows.mount(*root, scope, {{import_factory, import_input}}));
    auto* new_browse_view = static_cast<lux::editor::project::ImportView*>(root->findPane(ui::PaneIdView{"import"}));
    const auto second_browse = take(root->identify(*new_browse_view));
    assert(second_browse != first_browse && !root->findPane(first_browse));
    assert(new_browse_view->requestBrowse());
    assert(browse_requests == 2 && browse_target == second_browse);
    auto detach_new = take(windows.prepareClose(*root, std::span{&second_browse, 1}));
    assert(root->commit(detach_new));
    assert(messages.collectRetired() >= 1);
    // The remaining real project windows construct through the same registry without a Host.
    // Exact callback providers remain explicit borrowed inputs; they are not services with fake shared owners.
    const auto settings_factory = take(catalog.find(views::ViewTypeIdView{"lux.editor.settings"}));
    const auto results_factory = take(catalog.find(views::ViewTypeIdView{"lux.editor.content.results"}));
    const auto workspace_factory = take(catalog.find(views::ViewTypeIdView{"lux.editor.workspace"}));
    const auto creation_factory = take(catalog.find(views::ViewTypeIdView{"lux.editor.project.creation"}));
    const desktop::UiCreateInfo settings_input{messages.dispatcherRef(), ui::PaneId{"settings"}, {}, {}};
    const desktop::UiCreateInfo results_input{messages.dispatcherRef(), ui::PaneId{"results"}, {}, {}};
    const desktop::UiCreateInfo workspace_input{messages.dispatcherRef(), ui::PaneId{"workspace"}, {}, {}};
    const desktop::UiCreateInfo creation_input{messages.dispatcherRef(), ui::PaneId{"creation"}, {}, {}};
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.storage"}, *project));
    auto missing_plugins = windows.create(settings_factory, scope, settings_input);
    assert(!missing_plugins && missing_plugins.error().code == desktop::EUiError::DEPENDENCY);
    assert(scope.provide(services::ServiceNameView{"lux.project.plugins"}, plugins));
    assert(scope.provide(services::ServiceNameView{"lux.editor.results.observe"}, results_observe));
    assert(scope.provide(services::ServiceNameView{"lux.editor.results.request"}, results_request));
    auto missing_callbacks = windows.create(results_factory, scope, results_input);
    assert(!missing_callbacks && missing_callbacks.error().code == desktop::EUiError::INVALID_CONFIGURATION);
    unsigned observed{}, requested{};
    bool busy{};
    results_observe = [&]() -> EditorResult<lux::editor::project::ResultsSnapshot>
    {
        ++observed;
        if (busy)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "actual.owner.busy"});
        }
        return lux::editor::project::ResultsSnapshot{{{"Owner", {{"fact", {"Preserved result"}, {}}}}}};
    };
    results_request = [&](lux::editor::project::VResultIntent) -> EditorResult<void>
    {
        ++requested;
        return {};
    };
    workspace_observe = []() -> EditorResult<lux::editor::project::WorkspaceSnapshot>
    { return lux::editor::project::WorkspaceSnapshot{}; };
    workspace_request = [](lux::editor::project::VWorkspaceIntent) -> EditorResult<void> { return {}; };
    assert(scope.provide(services::ServiceNameView{"lux.editor.workspace.observe"}, workspace_observe));
    assert(scope.provide(services::ServiceNameView{"lux.editor.workspace.request"}, workspace_request));
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.creation.requests"}, creation_requests));
    auto missing_creation = windows.create(creation_factory, scope, creation_input);
    assert(!missing_creation && missing_creation.error().code == desktop::EUiError::INVALID_CONFIGURATION);
    creation_requests = [] { return lux::editor::project::ProjectCreationRequests{}; };
    auto incomplete_creation = windows.create(creation_factory, scope, creation_input);
    assert(!incomplete_creation && incomplete_creation.error().code == desktop::EUiError::INVALID_CONFIGURATION);
    unsigned creation_actions{};
    lux::editor::project::ProjectCreationProgress progress;
    creation_requests = [&]
    {
        return lux::editor::project::ProjectCreationRequests{
            [&] { return &plugins.catalog(); },
            [&]() -> const auto& { return progress; },
            [&](std::vector<ProjectPluginEntry>) -> EditorResult<void>
            {
                ++creation_actions;
                return {};
            },
            []() -> EditorResult<lux::editor::project::ProjectCreationConfiguration>
            { return cxx::unexpected(EditorFailure{EEditorError::BUSY, "not.configured"}); },
            [&](lux::editor::project::ProjectCreationDraft) -> EditorResult<void>
            {
                ++creation_actions;
                return {};
            },
            [&]() -> EditorResult<void>
            {
                ++creation_actions;
                return {};
            },
            [&] { ++creation_actions; },
            [&]() -> EditorResult<void>
            {
                ++creation_actions;
                return {};
            }
        };
    };
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.plugins.requests"}, plugin_requests));
    auto incomplete_plugins = windows.create(settings_factory, scope, settings_input);
    assert(!incomplete_plugins && incomplete_plugins.error().code == desktop::EUiError::INVALID_CONFIGURATION);
    assert(take(windows.describe(*root)).empty() && plugin_requests_received == 0);
    plugin_requests.save = [&](const lux::editor::project::PluginSelectionDraft& value) noexcept
    {
        ++plugin_requests_received;
        assert(value.based_on == project->manifest().plugins && value.desired.empty());
    };
    auto still_incomplete = windows.create(settings_factory, scope, settings_input);
    assert(!still_incomplete && still_incomplete.error().code == desktop::EUiError::INVALID_CONFIGURATION);
    plugin_requests.retry = [&]() noexcept { plugin_requests_received += 10; };
    plugin_requests.abandon = [&]() noexcept { plugin_requests_received += 100; };
    plugin_requests.acknowledge = [&]() noexcept { plugin_requests_received += 1000; };
    auto settings_pane = take(windows.create(settings_factory, scope, settings_input));
    assert(!settings_pane->attachedRoot() && settings_pane->content());
    unsigned selection_requests{};
    auto settings_connection = take(object::LuxObject::connect(
        static_cast<lux::editor::project::SettingsView*>(settings_pane.get()),
        &lux::editor::project::SettingsView::selectionRequested,
        [&](const lux::editor::project::PluginSelectionDraft& value) noexcept
        {
            ++selection_requests;
            assert(value.based_on == project->manifest().plugins && value.desired.empty());
        }
    ));
    assert(root->addSubPane(std::move(settings_pane)));
    assert(windows.mount(
        *root,
        scope,
        {{results_factory, results_input}, {workspace_factory, workspace_input}, {creation_factory, creation_input}}
    ));
    assert(observed == 0 && requested == 0 && creation_actions == 0);
    auto all = take(windows.describe(*root));
    assert(all.size() == 4);
    std::vector<ui::PaneHandle> tool_handles;
    for (const auto& info : all)
    {
        tool_handles.push_back(info.handle);
    }
    auto* actual_results = static_cast<lux::editor::project::ResultsView*>(root->findPane(ui::PaneIdView{"results"}));
    auto* actual_settings =
        static_cast<lux::editor::project::SettingsView*>(root->findPane(ui::PaneIdView{"settings"}));
    assert(actual_results && actual_settings);
    assert(plugin_requests_received == 0);
    assert(actual_settings->requestSave({}) && selection_requests == 1 && plugin_requests_received == 1);
    assert(root->update({}, nullptr));
    assert(observed == 1 && !actual_results->snapshot().sections.empty());
    busy = true;
    assert(root->update({}, nullptr));
    assert(actual_results->observationFailure()->code == EEditorError::BUSY);
    assert(actual_results->snapshot().sections.front().rows.front().messages.front() == "Preserved result");
    assert(actual_results->request(lux::editor::project::AcknowledgeMaintenance{}) && requested == 1);
    auto close_tools = take(windows.prepareClose(*root, tool_handles));
    assert(root->commit(close_tools));
    assert(messages.collectRetired() >= 4);
    assert(!settings_connection.connected() && plugin_requests_received == 1);
    const auto old_observed = observed;
    assert(root->update({}, nullptr));
    assert(observed == old_observed && creation_actions == 0);
    importer.requestClose();
    assert(importer.closeStatus().state == assets::EModelImportCloseState::CLOSED);
    std::cout << "EC4 project tools: exact dependencies, atomic mount, Root ownership, "
                 "IO survives close, fresh identity\n";
}
