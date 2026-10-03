#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/storage/ProjectPlugins.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/TaskScope.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <fstream>
#include <thread>

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    assert(argc == 3);
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    const auto file = root / "Project.luxproject";
    // Actual runtime admission uses the same helper as the product; errors are not empty catalogs.
    const ProjectPluginEntry builtin{"lux.builtin.scene_render", 1, {}};
    auto runtime_plugins = loadProjectPlugins(root, std::span{&builtin, 1}, argv[2]);
    assert(runtime_plugins && runtime_plugins->find(builtin.id));
    const ProjectPluginEntry missing{"missing.plugin", 1, {}};
    assert(!loadProjectPlugins(root, std::span{&missing, 1}, argv[2]));
    const ProjectPluginEntry absent{"test", 1, "absent.json"};
    auto absent_result = loadProjectPlugins(root, std::span{&absent, 1}, {});
    assert(!absent_result && absent_result.error().code == lux::project::EPluginError::IO_FAILURE);
    const auto outside = root.parent_path() / "plugin-outside.json";
    {
        std::ofstream stream(outside);
        stream << "{}";
    }
    const ProjectPluginEntry escaped{"test", 1, "../plugin-outside.json"};
    auto escaped_result = loadProjectPlugins(root, std::span{&escaped, 1}, {});
    assert(!escaped_result && escaped_result.error().code == lux::project::EPluginError::INVALID_PATH);

    const auto id = asset::AssetId{*uuids::uuid::from_string("57279371-1b9c-40d6-b55d-a1a12065d932")};
    ProjectManifest initial{id, "Publication test", {}, {}};
    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        stream << *encodeProjectManifest(initial);
        assert(stream.good());
    }
    auto execution = process::ExecutionRuntime::create({1, 64, 64, {64}, process::BlockingSchedulerConfig{1, 64}});
    assert(execution);
    auto messages = object::ObjectMessageQueue::create(32);
    assert(messages);
    process::TaskScope tasks{*execution};
    auto source = prepareProjectOpen(file);
    assert(source);
    lux::asset::AssetVfs assets;
    auto project = ProjectStorage::open(*source, assets, *execution->blocking(), tasks, messages->dispatcherRef());
    assert(project);
    persistence::WriteCoordinator writes;
    persistence::SaveService saves{writes};
    storage::FileArtifactStore files{root};
    persistence::SaveExecution delivery{*execution, saves, writes, files};
    std::unique_ptr<ProjectPublicationOperation> operation;
    const auto start = [&](ProjectUpdate update) -> EditorResult<void> {
        if (operation)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "test.project.publication"});
        auto publication = (*project)->preparePublication(update);
        if (!publication)
            return cxx::unexpected(publication.error());
        operation = std::make_unique<ProjectPublicationOperation>(
            **project,
            *execution,
            writes,
            files,
            delivery,
            std::move(*publication)
        );
        return {};
    };
    const auto await = [&](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!ready())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(execution->collectCompletions());
            if (operation)
                operation->update();
            assert(delivery.submitReady());
            std::this_thread::yield();
        }
    };
    const std::vector<ProjectPluginEntry> selection{{"test.runtime", 3, "Plugins/Runtime.json"}};
    ProjectUpdate selected;
    selected.plugins = selection;
    assert(start(std::move(selected)));
    assert(!start({}));
    ProjectUpdate competitor;
    assert(!(*project)->preparePublication(competitor));
    await([&] { return std::holds_alternative<PublicationSucceeded>(operation->status()); });
    assert((*project)->manifest().plugins == selection);
    assert(operation->terminal());
    operation.reset();
    assert(!operation && writes.size() == 0);

    // The same coordinator publishes other project changes without replacing plugin selection.
    assert(start({}));
    await([&] { return operation->terminal(); });
    assert(std::holds_alternative<PublicationSucceeded>(operation->status()));
    assert((*project)->manifest().plugins == selection);
    operation.reset();

    // A conflict remains in its operation owner, independently of any pane or notification.
    {
        std::ofstream external(file, std::ios::binary | std::ios::app);
        external << "\n# external edit\n";
    }
    ProjectUpdate deselected;
    deselected.plugins.emplace();
    assert(start(std::move(deselected)));
    await([&] { return std::holds_alternative<EditorFailure>(operation->status()); });
    assert((*project)->manifest().plugins == selection);
    operation->abandon();
    await([&] { return std::holds_alternative<PublicationAbandoned>(operation->status()); });
    assert(operation->terminal());
    operation.reset();
    assert((*project)->manifest().plugins == selection && writes.size() == 0);

    (*project)->requestClose();
    await([&] {
        const auto closed = (*project)->advanceClose();
        assert(closed);
        return *closed;
    });
    project->reset();
    assert(tasks.join());
    execution->requestStop();
    assert(execution->join());
    std::filesystem::remove(file);
}
