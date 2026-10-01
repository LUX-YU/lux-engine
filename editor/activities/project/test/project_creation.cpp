#include <lux/engine/editor/storage/ProjectCreation.hpp>
#include <lux/engine/editor/storage/ProjectOpenData.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/editor/project/ProjectBuilder.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <cassert>
#include <chrono>
#include <fstream>
#include <thread>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    asset::AssetId id(std::uint8_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return asset::AssetId{bytes};
    }

}

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]) /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    auto execution = process::ExecutionRuntime::create({1, 32, 32, {16}, process::BlockingSchedulerConfig{1, 32}});
    assert(execution);
    const auto await = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!condition())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(execution->collectCompletions());
            assert(execution->dispatchTaskEvents());
            std::this_thread::yield();
        }
    };
    const auto create = [&](const std::filesystem::path& directory,
                            ProjectBuildConfig config,
                            std::optional<EditorResult<ProjectCreationResult>>& output,
                            std::stop_token stop = {}) {
        return execution->submit(
            {"Create project", "test"},
            [&, directory, config = std::move(config), stop](process::TaskReporter reporter) mutable noexcept {
                return createProject(
                    *execution,
                    *execution->blocking(),
                    directory,
                    std::move(config),
                    stop.stop_possible() ? stop : reporter.stopToken()
                );
            },
            [&output](process::TTaskResult<ProjectCreationResult, EditorFailure>&& result) noexcept {
                if (result)
                    output.emplace(std::move(*result));
                else if (auto* failure = result.error().domainFailure())
                    output.emplace(lux::cxx::unexpected(std::move(*failure)));
                else
                    output.emplace(lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "test.create"}));
            }
        );
    };
    auto config = ProjectBuilder(id(1), "Empty").build();
    assert(config && !config->initial_scene);
    assert(std::filesystem::is_empty(root)); // Builder has no filesystem side effects.
    assert(!ProjectBuilder({}, "Bad").build());
    std::optional<EditorResult<ProjectCreationResult>> result;
    auto operation = create(root / std::filesystem::u8path("空项目 with spaces"), *config, result);
    assert(operation);
    await([&] { return result.has_value(); });
    assert(*result);
    const auto project_file = (*result)->project_file;
    assert(std::filesystem::exists(project_file));
    assert(!std::filesystem::exists(project_file.parent_path() / "Content"));
    {
        auto source = readProjectOpenData(project_file);
        assert(source && source->manifest.assets.empty() && source->manifest.name == "Empty");
    }
    // An existing directory, including an empty one, never transfers ownership to this operation.
    std::filesystem::create_directory(root / "existing");
    {
        std::optional<EditorResult<ProjectCreationResult>> conflict;
        auto request = create(root / "existing", *config, conflict);
        assert(request);
        await([&] { return conflict.has_value(); });
        assert(!*conflict && conflict->error().domain == "project.create.conflict");
        assert(std::filesystem::is_empty(root / "existing"));
    }
    {
        std::stop_source cancelled;
        cancelled.request_stop();
        std::optional<EditorResult<ProjectCreationResult>> stopped;
        auto request = create(root / "cancelled", *config, stopped, cancelled.get_token());
        assert(request);
        await([&] { return stopped.has_value(); });
        assert(!*stopped && !std::filesystem::exists(root / "cancelled"));
    }
    simulation::SimulationDescriptionBuilder rules;
    auto simulation = std::move(rules).build();
    assert(simulation);
    scene::SceneDescriptionBuilder description;
    description.setWorld(id(4));
    description.setSimulation(id(5));
    auto scene_description = std::move(description).build();
    assert(scene_description);
    auto package = scene::createScenePackage(
        id(2),
        "Main",
        {},
        std::make_shared<const simulation::SimulationDescription>(std::move(*simulation)),
        *scene_description
    );
    assert(package);
    ProjectBuilder beginner(id(3), "Beginner");
    beginner.setInitialScene(
        {"我的包/Main.luxscene", "我的包/Main", std::make_shared<const scene::ScenePackage>(std::move(*package))}
    );
    auto built = std::move(beginner).build();
    assert(built);
    std::stop_source cancellation;
    std::optional<EditorResult<ProjectCreationResult>> created;
    auto request = create(root / "beginner", *built, created, cancellation.get_token());
    assert(request);
    // Hold business adoption until the durable result has been published.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!execution->taskInfo(request->id())->finished)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        const auto epoch = execution->wakeEpoch();
        execution->waitForWork(epoch, deadline);
    }
    // The task terminal was published after the writer returned. Inspecting files
    // while the journal is being removed races Windows delete-pending file handles.
    assert(std::filesystem::exists(root / "beginner/Project.luxproject"));
    assert(!std::filesystem::exists(root / "beginner/.lux-editor-publication"));
    assert(!created);
    cancellation.request_stop();
    await([&] { return created.has_value(); });
    assert(*created);
    {
        auto opened = readProjectOpenData((*created)->project_file);
        assert(opened && opened->manifest.assets.size() == 1);
        assert(opened->manifest.default_scene == "Content/我的包/Main.luxscene");
        assert(opened->manifest.assets.front().mount_path == "我的包/Main");
        const auto path = opened->file.parent_path() / std::filesystem::u8path(opened->manifest.default_scene);
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        assert(input);
        auto bytes = std::make_shared<std::vector<std::byte>>(std::size_t(input.tellg()));
        input.seekg(0);
        assert(input.read(reinterpret_cast<char*>(bytes->data()), bytes->size()));
        auto decoded = scene::decodeScenePackage(cxx::SharedBytes<>::fromOwner(bytes, *bytes));
        assert(decoded && decoded->scene->id() == id(2) && decoded->partitions.size() == 1);
        input.close();
        auto messages = object::ObjectMessageQueue::create(16);
        assert(messages);
        process::TaskScope tasks{*execution};
        asset::AssetVfs assets;
        auto storage = ProjectStorage::open(*opened, assets, *execution->blocking(), tasks, messages->dispatcherRef());
        assert(storage);
        auto captured = (*storage)->captureSource(id(2), 1024 * 1024, projectContentDigest(*bytes));
        assert(captured && captured->open(id(2)));
        {
            std::ofstream replacement(path, std::ios::binary | std::ios::app);
            replacement << "changed after capture";
            assert(replacement.good());
        }
        const auto changed = captured->open(id(2));
        assert(!changed && changed.error() == asset::EAssetStorageError::CONTENT_CHANGED);
        (*storage)->requestClose();
        await([&] {
            const auto closed = (*storage)->advanceClose();
            assert(closed);
            return *closed;
        });
        storage->reset();
        assert(tasks.join());
    }
    execution->requestStop();
    assert(execution->join());
}
