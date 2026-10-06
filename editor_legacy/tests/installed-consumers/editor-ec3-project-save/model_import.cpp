#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <lux/engine/editor/assets/ModelImporter.hpp>
#include <lux/engine/editor/persistence/PersistenceServices.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/PublicationFileStore.hpp>
#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <thread>

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    const auto take = [](auto result)
    {
        assert(result);
        return std::move(*result);
    };
    assert(argc == 2);
    const auto directory = std::filesystem::absolute(argv[1]);
    assert(directory.filename() == "model-import" && directory.has_parent_path());
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    std::filesystem::create_directories(directory / "user");
    std::filesystem::create_directories(directory / "installation");
    const asset::AssetId project_id{*uuids::uuid::from_string("902b311b-7d7a-420f-9068-ce074df64e4c")};
    const asset::AssetId model_id{*uuids::uuid::from_string("892b311b-7d7a-420f-9068-ce074df64e4c")};
    {
        std::ofstream manifest(directory / "Project.luxproject");
        manifest << take(encodeProjectManifest({project_id, "SDK model import", {}, {}}));
        std::ofstream model(directory / "triangle.obj");
        model << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    auto execution =
        take(process::ExecutionRuntime::create({2, 64, 64, {64}, process::BlockingSchedulerConfig{2, 32}}));
    auto messages = take(object::ObjectMessageQueue::create(64));
    process::TaskScope project_tasks{execution};
    asset::AssetVfs vfs;
    auto prepared = take(prepareProjectOpen(directory / "Project.luxproject"));
    auto project =
        take(ProjectStorage::open(prepared, vfs, *execution.blocking(), project_tasks, messages.dispatcherRef()));
    auto roots =
        std::make_shared<const storage::PublicationRoots>(directory, directory / "user", directory / "installation");
    services::ServiceRegistry registry{messages.dispatcherRef()};
    auto scope = take(registry.createScope());
    auto user_directory = directory / "user";
    const auto recent_path = user_directory / "lux/editor/recent-projects.toml";
    assert(scope.provide(services::ServiceNameView{"lux.editor.user-directory"}, user_directory));
    assert(scope.provide(services::ServiceNameView{"lux.process.execution"}, execution));
    assert(scope.provide(services::ServiceNameView{"lux.editor.publication.roots"}, roots));
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.storage"}, *project));
    assert(registry.publish(
        {services::ServiceEntry::bind<storage::kPublicationFileStoreService>(object::CodeLease::builtin()),
         services::ServiceEntry::bind<persistence::kWriteCoordinatorService>(object::CodeLease::builtin()),
         services::ServiceEntry::bind<persistence::kSaveService>(object::CodeLease::builtin()),
         services::ServiceEntry::bind<persistence::kSaveExecutionService>(object::CodeLease::builtin()),
         services::ServiceEntry::bind<assets::kModelImporterService>(object::CodeLease::builtin()),
         services::ServiceEntry::bind<kRecentProjectsService>(object::CodeLease::builtin())}
    ));
    // A fresh user directory need not already contain a profile or recent-project file.
    assert(!std::filesystem::exists(recent_path.parent_path()));
    auto recent = take(registry.get<RecentProjects>(scope));
    assert(recent == take(registry.get<RecentProjects>(scope)));
    assert(!std::filesystem::exists(recent_path) && recent->settled());
    assert(recent->update());
    std::weak_ptr<RecentProjects> recent_lifetime = recent;
    recent.reset();
    assert(!recent_lifetime.expired());
    const auto recent_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;)
    {
        assert(std::chrono::steady_clock::now() < recent_deadline);
        assert(execution.collectCompletions() && scope.maintain());
        auto retained = recent_lifetime.lock();
        assert(retained && retained->update());
        assert(!retained->failure());
        if (retained->settled())
        {
            assert(retained->publication());
            assert(std::holds_alternative<persistence::CommitReceipt>(*retained->publication()));
            assert(retained->entries().size() == 1 && retained->entries().front() == project->projectFile());
            break;
        }
        std::this_thread::yield();
    }
    assert(std::filesystem::exists(recent_path));
    recent = take(registry.get<RecentProjects>(scope));
    assert(recent == recent_lifetime.lock());
    assert(recent->refresh() && recent->update(false));
    assert(recent->settled() && !recent->ticket());
    assert(std::holds_alternative<persistence::CommitReceipt>(*recent->publication()));
    recent.reset();
    auto importer = take(registry.get<assets::ModelImporter>(scope));
    assert(importer == take(registry.get<assets::ModelImporter>(scope)));
    assert(!importer->currentRequest());
    const auto id = take(importer->requestModel({model_id, directory / "triangle.obj", "Beginner/Triangle", {}}));
    auto duplicate = importer->requestModel({model_id, directory / "triangle.obj", "Beginner/Triangle", {}});
    assert(!duplicate && duplicate.error().code == EEditorError::BUSY);
    std::weak_ptr<assets::ModelImporter> lifetime = importer;
    importer.reset();
    assert(!lifetime.expired()); // No presenter/explicit caller is required for accepted work.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        assert(execution.collectCompletions());
        assert(scope.maintain());
        const auto owner = lifetime.lock();
        assert(owner);
        const auto status = take(owner->status(id));
        if (const auto* failure = std::get_if<EditorFailure>(&status))
        {
            std::cerr << failure->domain << ':' << failure->message << '\n';
            assert(false && "Real SDK import/publication failed");
        }
        if (std::holds_alternative<assets::ModelImportSucceeded>(status))
        {
            break;
        }
        std::this_thread::yield();
    }
    importer = take(registry.get<assets::ModelImporter>(scope));
    assert(importer == lifetime.lock());
    assert(std::holds_alternative<assets::ModelImportSucceeded>(take(importer->status(id))));
    const auto* asset = project->asset(model_id);
    assert(asset && std::filesystem::exists(directory / asset->source_path));
    assert(std::filesystem::exists(directory / asset->cooked_path));
    assert(importer->acknowledge(id));
    auto stale = importer->status(id);
    assert(!stale && stale.error().code == EEditorError::STALE_REQUEST);
    importer->requestClose();
    assert(importer->closeStatus().state == assets::EModelImportCloseState::CLOSED);
    auto closed = importer->reimportModel(model_id);
    assert(!closed && closed.error().code == EEditorError::CLOSING);
    importer.reset();
    assert(take(scope.settled()));
    assert(scope.release());
    while (!scope.drained())
    {
        assert(messages.collectRetired() > 0);
    }
    assert(lifetime.expired() && recent_lifetime.expired() && registry.drained());
    std::cout << "SDK recent projects: lazy shared owner, absent profile, actual IO, close permission, retirement\n";
    project->requestClose();
    assert(take(project->advanceClose()));
    std::cout
        << "SDK import: one scoped owner/coordinator, no UI, retained actual cooked result, close and retirement\n";
}
