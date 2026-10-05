#pragma once

#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/editor/project/RecentProjectsView.hpp>
#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/ui/Root.hpp>
#include <cassert>
#include <array>
#include <fstream>
#include <iostream>
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
    auto execution = take(process::ExecutionRuntime::create(
        {1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}}
    ));
    auto messages = take(object::ObjectMessageQueue::create(64));
    process::TaskScope project_tasks{execution};
    asset::AssetVfs vfs;
    auto prepared = take(prepareProjectOpen(directory / "Project.luxproject"));
    auto project = take(ProjectStorage::open(
        prepared, vfs, *execution.blocking(), project_tasks, messages.dispatcherRef()
    ));
    storage::FileArtifactStore files{directory};
    persistence::WriteCoordinator writes;
    persistence::SaveService saves{writes};
    persistence::SaveExecution publishing{execution, saves, writes, files};
    assets::ModelImporter importer{*project, execution, writes, files, publishing};
    RecentProjects recent{directory / "user", directory / "Project.luxproject", execution, writes, files, publishing};
    services::ServiceRegistry services{messages.dispatcherRef()};
    auto scope = take(services.createScope());
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.catalog"}, project->catalogModel()));
    assert(scope.provide(services::ServiceNameView{"lux.editor.assets.importer"}, importer));
    desktop::UiRegistry windows{messages.dispatcherRef(), services};
    const auto catalog = take(desktop::UiCatalog::prepare({
        desktop::UiEntry::bind<project::kImportView>(object::CodeLease::builtin()),
        desktop::UiEntry::bind<project::kRecentProjectsView>(object::CodeLease::builtin())
    }));
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
        static_cast<project::ImportView*>(import_pane), &project::ImportView::browseRequested,
        [&]() noexcept { ++browse_count; }
    ));
    std::optional<assets::ModelImportId> operation;
    auto request = [&](ui::Pane& pane)
    {
        auto& view = static_cast<project::ImportView&>(pane);
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
    importer.requestClose();
    assert(importer.closeStatus().state == assets::EModelImportCloseState::CLOSED);
    std::cout << "EC4 project tools: exact dependencies, atomic mount, Root ownership, "
                 "IO survives close, fresh identity\n";
}
