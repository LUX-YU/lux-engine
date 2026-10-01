#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/TaskScope.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <fstream>
#include <thread>

namespace
{
}

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    assert(argc == 2);
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    const auto file = root / "Project.luxproject";
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
    auto source = readProjectOpenData(file);
    assert(source);
    lux::asset::AssetVfs assets;
    auto project = ProjectStorage::open(*source, assets, *execution->blocking(), tasks, messages->dispatcherRef());
    assert(project);
    const std::vector<ProjectPluginEntry> selection{{"test.runtime", 3, "Plugins/Runtime.json"}};
    assert((*project)->savePlugins(selection, *execution));
    assert(!(*project)->savePlugins({}, *execution));
    const auto await = [&](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!ready())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(execution->collectCompletions());
            std::this_thread::yield();
        }
    };
    await([&] { return std::holds_alternative<PublicationSucceeded>(*(*project)->pluginSaveStatus()); });
    assert((*project)->manifest().plugins == selection);
    assert((*project)->acknowledgePluginSave());
    assert(!(*project)->pluginSaveStatus());

    // Other publications preserve the selection unless an explicit replacement is supplied.
    ProjectUpdate asset_update;
    auto publication = (*project)->preparePublication(asset_update);
    assert(publication && publication->manifest.plugins == selection);
    auto receipt = publishProjectFiles(*publication);
    assert(receipt && (*project)->adoptPublication(*publication, *receipt));
    *publication = {};

    // A conflict is retained by Project independently of any pane or notification.
    {
        std::ofstream external(file, std::ios::binary | std::ios::app);
        external << "\n# external edit\n";
    }
    assert((*project)->savePlugins({}, *execution));
    await([&] { return std::holds_alternative<EditorFailure>(*(*project)->pluginSaveStatus()); });
    assert((*project)->manifest().plugins == selection);
    (*project)->abandonPluginSave();
    await([&] { return std::holds_alternative<PublicationAbandoned>(*(*project)->pluginSaveStatus()); });
    assert((*project)->acknowledgePluginSave());
    assert((*project)->manifest().plugins == selection);

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
